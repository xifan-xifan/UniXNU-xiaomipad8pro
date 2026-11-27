#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""The store-switch gate of maint-session.py (need_bound_store): an ESP whose record declares data-store-replace is written only with
--replace-seed, only while the store on the tablet differs in its seed identity alone, and only when the record names the store that
is really there. Everything else stays a refusal. This drives need_bound_store with a stub session: no tablet, no images.

    python3 tooling/persist/test-store-switch.py
"""
import argparse
import importlib.util
import sys
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import common  # noqa: E402

spec = importlib.util.spec_from_file_location('tp', HERE / 'test-persist.py')
tp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tp)
spec = importlib.util.spec_from_file_location('maint', HERE / 'maint-session.py')
maint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maint)

failures = tp.failures
OLD = 'bb6dd1c365a6a82707542d3e730f282b7835ae56824b5282b2b92f218e4cf705'
NEW = 'b2583ea517732505b716600572ec8382ae832d0b239cc7971e1b52c280492a0f'
RECORD = {'image': 'esp-test-ul.img', 'seed': {'identity_sha256': NEW, 'pages': 6291456},
          'data_store': {'seed': NEW, 'partition': 'linux_data', 'first_lba': 117735999, 'replace': None}}


class Stub:
    """The pieces need_bound_store touches: the state written by bound_store, the CLI flags, the log, the backup and the save."""

    def __init__(self, problems, identity=OLD, execute=True, replace_seed=False):
        self.execute = execute
        self.a = argparse.Namespace(replace_seed=replace_seed)
        self.logs, self.saved, self.backups = [], [], []
        self.state = {'esp_store': {'partition': 'linux_data', 'first_lba': 117735999, 'node': 'sda36',
                                    'header': {'state': 'valid', 'identity': identity, 'pages': 6291456, 'generation': 3},
                                    'problems': problems}}

    def log(self, text):
        self.logs.append(text)

    def save(self, **kw):
        self.saved.append(kw)

    def backup(self, *args):
        self.backups.append(args)


def record(replace):
    import copy
    r = copy.deepcopy(RECORD)
    r['data_store']['replace'] = replace
    return r


def refused(what, fn, *args):
    """The gate raised maint.Stop."""
    try:
        fn(*args)
    except maint.Stop:
        tp.check(True, what)
    except Exception as got:
        tp.check(False, f'{what}: {type(got).__name__} instead of Stop: {got}')
    else:
        tp.check(False, what + ': nothing was raised')


def call(s, rec):
    return maint.need_bound_store(s, rec)


def unit():
    print('== need_bound_store: the plain gate and the intended switch')
    seed_problem = f'it is of seed {OLD[:16]} (6291456 pages), not {NEW[:16]} (6291456 pages)'
    other = 'it is bound to no LBA (unbound), not LBA 117735999'

    with patch.object(maint, 'bound_store', return_value=(True, 'the store ... IS the one this ESP resumes')):
        s = Stub([])
        call(s, record(None))
        tp.check(s.logs == ['  ok    the store ... IS the one this ESP resumes'], 'a matching store passes as before')
    with patch.object(maint, 'bound_store', return_value=(None, 'the store ... (dry run)')):
        s = Stub([])
        call(s, record(None))
        tp.check(s.logs and s.logs[0].startswith('  check '), 'a dry run prints the check without a tablet')

    with patch.object(maint, 'bound_store', return_value=(False, 'the store ... is NOT the one this ESP resumes (seed ...)')):
        s = Stub([seed_problem], replace_seed=True)
        refused('an ESP with no data-store-replace is still refused', call, s, record(None))
        s = Stub([seed_problem], replace_seed=False)
        refused('data-store-replace without --replace-seed is refused', call, s, record('bb6dd1c3'))
        s = Stub([seed_problem, other], replace_seed=True)
        refused('more than the seed identity differing is refused', call, s, record('bb6dd1c3'))
        s = Stub([other], replace_seed=True)
        refused('another problem alone is refused', call, s, record('bb6dd1c3'))
        s = Stub([seed_problem], replace_seed=True)
        refused('a switch naming another store than the tablet has is refused', call, s, record('0123abcd'))

    with patch.object(maint, 'bound_store', return_value=(False, 'the store ... is NOT the one this ESP resumes (seed ...)')), \
            patch.object(maint, 'part_geometry', return_value=('sda36', 117735999 - 1000, 4000)):
        s = Stub([seed_problem], execute=False, replace_seed=True)
        call(s, record('bb6dd1c3'))
        tp.check(s.logs and s.logs[-1].startswith('  check the store of seed bb6dd1c365a6a827'), 'the dry run states the switch')
        s = Stub([seed_problem], execute=True, replace_seed=True)
        call(s, record('bb6dd1c3'))
        tp.check([b[0] for b in s.backups] == ['k273-store-header'], 'the header being replaced is backed up')
        tp.check(s.backups[0][1:5] == ('sda36', 1000, 1, maint.B), 'the backup is that one 4 KiB block, read relative to the partition')
        tp.check(s.saved and s.saved[0]['store_replace']['from'] == OLD and s.saved[0]['store_replace']['to'] == NEW,
                 'the switch is recorded (the store it replaces, the seed it begins with)')
        s = Stub([seed_problem], identity='111111111111111107542d3e730f282b', execute=True, replace_seed=True)
        call(s, record('1'))
        tp.check(s.logs[-1].startswith('  ok    the store of seed 1111111111'), '"1" (any other seed store) is accepted as the intent')


def main():
    unit()
    print()
    if failures:
        print(f'{len(failures)} FAILURE(S):')
        for f in failures:
            print('  ' + f)
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
