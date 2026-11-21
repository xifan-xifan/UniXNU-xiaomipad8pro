"""Apply a verified GPT pair through a caller-provided storage backend."""


import hashlib
import runpy
from pathlib import Path


CHECKER = runpy.run_path(str(Path(__file__).with_name('check-piano-309-gpt-pair.py')))


def write_pair(read, write_and_flush, primary, backup):
    """Write the backup first and require exact readback before touching the primary.

    Offsets and lengths are bytes. The caller validates the device and both inputs
    before calling this function and handles recovery if either operation fails.
    """
    backup_offset = 510_165_778_432 - 5 * 4096
    if (len(primary), len(backup)) != (6 * 4096, 5 * 4096):
        raise ValueError('incorrect GPT fragment size')
    identities = tuple(CHECKER[key].get(hashlib.sha256(data).hexdigest())
                       for key, data in (('PRIMARY', primary), ('BACKUP', backup)))
    if identities[0] is None or identities[0] != identities[1]:
        raise ValueError('unverified GPT pair')
    if identities[0] == 'candidate':
        current = tuple(CHECKER[key].get(hashlib.sha256(read(offset, size)).hexdigest())
                        for key, offset, size in (('PRIMARY', 0, len(primary)),
                                                  ('BACKUP', backup_offset, len(backup))))
        if current != ('stock', 'stock'):
            raise OSError('candidate GPT requires both verified stock copies')
    for label, offset, fragment in (('backup', backup_offset, backup), ('primary', 0, primary)):
        write_and_flush(offset, fragment)
        if read(offset, len(fragment)) != fragment:
            raise OSError(f'{label} GPT readback mismatch')
    if read(backup_offset, len(backup)) != backup:
        raise OSError('backup GPT changed after primary update')
    if read(0, len(primary)) != primary:
        raise OSError('primary GPT changed during final pair verification')
