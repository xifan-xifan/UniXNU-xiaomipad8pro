#!/usr/bin/env python3
"""Which disk is which, for the k273 image tools (never guessed from text output: a wrong disk here is the Mac's own disk).

  disk-of.py whole IMAGE      the /dev/diskN hdiutil attached IMAGE as (its whole disk), from `hdiutil info -plist`
  disk-of.py volume /dev/diskN [UUID]  the sole APFS volume on that store, or the unique volume with UUID
  disk-of.py uuid /dev/diskMsK  that volume's UUID

Exits 1 with a message when there is no single answer.
"""
import os
import plistlib
import re
import subprocess
import sys


def run(*cmd):
    return plistlib.loads(subprocess.run(cmd, capture_output=True, check=True).stdout)


def whole(image):
    path = os.path.realpath(image)
    found = []
    for item in run('hdiutil', 'info', '-plist').get('images', []):
        if os.path.realpath(item.get('image-path', '')) != path:
            continue
        # the image's own disk, not the APFS container macOS synthesizes on a raw container image (content hint EF57347C-...)
        found += [e['dev-entry'] for e in item.get('system-entities', []) if re.fullmatch(r'/dev/disk\d+', e.get('dev-entry', ''))
                  and not e.get('content-hint', '').startswith('EF57347C-0000-11AA-AA11')]
    if len(found) != 1:
        sys.exit(f'{image}: {len(found)} attachments ({found})')
    return found[0]


def volume(disk, expected_uuid=None):
    name = disk.removeprefix('/dev/')
    if not re.fullmatch(r'disk[0-9]+', name):
        sys.exit(f'{disk}: expected a whole disk')
    found = []
    for container in run('diskutil', 'apfs', 'list', '-plist').get('Containers', []):
        stores = [s.get('DeviceIdentifier') for s in container.get('PhysicalStores', [])]
        if stores == [name] or stores == [name + 's1']:
            for entry in container.get('Volumes', []):
                device = '/dev/' + entry['DeviceIdentifier']
                if expected_uuid is None or uuid(device).casefold() == expected_uuid.casefold():
                    found.append(device)
    if len(found) != 1:
        sys.exit(f'{disk}: expected one matching APFS volume, found {len(found)} ({found})')
    return found[0]


def uuid(device):
    return run('diskutil', 'info', '-plist', device)['VolumeUUID']


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'volume':
        print(volume(*sys.argv[2:]))
        return
    if len(sys.argv) != 3 or sys.argv[1] not in ('whole', 'volume', 'uuid'):
        sys.exit(__doc__)
    print({'whole': whole, 'volume': volume, 'uuid': uuid}[sys.argv[1]](sys.argv[2]))


if __name__ == '__main__':
    main()
