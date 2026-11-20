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
        return f'(bootloader) {var}: {self.values.get(var)}'

    def need(self, condition, what):
        self.checks.append((condition, what))
        if not condition:
            raise AssertionError(what)

    def save(self, **values):
        self.state.update(values)

    def log(self, message):
        self.logs.append(message)



class FakeSnapshotSession:
    execute = True

    def __init__(self, userspace='yes', status='none'):
        self.userspace, self.status = userspace, status
        self.state = {}
        self.logs = []

    def fastboot(self, *args, **kwargs):
        values = {'is-userspace': self.userspace, 'snapshot-update-status': self.status}
        return f'(bootloader) {args[1]}: {values[args[1]]}'

    def need(self, condition, what):
        if not condition:
            raise AssertionError(what)

    def save(self, **values):
        self.state.update(values)

    def log(self, message):
        self.logs.append(message)

def final_values(retry='6', successful='yes', unbootable='no', slot='a'):
    return {
        'is-userspace': 'no',
        'snapshot-update-status': 'none',
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

    maint.abl_final(FakeSession(final_values()))
    for bad in ('-1', '8', '0x6', 'unknown', None):
        must_reject(final_values(retry=bad))
    must_reject(final_values(retry='5'))
    must_reject(final_values(successful='no'))
    must_reject(final_values(unbootable='yes'))
    must_reject(final_values(slot='b'))
    for key, value in (('is-userspace', 'yes'), ('snapshot-update-status', 'merging'),
                       ('snapshot-update-status', None)):
        values = preflight_values()
        values[key] = value
        must_reject(values)
        try:
            maint.abl_check(FakeSession(values))
        except AssertionError:
            pass
        else:
            raise AssertionError(f'abl-check accepted {key}={value!r}')
    good = FakeSnapshotSession()
    maint.snapshot_check(good)
    for userspace, status in (('no', 'none'), ('yes', 'MERGING'), ('yes', 'SNAPSHOTTED'), ('yes', 'UNSUPPORTED'), ('yes', None)):
        try:
            maint.snapshot_check(FakeSnapshotSession(userspace, status))
        except AssertionError:
            pass
        else:
            raise AssertionError(f'snapshot-check accepted userspace={userspace!r}, status={status!r}')
    print('PASS: A/B state and userspace Virtual A/B snapshot preflight gates')


if __name__ == '__main__':
    main()
