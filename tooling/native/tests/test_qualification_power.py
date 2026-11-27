# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""Fault checks for the read-only daily charging qualification contract."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('qualification_power', Path(__file__).parents[1] / 'qualification_power.py')
power = importlib.util.module_from_spec(spec)
spec.loader.exec_module(power)


def row(soc=80, hold=1):
    return dict(HealthEnabled=1, HealthStop=80, HealthResume=75, Mode=0,
                HealthInhibitSeconds=0, ConfigPending=0, ConfigError=0,
                HealthPending=0, PendingSlots=0, Pack0Valid=1, Pack1Valid=1,
                PackAgeMs=2000, InputValid=257, Pack0SOC=soc, Pack1SOC=soc,
                SafetyValid=31, SafetyAlarmMask=0, DTPT=0, LPD=0,
                HealthHold=hold, EnableReadback=1-hold, HealthDesired=1-hold,
                HealthApplied=1, HealthReason=hold)


class Contract(unittest.TestCase):
    def test_thresholds_and_hysteresis(self):
        for soc in (76, 79, 80, 100):
            self.assertEqual(power.policy_state(row(soc)), 'hold')
        for soc in (0, 75, 76, 79):
            self.assertEqual(power.policy_state(row(soc, 0)), 'charging')
        for d in (row(75), row(80, 0), row(101), row(-1)):
            with self.assertRaises(ValueError): power.policy_state(d)

    def test_all_faults_remain_failures(self):
        faults = dict(HealthEnabled=0, HealthStop=90, HealthResume=70, Mode=1,
                      HealthInhibitSeconds=2, ConfigPending=1, ConfigError=1,
                      HealthPending=1, PendingSlots=1, Pack0Valid=0, Pack1Valid=0,
                      PackAgeMs=45001, InputValid=256, SafetyValid=15,
                      SafetyAlarmMask=1, DTPT=1, LPD=1, HealthHold=2,
                      EnableReadback=1, HealthDesired=1, HealthApplied=0, HealthReason=2)
        for key, value in faults.items():
            with self.subTest(key=key), self.assertRaises(ValueError):
                power.policy_state(dict(row(), **{key: value}))
        d = row(); del d['Pack0Valid']
        with self.assertRaises(KeyError): power.policy_state(d)
        for values in (dict(ConfigPending=None), dict(HealthHold=1.0), dict(Pack0SOC=80.0), dict(InputValid=-1)):
            with self.subTest(values=values), self.assertRaises(ValueError):
                power.policy_state(dict(row(), **values))

    def test_gauge_age_bounds(self):
        for age in (0, 45000):
            self.assertEqual(power.policy_state(dict(row(), PackAgeMs=age)), 'hold')
        with self.assertRaises(ValueError):
            power.policy_state(dict(row(), PackAgeMs=-1))

    def test_ui_must_match_vote(self):
        for state, text in [('hold', 'AC Power; not charging;'), ('charging', 'AC Power; charging;')]:
            power.ui_state(text, state)
            with self.assertRaises(ValueError): power.ui_state(text, 'hold' if state == 'charging' else 'charging')
        with self.assertRaises(ValueError): power.ui_state('Battery Power; discharging;', 'hold')
        with self.assertRaises(ValueError): power.ui_state('AC Power; charging;', 'unknown')
        with self.assertRaises(ValueError): power.ui_state('Battery Power; label=AC Power; charging;', 'charging')


if __name__ == '__main__': unittest.main()
