#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check repository manifests, patch headers and CI shell syntax on a host checkout."""
import importlib.util
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from repo_manifest import parse_manifest

ROOT = Path(__file__).resolve().parents[2]
KEXT_LIST = 'configs/k27-native-kexts.txt'
KEXT_TOOLS = Path('tooling/k27')
IMAGE_BUILD = KEXT_TOOLS / 'build-native-image.sh'
COMPILER = KEXT_TOOLS / 'build-native-kext.py'
SKIP_FILE = 'scripts/ci/self-tests.skip'

# The Ethernet companion builds from the same package as PianoWifi.
SOURCE_PACKAGES = {'PianoWifiEthernet': 'PianoWifi'}
# These two packages use UPSTREAM for their BSD license declaration.
LICENSE_IN_UPSTREAM = {'ARMPlatformStandIn', 'PianoNullEthernet'}
SHA = re.compile(r'[0-9a-f]{40}')
# The lock's remote sources use public GitHub HTTPS URLs without credentials.
# The separate pin-fetch CI job verifies that the repositories are reachable.
PUBLIC_GIT = re.compile(r'https://github\.com/[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9_.-]+', re.I)
URL = re.compile(r"""(?i)\b(?:[a-z][a-z0-9+.-]*://|www\.)[^\s<>"'\x60),;]+""")
LOCAL_REFERENCE = re.compile(
    r'''(?<![\w./-])(?:[~.]?/|\.\./|[A-Za-z]:[/\\]|\\\\|private/|\$(?:HOME|\{HOME\})[/\\])(?=[.\w])'''
    r'''|(?<![\w.-])(?:[\w-]+\.)*(?:[\w-]+\.(?:local|lan|home\.arpa|internal|private|localdomain)|localhost)(?![\w.-])'''
    r'''|(?<![\w.-])[\w.-]+@[\w.-]+:'''
    r'''|(?<![\w./-])(?:\.\.?|~)\\(?=\w)''',
    re.I,
)
CONFLICT = re.compile(r'^\+?[ \t]*(?:<{7,}|={7,}|>{7,})(?:[ \t]|$)')
HUNK = re.compile(r'^@@ -[0-9]+(?:,[0-9]+)? \+[0-9]+(?:,[0-9]+)? @@(?: |$)')
SKIP_REASON = re.compile(r'(?:ARGS|SUITE|INPUT|HEADER|KNOWN-FAIL):[ \t]*\S')


def read(root, path):
    try:
        return (root / path).read_text(encoding='utf-8')
    except (OSError, UnicodeError):
        raise ValueError(f'{path}: expected a readable UTF-8 file') from None


def public_git(value):
    return isinstance(value, str) and PUBLIC_GIT.fullmatch(value) and value.rsplit('/', 1)[1] not in ('.', '..')


def lock_text(value):
    """Yield decoded text including quoted keys and values in nested tables."""
    if isinstance(value, dict):
        yield from value
        for item in value.values():
            yield from lock_text(item)
    elif isinstance(value, list):
        for item in value:
            yield from lock_text(item)
    elif isinstance(value, str):
        yield value


def check_lock_references(text, label):
    for match in URL.finditer(text):
        if not public_git(match[0]):
            raise ValueError(f'{label}: use a public GitHub HTTPS repository URL')
    if LOCAL_REFERENCE.search(URL.sub(' ', text)):
        raise ValueError(f'{label}: local or private reference')


def pinned_sha(value, label):
    if not isinstance(value, str) or not SHA.fullmatch(value):
        raise ValueError(f'{label}: expected a 40-character lowercase hexadecimal SHA')


def validate_lock(contents):
    lock = parse_manifest(contents, 'unixnu.lock')
    # Check source lines as well as decoded strings to cover comments and TOML escapes.
    for lineno, line in enumerate(contents.splitlines(), 1):
        check_lock_references(line, f'unixnu.lock:{lineno}')
    for value in lock_text(lock):
        check_lock_references(value, 'unixnu.lock')
    for section in ('source', 'pin'):
        entries = lock.get(section)
        if not isinstance(entries, dict) or not entries:
            raise ValueError(f'unixnu.lock: expected [{section}.*] tables')
        for name, entry in entries.items():
            label = f'unixnu.lock [{section}.{name}]'
            if not isinstance(entry, dict):
                raise ValueError(f'{label}: expected a table')
            pinned_sha(entry.get('commit'), f'{label}.commit')
            if section == 'pin' or 'git' in entry:
                if not public_git(entry.get('git')):
                    raise ValueError(f'{label}.git: expected a public GitHub HTTPS repository URL')
            for field in ('tree', 'local'):
                if field in entry:
                    pinned_sha(entry[field], f'{label}.{field}')
            submodules = entry.get('submodules', {})
            if not isinstance(submodules, dict):
                raise ValueError(f'{label}.submodules: expected a table of commits')
            for submodule, commit in submodules.items():
                pinned_sha(commit, f'{label}.submodules.{submodule}')
    return lock


def validate_manifests(root):
    lock = validate_lock(read(root, 'unixnu.lock'))
    board = parse_manifest(read(root, 'board.toml'), 'board.toml')
    feeds = parse_manifest(read(root, 'feeds.toml'), 'feeds.toml')
    sources = feeds.get('source')
    if not isinstance(sources, list) or not sources or not all(isinstance(s, dict) for s in sources):
        raise ValueError('feeds.toml: expected [[source]] entries')
    kinds = {}
    for entry in sources:
        name, kind = entry.get('name'), entry.get('kind')
        if not isinstance(name, str) or not name or name in kinds or not isinstance(kind, str) or not kind:
            raise ValueError('feeds.toml: expected a unique source name and a kind')
        kinds[name] = kind
        if not public_git(entry.get('git')):
            raise ValueError(f'feeds.toml {name}.git: expected a public GitHub HTTPS repository URL')
        pinned_sha(entry.get('commit'), f'feeds.toml {name}.commit')
        pin = lock['source'].get(name, {})
        if entry['commit'] != pin.get('commit') or pin.get('git', entry['git']) != entry['git']:
            raise ValueError(f'feeds.toml {name}: source differs from unixnu.lock')
    table = board.get('board')
    soc = table.get('soc_source') if isinstance(table, dict) else None
    if not isinstance(soc, str) or kinds.get(soc) != 'soc':
        raise ValueError('board.toml [board].soc_source: expected a SoC source from feeds.toml')


def kext_names(root):
    names = []
    for lineno, line in enumerate(read(root, KEXT_LIST).splitlines(), 1):
        name = line.partition('#')[0].strip()
        if name:
            if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', name) or name in names:
                raise ValueError(f'{KEXT_LIST}:{lineno}: expected a unique kext name')
            names.append(name)
    if not names:
        raise ValueError(f'{KEXT_LIST}: empty inventory')
    return names


def json_pairs(pairs):
    obj = dict(pairs)
    if len(obj) != len(pairs):
        raise ValueError('duplicate JSON key')
    return obj


def invalid_json_constant(_):
    raise ValueError('invalid JSON constant')


def validate_descriptors(root, names):
    earlier = set()
    for name in names:
        path = KEXT_TOOLS / 'kext' / f'{name}.json'
        if not (root / path).is_file():
            raise ValueError(f'{path}: missing descriptor for {name}')
        try:
            desc = json.loads(read(root, path), object_pairs_hook=json_pairs, parse_constant=invalid_json_constant)
        except ValueError as exc:
            raise ValueError(f'{path}: {exc}') from None
        if not isinstance(desc, dict) or desc.get('name') != name:
            raise ValueError(f'{path}: descriptor name must match the inventory')
        bundle = desc.get('bundle')
        if not isinstance(bundle, str) or not bundle.strip() or bundle in earlier:
            raise ValueError(f'{path}: expected a non-empty, unique bundle ID')
        if desc.get('objects') != f'@OUT@/native-kexts/{name}/objects.json':
            raise ValueError(f'{path}: objects must point to the generated manifest for {name}')
        libraries = desc.get('libraries')
        if not isinstance(libraries, dict):
            raise ValueError(f'{path}: libraries must be a mapping of bundle IDs to versions')
        for library, version in libraries.items():
            if not library.strip() or not isinstance(version, str) or not version.strip():
                raise ValueError(f'{path}: expected a library bundle ID and version')
            if library == bundle or (library not in earlier and not library.startswith('com.apple.')):
                raise ValueError(f'{path}: library {library} must be provided earlier in {KEXT_LIST}')
        earlier.add(bundle)


def wrapper_sources(name, path, script):
    """Resolve the repository-rooted source arguments of the native build wrappers."""
    script = script.replace('\\\n', '')
    command = re.search(rf'^python3 "\$D/{re.escape(str(COMPILER))}" {re.escape(name)}[ \t]+(.+)$', script, re.M)
    if not command:
        raise ValueError(f'{path}: expected to compile {name} with {COMPILER}')

    aliases = {'D': Path('.')}
    for match in re.finditer(r'(?:^|;)[ \t]*([A-Z]+)=\$([A-Z]+)(/[\w./-]+)(?=[;\s]|$)', script, re.M):
        alias, parent, suffix = match.groups()
        if parent in aliases:
            aliases[alias] = aliases[parent] / suffix[1:]

    args = shlex.split(command[1], comments=True)
    if name == 'PianoA830' and '${SRC[@]}' in args:
        # This wrapper puts its inherited sources and board sources in a Bash array.
        for group in ('INHERITED', 'GEN8'):
            members = re.search(rf'^{group}="([^"]+)"$', script, re.M)
            if not members:
                raise ValueError(f'{path}: missing {group} source list')
            args.extend(f'$A/{member}.cpp' for member in members[1].split())
        for member_list in re.findall(r'^SRC\+=\((.*)\)$', script, re.M):
            args.extend(shlex.split(member_list))

    for arg in args:
        if Path(arg).suffix not in ('.c', '.cc', '.cpp', '.S'):
            continue
        source = re.fullmatch(r'\$([A-Z]+)/([^$]+)', arg)
        if source:
            if source[1] not in aliases:
                raise ValueError(f'{path}: unresolved source directory variable {source[1]}')
            relative = aliases[source[1]] / source[2]
        else:
            relative = Path(arg)
        if relative.is_absolute() or '..' in relative.parts or '$' in relative.as_posix():
            raise ValueError(f'{path}: expected a repository-relative source input')
        if any(part.casefold() == 'private' for part in relative.parts):
            raise ValueError(f'{path}: source {relative} references private checkout content')
        if relative.parts[0] == 'out':
            continue  # Generated sources need the image build inputs.
        # The GPU wrapper uses one brace expansion and a source glob.
        brace = re.search(r'\{([^{}]+)\}', relative.as_posix())
        if brace:
            for choice in brace[1].split(','):
                yield Path(relative.as_posix().replace(brace[0], choice))
        else:
            yield relative


def validate_kext_build(root, names):
    root = root.resolve()
    script = '\n'.join(line.strip() for line in read(root, IMAGE_BUILD).splitlines()
                       if not line.lstrip().startswith('#'))
    # The default LIST drives both the wrapper dispatch and descriptor staging, in inventory order.
    if not re.search(r'^LIST=\$\(.*"\$\{PIANO_KEXT_LIST:-' + re.escape(KEXT_LIST) + r'\}"', script, re.M):
        raise ValueError(f'{IMAGE_BUILD}: LIST must default to {KEXT_LIST}')
    if not re.search(r'^for k in \$LIST; do\b[^\n]*"tooling/k27/kext/\$k\.json"', script, re.M):
        raise ValueError(f'{IMAGE_BUILD}: expected to stage descriptors from LIST')
    dispatch = re.search(r'^for k in \$LIST; do\b.*?^done$', script, re.M | re.S)
    loop = dispatch[0] if dispatch else ''
    # The first case in this loop is the codeless skip. The b= arms map the compiled kexts.
    cases = re.findall(r'^case \$k in (.+?) esac\b', loop, re.M)
    wrappers = dict(re.findall(r'\b([A-Za-z][A-Za-z0-9_]*)\) b=([a-z0-9-]+) ;;', ' '.join(cases)))
    if 'tooling/k27/build-native-$b.sh ' not in loop or not (root / COMPILER).is_file():
        raise ValueError(f'{IMAGE_BUILD}: missing the native build wrapper dispatch or shared compiler')
    for name in names:
        if name not in wrappers:
            raise ValueError(f'{IMAGE_BUILD}: no native build wrapper for {name}')
        path = KEXT_TOOLS / f'build-native-{wrappers[name]}.sh'
        package = Path('package') / SOURCE_PACKAGES.get(name, name)
        upstream = read(root, package / 'UPSTREAM')
        if not upstream.strip():
            raise ValueError(f'{package}/UPSTREAM: expected source provenance')
        if package.name in LICENSE_IN_UPSTREAM:
            if 'BSD-2-Clause-Patent' not in upstream:
                raise ValueError(f'{package}/UPSTREAM: expected the license declaration')
        elif not (root / package / 'LICENSE').is_file():
            raise ValueError(f'{package}/LICENSE: missing package license')

        if not os.access(root / path, os.X_OK) or not (root / path).is_file():
            raise ValueError(f'{path}: missing executable build wrapper')
        sources = set(wrapper_sources(name, path, read(root, path)))
        if not any(package in source.parents for source in sources):
            raise ValueError(f'{path}: expected a source in {package}')
        for source in sorted(sources):
            found = False
            for match in root.glob(source.as_posix()):
                resolved = match.resolve()
                if not resolved.is_relative_to(root) or any(
                    part.casefold() == 'private' for part in resolved.relative_to(root).parts
                ):
                    raise ValueError(f'{path}: source {match.relative_to(root)} must resolve inside the public checkout')
                if match.is_file():
                    found = True
            if not found:
                raise ValueError(f'{path}: missing source {source}')


def validate_patches(root):
    paths = sorted((root / 'patches').rglob('*.patch'))
    if not paths:
        raise ValueError('patches/: expected at least one patch')
    prefixes = {}
    for full in paths:
        path = full.relative_to(root)
        # The build applies sorted globs. Four digits keep lexical and numeric order the same.
        prefix = re.match(r'^([0-9]{4})-.+\.patch$', path.name)
        if not prefix:
            raise ValueError(f'{path}: expected a four-digit patch prefix')
        key = (path.parent, prefix[1])
        if key in prefixes:
            raise ValueError(f'{path}: duplicate patch prefix {prefix[1]} ({prefixes[key]})')
        prefixes[key] = path.name
        contents = read(root, path)
        if not contents.strip():
            raise ValueError(f'{path}: empty patch')
        lines = contents.splitlines()
        for lineno, line in enumerate(lines, 1):
            if CONFLICT.match(line):
                raise ValueError(f'{path}:{lineno}: unresolved conflict marker')
        # Check each git file diff separately. A plain unified diff has no git preamble.
        starts = [i for i, line in enumerate(lines) if line.startswith('diff --git ')] or [0]
        for start, end in zip(starts, starts[1:] + [len(lines)]):
            header = next((i for i in range(start, end) if lines[i].startswith(('--- ', '+++ ', '@@ '))), end)
            if (header + 2 >= end or not re.match(r'^--- \S', lines[header])
                    or not re.match(r'^\+\+\+ \S', lines[header + 1]) or not HUNK.match(lines[header + 2])):
                raise ValueError(f'{path}:{start + 1}: expected --- / +++ unified diff headers followed by a hunk')


def skip_entries(root):
    paths = set()
    for lineno, line in enumerate(read(root, SKIP_FILE).splitlines(), 1):
        entry, _, reason = line.partition('#')
        entry = entry.strip()
        if not entry:
            continue
        path = Path(entry)
        label = f'{SKIP_FILE}:{lineno}'
        if path.is_absolute() or '..' in path.parts or not (root / path).is_file():
            raise ValueError(f'{label}: skip path must name an existing repository file')
        if path in paths:
            raise ValueError(f'{label}: duplicate skip path {path}')
        if not SKIP_REASON.match(reason.strip()):
            raise ValueError(f'{label}: expected a documented reason class and explanation')
        paths.add(path)
    return paths


def shell_tests():
    # Use the runner's discovery rules, with skips still present in collect().
    spec = importlib.util.spec_from_file_location('self_tests', ROOT / 'scripts/ci/self-tests.py')
    runner = importlib.util.module_from_spec(spec)
    saved = sys.dont_write_bytecode
    try:
        sys.dont_write_bytecode = True
        spec.loader.exec_module(runner)
    finally:
        sys.dont_write_bytecode = saved
    tests, _ = runner.collect(None, runner.load_skips())
    return {Path(path) for kind, path, _ in tests if kind == 'sh'}


def check_shell_syntax(root, path):
    env = {k: v for k, v in os.environ.items() if k != 'BASH_ENV'}
    result = subprocess.run(['bash', '-n', '--', str(path)], cwd=root, env=env, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f'{path}: bash -n failed\n{result.stderr.strip()}')


class RepositoryManifests(unittest.TestCase):
    def test_native_kexts(self):
        names = kext_names(ROOT)
        validate_descriptors(ROOT, names)
        validate_kext_build(ROOT, names)

    def test_manifests(self):
        validate_manifests(ROOT)

    def test_patches(self):
        validate_patches(ROOT)

    def test_ci_shell_scripts(self):
        skipped = {path for path in skip_entries(ROOT) if path.suffix == '.sh'}
        for path in sorted(shell_tests() | skipped):
            with self.subTest(path=path):
                check_shell_syntax(ROOT, path)


class Regressions(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def write(self, path, contents):
        path = self.root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding='utf-8')

    def write_descriptor(self, name, libraries=None):
        contents = dict(name=name, bundle=f'org.example.{name}', libraries=libraries or {},
                        objects=f'@OUT@/native-kexts/{name}/objects.json')
        self.write(KEXT_TOOLS / 'kext' / f'{name}.json', json.dumps(contents))

    def test_missing_descriptor(self):
        with self.assertRaisesRegex(ValueError, 'missing descriptor for Example'):
            validate_descriptors(self.root, ['Example'])

    def test_library_order(self):
        self.write_descriptor('Provider')
        self.write_descriptor('Consumer', {'org.example.Provider': '1.0', 'com.apple.kpi.iokit': '18.0'})
        validate_descriptors(self.root, ['Provider', 'Consumer'])
        with self.assertRaisesRegex(ValueError, 'library org.example.Provider must be provided earlier'):
            validate_descriptors(self.root, ['Consumer', 'Provider'])

    def test_native_build_requires_the_declared_source(self):
        self.write(IMAGE_BUILD,
                   'LIST=$(cat "${PIANO_KEXT_LIST:-configs/k27-native-kexts.txt}")\n'
                   'for k in $LIST; do\n'
                   '  case $k in Example) b=example ;; *) b= ;; esac\n'
                   '  tooling/k27/build-native-$b.sh > build.log\n'
                   'done\n'
                   'for k in $LIST; do cat "tooling/k27/kext/$k.json"; done\n')
        self.write(COMPILER, '# stub compiler\n')
        wrapper = KEXT_TOOLS / 'build-native-example.sh'
        self.write(wrapper,
                   'P=$D/package/Example\n'
                   'python3 "$D/tooling/k27/build-native-kext.py" Example "$P/Example.cpp"\n')
        (self.root / wrapper).chmod(0o755)
        package = Path('package/Example')
        self.write(package / 'UPSTREAM', 'Example source provenance\n')
        self.write(package / 'LICENSE', 'Example license\n')
        source = package / 'Example.cpp'
        self.write(source, '// Example source\n')
        validate_kext_build(self.root, ['Example'])
        (self.root / source).unlink()
        with self.assertRaisesRegex(ValueError, r'missing source package/Example/Example\.cpp'):
            validate_kext_build(self.root, ['Example'])

    def test_native_wrapper_refuses_private_source(self):
        script = 'python3 "$D/tooling/k27/build-native-kext.py" Example "$D/private/example.cc"'
        with self.assertRaisesRegex(ValueError, 'references private checkout content'):
            list(wrapper_sources('Example', 'fixture.sh', script))

    def test_bad_lock_shas(self):
        good = 'a' * 40
        bad = 'F' * 40
        source = f'[source.example]\ncommit = "{good}"\n'
        pin = f'[pin.example]\ngit = "https://github.com/example/repo"\ncommit = "{good}"\n'
        validate_lock(source + pin)
        with self.assertRaisesRegex(ValueError, r'\[source\.example\]\.commit: expected a 40-character'):
            validate_lock(source.replace(good, bad) + pin)
        with self.assertRaisesRegex(ValueError, r'\.submodules\.kit: expected a 40-character'):
            validate_lock(source + pin + f'submodules = {{kit = "{bad}"}}\n')

    def test_lock_local_and_private_references(self):
        pin = 'a' * 40
        lock = f'[source.example]\ncommit = "{pin}"\n[pin.example]\ncommit = "{pin}"\n'
        with self.assertRaisesRegex(ValueError, 'public GitHub HTTPS'):
            validate_lock(lock + 'git = "https://example.invalid/project"\n')
        with self.assertRaisesRegex(ValueError, 'local or private reference'):
            validate_lock(lock + 'git = "https://github.com/example/repo"\nrole = "checkout ~/example"\n')

    def test_manifest_rejects_bad_toml(self):
        for contents in ('x = ["a" "b"]', 'x = {a = "value",}', '[example]\n[example]', '[[source] ]',
                         'x = true\nx = false', 'x = {name = "a"}\n[x]', 'x = "bad\\/escape"'):
            with self.subTest(contents=contents), self.assertRaisesRegex(ValueError, 'example.toml:'):
                parse_manifest(contents, 'example.toml')

    def test_manifest_accepts_repository_toml_forms(self):
        contents = '''
"schema_version" = 2
["board"."detail"]
values = ["text # kept", true, -2, {kit = 'test'},] # comment
[[source]]
name = "one"
[[source]]
name = "two"
'''
        self.assertEqual(parse_manifest(contents, 'example.toml'), {
            'schema_version': 2,
            'board': {'detail': {'values': ['text # kept', True, -2, {'kit': 'test'}]}},
            'source': [{'name': 'one'}, {'name': 'two'}],
        })

    def test_duplicate_patch_prefix(self):
        diff = '--- a/file\n+++ b/file\n@@ -1 +1 @@\n-old\n+new\n'
        self.write('patches/first/0001-one.patch', diff)
        self.write('patches/second/0001-two.patch', diff)
        validate_patches(self.root)
        self.write('patches/first/0001-three.patch', diff)
        with self.assertRaisesRegex(ValueError, 'duplicate patch prefix 0001'):
            validate_patches(self.root)

    def test_patch_conflict_markers(self):
        path = 'patches/0001-example.patch'
        diff = '--- a/file\n+++ b/file\n@@ -1 +1 @@\n-old\n'
        for marker in ('+<<<<<<< branch', '+=======', '>>>>>>> branch'):
            with self.subTest(marker=marker):
                self.write(path, diff + marker + '\n')
                with self.assertRaisesRegex(ValueError, 'unresolved conflict marker'):
                    validate_patches(self.root)

    def test_patch_requires_headers_in_each_git_section(self):
        self.write('patches/0001-example.patch',
                   'diff --git a/one b/one\n--- a/one\n+++ b/one\n@@ -1 +1 @@\n-old\n+new\n'
                   'diff --git a/two b/two\n--- a/two\n@@ -1 +1 @@\n-old\n+new\n')
        with self.assertRaisesRegex(ValueError, 'expected --- / \\+\\+\\+ unified diff headers'):
            validate_patches(self.root)

    def test_skip_reason_and_bash_syntax(self):
        path = Path('scripts/test-example.sh')
        self.write(path, 'if then\n')
        self.write(SKIP_FILE, f'{path} # needs arguments\n')
        with self.assertRaisesRegex(ValueError, 'expected a documented reason class'):
            skip_entries(self.root)
        self.write(SKIP_FILE, f'{path} # ARGS: takes a fixture\n')
        self.assertEqual(skip_entries(self.root), {path})
        with self.assertRaisesRegex(ValueError, 'bash -n failed'):
            check_shell_syntax(self.root, path)
        (self.root / path).unlink()
        with self.assertRaisesRegex(ValueError, 'must name an existing repository file'):
            skip_entries(self.root)


if __name__ == '__main__':
    unittest.main()
