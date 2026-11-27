"""Exercise the session against a temporary SQLite database and mocked processes."""

from contextlib import closing, ExitStack
import os
from pathlib import Path
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import run


ACCESS_SCHEMA = """
CREATE TABLE access (
    service TEXT NOT NULL,
    client TEXT NOT NULL,
    client_type INTEGER NOT NULL,
    auth_value INTEGER NOT NULL,
    auth_reason INTEGER NOT NULL,
    auth_version INTEGER NOT NULL,
    csreq BLOB,
    policy_id INTEGER,
    indirect_object_identifier_type INTEGER,
    indirect_object_identifier TEXT NOT NULL DEFAULT 'UNUSED',
    indirect_object_code_identity BLOB,
    flags INTEGER,
    last_modified INTEGER NOT NULL DEFAULT (CAST(strftime('%s', 'now') AS INTEGER)),
    pid INTEGER,
    pid_version INTEGER,
    boot_uuid TEXT NOT NULL DEFAULT 'UNUSED',
    last_reminded INTEGER NOT NULL DEFAULT (CAST(strftime('%s', 'now') AS INTEGER)),
    PRIMARY KEY (service, client, client_type, indirect_object_identifier)
);
"""

REQUIREMENT = b"\x00\x01\xa2\xff"
BOOT = "00000000-0000-0000-0000-000000000001"
SERVICE = "kTCCServiceAccessibility"
WRAPPER = "/usr/libexec/sshd-keygen-wrapper"


class SessionPolicy(unittest.TestCase):
    def setUp(self):
        sqlite_cli = shutil.which("sqlite3")
        if not sqlite_cli:
            self.skipTest("the session requires the sqlite3 CLI")
        fixture = tempfile.TemporaryDirectory()
        self.addCleanup(fixture.cleanup)
        root = Path(fixture.name)
        self.db = root / "TCC.db"
        self.session = root / "session"
        self.session.mkdir()
        self.events = root / "events"
        self.events.touch()
        self.script = self.session / "session.sh"
        # Keep the transaction and cleanup code intact. Replace only the production path
        # and root/dir guard so the copied script can run against an unprivileged fixture.
        source = (TOOLS / "session.sh").read_text()
        overrides = {
            '[[ $EUID == 0 && $uid =~ ^[0-9]+$ && $uid -ge 501 && '
            '$dir =~ ^/private/var/run/piano-gui\\.[[:xdigit:]]{32}$ ]]':
                '[[ $uid == 501 && $dir == "$FIXTURE_SESSION" ]]',
            "db='/Library/Application Support/com.apple.TCC/TCC.db'": 'db=$FIXTURE_DB',
        }
        for original, replacement in overrides.items():
            self.assertEqual(source.count(original), 1)
            source = source.replace(original, replacement)
        self.script.write_text(source)
        mocks = root / "mocks.bash"
        mocks.write_text(r"""
sysctl() {
    case "$2" in
        kern.bootsessionuuid) echo "$FIXTURE_BOOT" ;;
        kern.bootargs) echo 'data-persist=2' ;;
        *) return 2 ;;
    esac
}
stat() { [[ $* == '-f %u /dev/console' ]] && echo 501; }
pgrep() {
    case "$*" in
        '-u 501 -x Finder') echo 712 ;;
        '-u 501 -x tccd') echo 813 ;;
        *) return 2 ;;
    esac
}
launchctl() { [[ $* == 'print gui/501' ]]; }
ioreg() { echo IOConsoleUsers; }
csreq() { [[ $1 == -r- && $2 == -b ]] && printf '\x00\x01\xa2\xff' > "$3"; }
kill() { printf '%s\n' "$*" >> "$FIXTURE_EVENTS"; }
sqlite3() { command "$FIXTURE_SQLITE" -init /dev/null "$@"; }
""")
        self.env = dict(os.environ, BASH_ENV=str(mocks), FIXTURE_DB=str(self.db),
                        FIXTURE_SESSION=str(self.session), FIXTURE_BOOT=BOOT,
                        FIXTURE_EVENTS=str(self.events), FIXTURE_SQLITE=sqlite_cli)
        with closing(sqlite3.connect(self.db)) as db, db:
            db.executescript(ACCESS_SCHEMA)

    def add_policy(self, auth, requirement, client=WRAPPER, indirect="UNUSED"):
        with closing(sqlite3.connect(self.db)) as db, db:
            db.execute("INSERT INTO access VALUES (?, ?, 1, ?, 7, 3, ?, 17, 0, ?, ?, 91, 101, 19, 2, ?, 102)",
                       (SERVICE, client, auth, requirement, indirect, b"\x00\xff", "fixture-boot"))

    def invoke(self, op, status, receipt):
        result = subprocess.run(["/bin/bash", self.script, f"{op}-locked", BOOT, "501"],
                                env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines(), [receipt])
        self.assertEqual(result.stderr, "")

    def rows(self):
        with closing(sqlite3.connect(self.db)) as db:
            return db.execute("SELECT rowid, * FROM access ORDER BY rowid").fetchall()

    def test_missing_policy_seed_is_owned_and_cleaned_up(self):
        self.add_policy(0, b"unrelated", client="unrelated-client")
        original = self.rows()
        self.invoke("seed", 0, "GUI_TCC_SEEDED")
        owner = self.session / "seed-owner"
        self.assertEqual(stat.S_IMODE(owner.stat().st_mode), 0o600)
        row, stamp, requirement = owner.read_text().split()
        self.assertEqual(requirement, REQUIREMENT.hex())
        rows = self.rows()
        self.assertEqual(rows[:1], original)
        self.assertEqual(rows[1][:8], (int(row), SERVICE, WRAPPER, 1, 2, 4, 1, REQUIREMENT))
        self.assertEqual(rows[1][12:14], (0, int(stamp)))
        self.assertEqual(self.events.read_text().splitlines(), ["-KILL 813"])
        self.invoke("cleanup", 0, "GUI_CLEANUP_OK")
        self.assertEqual(self.rows(), original)
        self.assertEqual(self.events.read_text().splitlines(), ["-KILL 813", "-KILL 813"])
        self.assertFalse(self.session.exists())

    def test_exact_existing_grant_survives_seed_and_cleanup_unchanged(self):
        self.add_policy(2, REQUIREMENT)
        original = self.db.read_bytes()
        self.invoke("seed", 0, "GUI_TCC_ALREADY_TRUSTED")
        mode = self.session / "seed-existing"
        self.assertEqual(mode.read_text(), "existing-unchanged\n")
        self.assertEqual(stat.S_IMODE(mode.stat().st_mode), 0o600)
        self.assertFalse((self.session / "seed-owner").exists())
        self.assertEqual(self.db.read_bytes(), original)
        self.invoke("cleanup", 0, "GUI_CLEANUP_OK")
        self.assertEqual(self.db.read_bytes(), original)
        self.assertEqual(self.events.read_text(), "")
        self.assertFalse(self.session.exists())

    def assert_conflicting_policy_preserved(self, auth, requirement):
        self.add_policy(auth, requirement)
        original = self.db.read_bytes()
        self.invoke("seed", 2, "GUI_TCC_EXISTING_POLICY")
        self.assertFalse((self.session / "seed-owner").exists())
        self.assertFalse((self.session / "seed-existing").exists())
        self.assertEqual(self.db.read_bytes(), original)
        self.invoke("cleanup", 0, "GUI_CLEANUP_OK")
        self.assertEqual(self.db.read_bytes(), original)
        self.assertEqual(self.events.read_text(), "")
        self.assertFalse(self.session.exists())

    def test_denied_policy_survives_seed_and_cleanup_unchanged(self):
        self.assert_conflicting_policy_preserved(0, REQUIREMENT)

    def test_stale_requirement_survives_seed_and_cleanup_unchanged(self):
        self.assert_conflicting_policy_preserved(2, b"stale")

    def test_null_requirement_survives_seed_and_cleanup_unchanged(self):
        self.assert_conflicting_policy_preserved(2, None)

    def test_multiple_matching_rows_are_ambiguous(self):
        self.add_policy(2, REQUIREMENT)
        self.add_policy(0, REQUIREMENT, indirect="fixture-indirect-object")
        original = self.db.read_bytes()
        self.invoke("seed", 2, "GUI_TCC_EXISTING_POLICY")
        self.invoke("cleanup", 0, "GUI_CLEANUP_OK")
        self.assertEqual(self.db.read_bytes(), original)


class RunnerPolicyReceipts(unittest.TestCase):
    def setUp(self):
        stack = ExitStack()
        self.addCleanup(stack.close)
        self.results = Path(stack.enter_context(tempfile.TemporaryDirectory()))
        stack.enter_context(patch.dict(os.environ, GUI_RESULTS=str(self.results)))
        self.verdict = stack.enter_context(patch.object(run, "verdict"))

    @staticmethod
    def reply(receipt, status=0):
        return subprocess.CompletedProcess([], status, b"synthetic-private-text\n" + receipt + b"\n")

    def test_existing_trust_probe_records_only_boolean(self):
        reply = self.reply(b'GUI {"trusted":true,"extra":"synthetic-private-text"}')
        with patch.object(run, "bash", return_value=reply) as transport:
            run.prepare_ax()
        transport.assert_called_once()
        self.assertEqual((self.results / "ax-trust.txt").read_bytes(), b'GUI {"trusted":true}\n')
        self.assertFalse((self.results / "ax-seed.txt").exists())
        self.verdict.assert_called_once_with("INFO", "gui-checks console-user Accessibility was already trusted")

    def test_seed_success_requires_trust_probe_and_records_only_status(self):
        for state, summary in (
            (b"GUI_TCC_SEEDED", "temporary Accessibility grant inserted"),
            (b"GUI_TCC_ALREADY_TRUSTED", "existing Accessibility grant matched"),
        ):
            with self.subTest(state=state):
                replies = [self.reply(b'GUI {"trusted":false}'), self.reply(state),
                           self.reply(b'GUI {"trusted":true}')]
                with patch.object(run, "bash", side_effect=replies) as transport:
                    run.prepare_ax()
                self.assertEqual([call.args[1] for call in transport.call_args_list],
                                 ["ax-trust", "seed", "ax-trust"])
                self.assertEqual((self.results / "ax-seed.txt").read_bytes(), state + b"\n")
                self.verdict.assert_called_with("INFO", f"gui-checks {summary}")

    def test_conflicting_policy_reports_authorization_required(self):
        state = b"GUI_TCC_EXISTING_POLICY"
        replies = [self.reply(b'GUI {"trusted":false}'), self.reply(state, 2)]
        with patch.object(run, "bash", side_effect=replies):
            with self.assertRaisesRegex(run.CheckError, "policy requires authorization"):
                run.prepare_ax()
        self.assertEqual((self.results / "ax-seed.txt").read_bytes(), state + b"\n")
        self.verdict.assert_called_once_with("INFO", "gui-checks existing Accessibility policy differs")

    def test_matching_policy_receipt_cannot_substitute_for_actual_trust(self):
        untrusted = self.reply(b'GUI {"trusted":false}')
        replies = [untrusted, self.reply(b"GUI_TCC_ALREADY_TRUSTED"), *([untrusted] * 5)]
        with (patch.object(run, "bash", side_effect=replies), patch.object(run.time, "sleep")):
            with self.assertRaisesRegex(run.CheckError, "probe did not become trusted"):
                run.prepare_ax()
        self.assertEqual((self.results / "ax-trust.txt").read_bytes(), b'GUI {"trusted":false}\n')

    def test_ambiguous_or_failed_seed_receipts_are_rejected(self):
        for result in (self.reply(b"GUI_TCC_SEEDED\nGUI_TCC_ALREADY_TRUSTED"),
                       self.reply(b"GUI_TCC_ALREADY_TRUSTED", 2)):
            with self.subTest(result=result):
                with patch.object(run, "bash", side_effect=[self.reply(b'GUI {"trusted":false}'), result]):
                    with self.assertRaisesRegex(run.CheckError, "could not confirm"):
                        run.prepare_ax()
        self.assertFalse((self.results / "ax-seed.txt").exists())
        self.verdict.assert_not_called()
