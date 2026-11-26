#!/usr/bin/env python3
"""Read a unit's own Android slot-A pair (vendor_boot_a, init_boot_a) off its disk, for tooling/persist/unit_profile.py.

    fetch-slot-a-pair.py OUT --vendor-boot-dev sde23 --init-boot-dev sde28

The tooling knows three slot-A boot pairs, all of them taken from the reference unit's images, and a session's `map` refuses
anything else. A second unit running the same Android release does not necessarily hold the same bytes - the pair here is read
from the medium, block by block, with the device's own caches dropped, which is what a unit's own pair means. The output is
meant for unit_profile.py --android-slot-a; it records the digests, the device nodes and the partition geometry, and nothing
about whether Android boots, which the session keeps checking on its own terms.
"""
import hashlib, json, os, re, subprocess, sys, time
from pathlib import Path

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(D / 'tooling/persist'))
import common  # noqa: E402

argv = sys.argv[1:]
if not argv:
    sys.exit(__doc__)
out = Path(argv[0])
devs, geometry = {}, {}
i = 1
while i < len(argv):
    name = argv[i]
    if name == '--vendor-boot-dev':
        devs['vendor_boot_a'] = argv[i + 1]
    elif name == '--init-boot-dev':
        devs['init_boot_a'] = argv[i + 1]
    else:
        sys.exit(f'unknown option {name}')
    i += 2
for key in ('vendor_boot_a', 'init_boot_a'):
    if key not in devs:
        sys.exit(f'--{key.replace("_","-")}-dev is required')
shell = D / 'tooling/native/maint-shell.py'


def ask(command, timeout=120):
    r = subprocess.run([sys.executable, str(shell), command, str(timeout)], capture_output=True, text=True,
                       timeout=timeout + 30)
    if r.returncode:
        sys.exit(f'tablet command failed ({r.returncode}): {command}: {((r.stdout or "") + (r.stderr or ""))[:300]}')
    return (r.stdout or '').replace('\r', '')


mapping = {}
for line in ask('sh /run/pp/tablet.sh map', 90).splitlines():
    m = re.match(r'^([a-z0-9_]+)=(\S+) (\d+) (\d+)$', line.strip())
    if m:
        mapping[m.group(1)] = {'dev': m.group(2), 'start': int(m.group(3)), 'size': int(m.group(4))}
record = {'read': time.strftime('%Y-%m-%d %H:%M:%S'), 'unit_profile': common.UNIT_PROFILE, 'parts': {}}
for key, dev in devs.items():
    names = [k for k, v in mapping.items() if v['dev'] == dev]
    if not names:
        sys.exit(f'{dev} is not in the tablet map (known: {sorted(set(v["dev"] for v in mapping.values()))})')
    name = names[0]
    size512 = mapping[name]['size']
    if size512 % 8:
        sys.exit(f'{name}: {size512} (512-byte units) is not a whole number of 4096-byte blocks')
    blocks = size512 // 8
    # A detached job: the hash runs on the tablet and outlives one telnet line, so this polls it instead of holding a shell.
    job = f'sha-{name.replace("_", "-")}'
    r = ask(f'sh /run/pp/tablet.sh job {job} hash {dev} 0 {blocks} 4096', 90)
    if 'started=' not in r:
        sys.exit(f'{name}: the job did not start: {r[:200]}')
    deadline = time.time() + 1800
    reply = ''
    while time.time() < deadline:
        time.sleep(10)
        reply = ask(f'sh /run/pp/tablet.sh job-status {job}', 90)
        if 'running=1' not in reply:
            break
    else:
        sys.exit(f'{name}: the tablet is still hashing it after 30 min')
    if 'rc=0' not in reply:
        sys.exit(f'{name}: the tablet failed to hash it: {reply[:300]}')
    m = re.search(r'sha256=([0-9a-f]{64})', reply)
    if not m:
        sys.exit(f'{name}: no sha256 in the reply: {reply[:200]}')
    record['parts'][key] = {'device': dev, 'partition': name, 'sha256': m.group(1), 'blocks_4k': blocks,
                            'bytes': blocks * 4096, 'start_512': mapping[name]['start']}
    print(f'{name} ({dev}): {m.group(1)}', file=sys.stderr)
if len(record['parts']) != 2:
    sys.exit('two partitions are required')
record['vendor_boot_a_sha256'] = record['parts']['vendor_boot_a']['sha256']
record['init_boot_a_sha256'] = record['parts']['init_boot_a']['sha256']
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(record, indent=1) + '\n')
print(f'{out}')