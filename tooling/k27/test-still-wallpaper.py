#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host test of still-wallpaper.py and still-wallpaper.sh on a temporary fake root; no Apple files and no tablet access."""
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
GENERATOR = HERE / 'still-wallpaper.py'
SCRIPT = HERE / 'still-wallpaper.sh'
ROOT_ID = 'FFFFEEEE-DDDD-CCCC-BBBB-AAAA00000000'
AGENT_ID = 'FFFFEEEE-DDDD-CCCC-BBBB-AAAA0000005C'
GUID = 'abcdefab-cdef-4abc-8def-abcdefabcdef'
STOCK_URL = 'file:///System/Library/CoreServices/DefaultDesktop.heic'


def run(*args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kwargs)


def check(choice):
    configuration = plistlib.loads(choice['Configuration'])
    assert configuration == {'type': 'imageFile', 'url': {'relative': STOCK_URL}}
    assert choice['Files'] == [{'relative': STOCK_URL}]
    assert choice['Provider'] == 'com.apple.wallpaper.choice.image'


def main():
    with tempfile.TemporaryDirectory(prefix='still-wallpaper-test-') as temporary:
        tmp = Path(temporary)
        stage = tmp / 'stage'
        made = run(sys.executable, str(GENERATOR), str(stage))
        assert made.stdout.count('--file ') == 3
        png = (stage / 'still.png').read_bytes()
        assert png[:8] == b'\x89PNG\r\n\x1a\n'
        assert struct.unpack('>II', png[16:24]) == (3200, 2136)
        metadata = plistlib.loads((stage / 'Metadata.plist').read_bytes())
        check(metadata['Content']['Choices'][0])
        index = plistlib.loads((stage / 'Index.plist').read_bytes())
        check(index['AllSpacesAndDisplays']['Linked']['Content']['Choices'][0])
        assert not any('UniXNU Still' in str(value) for value in (metadata, index))

        fake_bin = tmp / 'bin'
        fake_bin.mkdir()
        sips_log = tmp / 'sips-args'
        sips = fake_bin / 'sips'
        sips.write_text('''#!/bin/sh
printf '%s\\n' "$@" >> "$SIPS_LOG"
[ "${FAKE_SIPS_FAIL:-}" != 1 ] || exit 1
output=
while [ "$#" -gt 0 ]; do
  if [ "$1" = --out ]; then output=$2; shift 2; else input=$1; shift; fi
done
[ -z "$output" ] || cp "$input" "$output"
''')
        sips.chmod(0o755)
        stock = tmp / 'stock Default.heic'
        stock.write_bytes(b'synthetic stock picture')
        database = tmp / 'Wallpapers'
        users = tmp / 'dslocal-users'
        users.mkdir()
        home = tmp / 'home'
        (home / 'Library').mkdir(parents=True)
        record = {
            'uid': ['501'], 'gid': ['20'], 'generateduid': [GUID], 'home': [str(home)],
        }
        (users / 'owner.plist').write_bytes(plistlib.dumps(record, fmt=plistlib.FMT_BINARY))
        for name, guid in (('empty', ''), ('invalid', 'missing-uuid')):
            (users / f'{name}.plist').write_bytes(plistlib.dumps({**record, 'generateduid': [guid]}))
        env = {
            **os.environ,
            'PATH': f'{fake_bin}:{os.environ["PATH"]}',
            'SIPS_LOG': str(sips_log),
            'PIANO_WALLPAPER_DB': str(database),
            'PIANO_WALLPAPER_STOCK': str(stock),
            'PIANO_WALLPAPER_DSLOCAL': str(users),
            'PIANO_WALLPAPER_NOCHOWN': '1',
            'PIANO_WALLPAPER_CACHE': str(tmp / 'stock-cache.png'),
        }
        account = database / GUID.upper()
        store = home / 'Library/Application Support/com.apple.wallpaper/Store/Index.plist'
        cache = Path(env['PIANO_WALLPAPER_CACHE'])

        def settled():
            # the stock conversion is a background job; boot() only returns once its end of the pipe is closed, but wait anyway
            for _ in range(200):
                if not cache.exists() or cache.read_bytes() == stock.read_bytes():
                    return
                time.sleep(0.05)

        def boot():
            sips_log.write_text('')
            return run('/bin/sh', str(SCRIPT), str(stage), env=env).stdout

        out = boot()
        assert 'uid 501: login picture written, store written' in out, out
        assert 'staged still picture now' in out, out
        assert (account / 'Wallpaper.png').read_bytes() == png   # the choice lands without waiting for any conversion
        settled()
        assert cache.read_bytes() == stock.read_bytes()
        assert not (database / 'Wallpaper.png').exists()
        assert not (database / 'MISSING-UUID').exists()
        assert (account / 'Metadata.plist').read_bytes() == (stage / 'Metadata.plist').read_bytes()
        assert store.read_bytes() == (stage / 'Index.plist').read_bytes()
        for identifier in (ROOT_ID, AGENT_ID):
            assert (database / identifier / 'Wallpaper.png').read_bytes() == png
        args = sips_log.read_text().splitlines()
        assert args[:6] == ['-s', 'format', 'png', '--resampleHeight', '2136', str(stock)]
        assert args[6] == '--out' and args[7].startswith(str(database))
        assert args[8:11] == ['-c', '2136', '3200'] and args[11] == args[7]

        out = boot()   # the cache written by the previous boot is what the picture comes from now on
        assert 'uid 501: login picture replaced ours, store replaced ours' in out
        assert 'cached on an earlier boot' in out, out
        assert (account / 'Wallpaper.png').read_bytes() == stock.read_bytes()
        assert sips_log.read_text() == ''   # nothing converted a second time
        (account / 'Metadata.plist').write_bytes(b'binary plist with our old UniXNU Still choice')
        (account / 'Wallpaper.png').write_bytes(b'old ochre export')
        store.parent.mkdir(parents=True, exist_ok=True)
        store.write_bytes(b'binary plist with our old UniXNU Still choice')
        out = boot()
        assert 'uid 501: login picture replaced ours, store replaced ours' in out
        assert (account / 'Wallpaper.png').read_bytes() == stock.read_bytes()
        assert store.read_bytes() == (stage / 'Index.plist').read_bytes()

        user_metadata = plistlib.dumps({'Choice': 'the user selected another picture'}, fmt=plistlib.FMT_BINARY)
        user_index = plistlib.dumps({'User': 'a different wallpaper store'}, fmt=plistlib.FMT_BINARY)
        (account / 'Metadata.plist').write_bytes(user_metadata)
        (account / 'Wallpaper.png').write_bytes(b'the user picture')
        store.write_bytes(user_index)
        out = boot()
        assert "uid 501: login picture kept the user's, store kept the user's" in out
        assert (account / 'Metadata.plist').read_bytes() == user_metadata
        assert (account / 'Wallpaper.png').read_bytes() == b'the user picture'
        assert store.read_bytes() == user_index

        (account / 'Metadata.plist').unlink()
        out = boot()
        assert "uid 501: login picture kept the user's, store kept the user's" in out
        assert (account / 'Wallpaper.png').read_bytes() == b'the user picture'

        cache.unlink()
        stock.unlink()
        (account / 'Metadata.plist').write_bytes(b'our UniXNU Still choice')
        store.write_bytes(b'our UniXNU Still choice')
        out = boot()
        assert (account / 'Wallpaper.png').read_bytes() == png   # no cache and no stock picture: the staged still is the fallback
        assert not cache.exists()
        assert store.read_bytes() == (stage / 'Index.plist').read_bytes()

        stock.write_bytes(b'synthetic stock picture')
        (account / 'Metadata.plist').write_bytes(b'our UniXNU Still choice')
        env['FAKE_SIPS_FAIL'] = '1'
        out = boot()
        assert 'staged still picture now' in out, out
        assert (account / 'Wallpaper.png').read_bytes() == png
        assert not cache.exists()   # a conversion that failed caches nothing
        assert not list(database.glob('.piano-*'))

    print('PASS: generated stock choice; still first with the stock conversion cached; written/replaced/kept rules; fallbacks; cleanup')


if __name__ == '__main__':
    main()
