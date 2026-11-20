#!/usr/bin/env python3
"""Host-only checks for ABL slot success and retry-count gates; never invokes fastboot."""
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location('maint_session', HERE / 'maint-session.py')
maint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maint)


class FakeSession:
    execute = True

    def __init__(self, values, previous=6):
        self.values = values
        self.state = {'abl': {'slot-retry-count:a': previous}}
        self.checks = []
        self.logs = []

    def fastboot(self, *args, **kwargs):
        var = args[1]
        default = 'none' if var == 'snapshot-update-status' else None
        return f'(bootloader) {var}: {self.values.get(var, default)}'

    def need(self, condition, what):
        self.checks.append((condition, what))
        if not condition:
            raise AssertionError(what)

    def save(self, **values):
        self.state.update(values)

    def log(self, message):
        self.logs.append(message)


def final_values(retry='6', successful='yes', unbootable='no', slot='a'):
    return {
        'current-slot': slot,
        'slot-successful:a': successful,
        'slot-unbootable:a': unbootable,
        'slot-retry-count:a': retry,
    }


def preflight_values(retry='6'):
    values = final_values(retry=retry)
    values.update({
        'product': 'piano',
        'unlocked': 'yes',
        'battery-soc-ok': 'yes',
        'partition-size:boot_a': '0x6000000',
        'battery-voltage': '4.0V',
        'snapshot-update-status': 'none',
    })
    return values


def must_reject(values, previous=6):
    try:
        maint.abl_final(FakeSession(values, previous))
    except AssertionError:
        return
    raise AssertionError(f'accepted unsafe or inconsistent ABL state: {values}, previous={previous}')


def main():
    initial = FakeSession(preflight_values())
    maint.abl_check(initial)
    assert initial.state['abl']['slot-retry-count:a'] == 6
    try:
        maint.abl_check(FakeSession(preflight_values(retry='8')))
    except AssertionError:
        pass
    else:
        raise AssertionError('abl-check accepted a retry count outside the 3-bit range')

    for status in ('MERGING', 'SNAPSHOTTED', 'UNSUPPORTED', None):
        values = preflight_values()
        values['snapshot-update-status'] = status
        try:
            maint.abl_check(FakeSession(values))
        except AssertionError:
            pass
        else:
            raise AssertionError(f'abl-check accepted snapshot status {status!r}')

    maint.abl_final(FakeSession(final_values()))
    for status in ('MERGING', 'SNAPSHOTTED', 'UNSUPPORTED', None):
        values = final_values()
        values['snapshot-update-status'] = status
        try:
            maint.abl_final(FakeSession(values))
        except AssertionError:
            pass
        else:
            raise AssertionError(f'abl-final accepted snapshot status {status!r}')
    for bad in ('-1', '8', '0x6', 'unknown', None):
        must_reject(final_values(retry=bad))
    must_reject(final_values(retry='5'))
    must_reject(final_values(successful='no'))
    must_reject(final_values(unbootable='yes'))
    must_reject(final_values(slot='b'))
    print('PASS: successful/bootable slot and unchanged retry count gates')


if __name__ == '__main__':
    main()
