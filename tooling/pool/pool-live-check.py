#!/usr/bin/env python3
"""T-20 v2 R1b: the live old System (OS) and SystemOS volumes of the running
installed system compared by content with the manifests of the quiesced capture (the archives' expected.manifest), on the host; the
tablet only runs `pool-manifest --paths LIST /` (no python there).

  pool-live-check.py list    --systemos SYS.manifest --os OS.manifest --mounts MOUNT.txt --out LIST
  pool-live-check.py compare --systemos SYS.manifest --os OS.manifest --mounts MOUNT.txt --list LIST --live LIVE [--json OUT]

`/` is the owner chain's union: SystemOS mounted read-only with MNT_UNION over the OS root (kern_exec.c:8165-8226); a name missing from a
SystemOS directory is looked up in the covered OS directory at the same path, at any depth (vfs_lookup.c:1736-1761); the top wins for a
name both have. `list` writes the paths that view shows (TAG<TAB>PATH, tag s = SystemOS, o = OS, each layer in its manifest's order);
`compare` reads the tablet's output for that list. Compared exactly: type, mode, uid, gid, flags, size, mtime, content sha256, link
target, xattrs, ACL, decmpfs type:size, hard links. Tolerated (PLAN-v2.md T1-T7; each counted in the report):
  T1 APFS metadata: not in a manifest (raw bytes, checkpoints, inode numbers, st_dev, ctime/atime/birthtime)
  T2 the ten k27 firmlink sources and everything below (a lookup goes on into Data)
  T3 every live mount point but `/` (MOUNT.txt: `mount` of that boot) and everything below
  T4 OS entries the union cannot show: a name SystemOS also has; anything below a path that is no directory in the merged view
  T5 .fseventsd at a volume's root
  T6 a directory listing may also show the other layer's names (a name in neither layer is an added entry: DIFFERENT)
  T7 the hard-link field among the visible members of each layer's group
Also required: both old volumes mounted on `/` read-only (MOUNT.txt). Exit 0 EQUAL, 1 DIFFERENT, 2 VOID (an incomplete or failed live
run, a malformed input).
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

FIRMLINKS = ('private', 'Applications', 'Library', 'Users', 'Volumes', 'cores', 'opt', 'usr/local', 'System/Library/Caches',
             'System/Volumes/Hardware')
FIELDS = ('path', 'type', 'mode', 'uid', 'gid', 'flags', 'size', 'mtime', 'data', 'link', 'hardlink', 'xattrs', 'acl', 'compression')
SUMMARY = re.compile(r'# entries (\d{1,20}) files \d{1,20} bytes \d{1,20}(?: missing (\d{1,20}))? sha256 ([0-9a-f]{64}) errors 0')


def escape(text):
    """pool-manifest's path escaping: bytes outside 0x21-0x7e and the backslash as \\xHH."""
    return ''.join(chr(b) if 0x20 < b < 0x7f and b != 0x5c else f'\\x{b:02x}' for b in text.encode())


def manifest_lines(path, *, live=False):
    raw = Path(path).read_bytes()
    body, separator, footer = raw[:-1].rpartition(b'\n')
    summary = footer.decode(errors='surrogateescape')
    match = SUMMARY.fullmatch(summary)
    if not match or not raw.endswith(b'\n') or (match[2] is not None) != live:
        raise SystemExit(f'VOID: {path}: no clean final summary line')
    if hashlib.sha256(body + separator).hexdigest() != match[3]:
        raise SystemExit(f'VOID: {path}: manifest SHA-256 does not match its final summary')
    return body.decode(errors='surrogateescape').splitlines(), summary, int(match[1]), int(match[2] or 0)


def load_manifest(path):
    """[(path, fields)] in file order, with one record per path in a complete manifest."""
    lines, _, entries, _ = manifest_lines(path)
    rows, paths = [], set()
    for line in lines:
        f = line.split('\t')
        if len(f) != 14:
            raise SystemExit(f'VOID: {path}: a line of {len(f)} fields')
        if f[0] in paths:
            raise SystemExit(f'VOID: {path}: repeated path {f[0]}')
        paths.add(f[0])
        rows.append((f[0], f))
    if len(rows) != entries or not any(p == '.' and f[1] == 'd' for p, f in rows):
        raise SystemExit(f'VOID: {path}: incomplete source manifest')
    return rows


def mounts(path):
    """(the mount points other than `/`, escaped and relative; the `/` lines)."""
    points, roots = [], []
    for line in Path(path).read_text().splitlines():
        m = re.match(r'^(.+?) on (.+) \(([^()]*)\)$', line)
        if not m:
            continue
        where = m.group(2)
        if where == '/':
            roots.append(line)
        elif where.startswith('/'):
            points.append(escape(where[1:]))
    return points, roots


def parent(p):
    return '.' if '/' not in p else p.rsplit('/', 1)[0]


def ancestors(p):
    if p == '.':
        return []
    parts = p.split('/')
    return ['.'] + ['/'.join(parts[:i]) for i in range(1, len(parts))]


class View:
    def __init__(self, a):
        self.layers = {'s': load_manifest(a.systemos), 'o': load_manifest(a.os)}
        self.by = {t: dict(rows) for t, rows in self.layers.items()}
        self.points, self.roots = mounts(a.mounts)
        self.excl = [(x, 'T2') for x in FIRMLINKS] + [(x, 'T3') for x in self.points]
        self.children = {t: {} for t in self.layers}
        for t, rows in self.layers.items():
            for p, _ in rows:
                if p != '.':
                    self.children[t].setdefault(parent(p), set()).add(p.rsplit('/', 1)[-1])

    def eff(self, p):
        return self.by['s'].get(p) or self.by['o'].get(p)

    def why_hidden(self, tag, p):
        if p == '.fseventsd' or p.startswith('.fseventsd/'):
            return 'T5'
        for x, why in self.excl:
            if p == x or p.startswith(x + '/'):
                return why
        if tag == 'o' and p in self.by['s']:
            return 'T4'
        for q in ancestors(p):
            e = self.eff(q)
            if e is None or e[1] != 'd':
                return 'T4'
        return None

    def listing(self):
        out, hidden = [], {}
        for tag in ('s', 'o'):
            for p, _ in self.layers[tag]:
                why = self.why_hidden(tag, p)
                if why:
                    hidden[why] = hidden.get(why, 0) + 1
                else:
                    out.append((tag, p))
        return out, hidden


def cmd_list(a):
    v = View(a)
    rows, hidden = v.listing()
    Path(a.out).write_text(''.join(f'{t}\t{p}\n' for t, p in rows))
    print(f'list: {len(rows)} paths (systemos {sum(1 for t, _ in rows if t == "s")}, os {sum(1 for t, _ in rows if t == "o")}); '
          f'hidden {json.dumps(hidden, sort_keys=True)}; mount points {len(v.points)}')
    return 0


def cmd_compare(a):
    v = View(a)
    want = [tuple(line.split('\t', 1)) for line in Path(a.list).read_text().splitlines() if line]
    rows, hidden = v.listing()
    if want != rows:
        raise SystemExit('VOID: the list is not the one these manifests and mounts give (made from other inputs)')
    live, ls, missing = {}, {}, {}
    lines, summary, entries, missing_count = manifest_lines(a.live, live=True)
    for line in lines:
        f = line.split('\t')
        if len(f) == 4 and f[0] == '#ls':
            ls.setdefault((f[1], f[2]), set()).add(f[3])
            continue
        if len(f) == 4 and f[0] == '#missing':
            key, store, value = (f[1], f[2]), missing, f[3]
        elif len(f) == 15:
            key, store, value = (f[0], f[1]), live, f[1:]
        else:
            raise SystemExit(f'VOID: {a.live}: malformed live record')
        if key in live or key in missing:
            raise SystemExit(f'VOID: {a.live}: repeated live path {key}')
        store[key] = value
    if len(live) != entries or len(missing) != missing_count:
        raise SystemExit(f'VOID: {a.live}: live counts disagree with its final summary')
    counts, samples = {}, []

    def bad(kind, text):
        counts[kind] = counts.get(kind, 0) + 1
        if len(samples) < 20:
            samples.append(f'{kind}: {text}')

    ro = [r for r in v.roots if 'read-only' in r]
    if len(v.roots) != 2 or len(ro) != 2:
        bad('root-not-read-only', f'{len(v.roots)} mounts on / ({len(ro)} read-only); both old volumes must be read-only')
    # T7: the expected hard-link field among the visible members, in list order per layer
    first = {}
    for tag, p in rows:
        exp = v.by[tag][p]
        g = (tag, exp[10] if exp[10] != '-' else p)
        hl = '-'
        if exp[1] == 'f':
            if g in first:
                hl = first[g]
            else:
                first[g] = p
        if (tag, p) in missing:
            bad('missing', f'{tag} {p} (errno {missing[(tag, p)]})')
            continue
        got = live.get((tag, p))
        if got is None:
            bad('not-in-live-output', f'{tag} {p}')
            continue
        for k in range(1, 14):
            e = hl if k == 10 else exp[k]
            g2 = got[k]
            if g2 != e:
                bad(FIELDS[k], f'{tag} {p}: {e} -> {g2}')
        if exp[1] == 'd':
            names = ls.get((tag, p), set())
            own = v.children[tag].get(p, set())
            other = v.children['o' if tag == 's' else 's'].get(p, set())
            if p == '.':
                own = own - {'.fseventsd'}
                other = other | {'.fseventsd'}
            for n in sorted(own - names):
                bad('not-listed', f'{tag} {p}/{n}')
            for n in sorted(names - own - other):
                bad('added', f'{tag} {p}/{n}')
            tolerated = len(names & other - own)
            if tolerated:
                hidden['T6'] = hidden.get('T6', 0) + tolerated
    extra = set(live) - set(rows)
    for t, p in sorted(extra)[:5]:
        bad('unlisted-in-output', f'{t} {p}')
    verdict = 'DIFFERENT' if counts else 'EQUAL'
    line = (f'{verdict}: {len(rows)} live paths compared (systemos {sum(1 for t, _ in rows if t == "s")}, os '
            f'{sum(1 for t, _ in rows if t == "o")}); differences {json.dumps(counts, sort_keys=True)}; tolerated '
            f'{json.dumps(hidden, sort_keys=True)}')
    print(line)
    for s_ in samples:
        print('  ' + s_)
    print(f'  live: {summary}')
    if a.json:
        Path(a.json).write_text(json.dumps({'verdict': verdict, 'compared': len(rows), 'differences': counts, 'tolerated': hidden,
                                            'samples': samples, 'live_summary': summary}, indent=1) + '\n')
    return 1 if counts else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=('check-manifests', 'list', 'compare'))
    ap.add_argument('--systemos', required=True)
    ap.add_argument('--os', required=True)
    ap.add_argument('--mounts')
    ap.add_argument('--out')
    ap.add_argument('--list')
    ap.add_argument('--live')
    ap.add_argument('--json')
    a = ap.parse_args()
    try:
        if a.mode == 'check-manifests':
            load_manifest(a.os)
            load_manifest(a.systemos)
            return
        if not a.mounts:
            ap.error(f'{a.mode} needs --mounts')
        if a.mode == 'list':
            if not a.out:
                ap.error('list needs --out')
            sys.exit(cmd_list(a))
        if not a.list or not a.live:
            ap.error('compare needs --list and --live')
        sys.exit(cmd_compare(a))
    except OSError as e:
        print(f'VOID: {e}')
        sys.exit(2)
    except SystemExit as e:
        if isinstance(e.code, str):
            print(e.code)
            sys.exit(2)
        raise


if __name__ == '__main__':
    main()
