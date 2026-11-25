# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""Read-only qualification of the daily charging vote, including its SOC hold."""


def signed(value):
    return value - (1 << 32) if value >= (1 << 31) else value


def policy_state(row):
    """Accept only a fully applied 80/75 vote supported by fresh dual gauges.

    A hold in the hysteresis band is valid; a hold at/below resume is not.
    Current can still describe the gauge sample preceding a newly applied stop.
    Callers observing settled current must check it independently.
    """
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    require(tuple(row[k] for k in ('HealthEnabled', 'HealthStop', 'HealthResume',
                                  'Mode', 'HealthInhibitSeconds')) == (1, 80, 75, 0, 0),
            'daily 80/75 5V-only config')
    require(not any(row[k] for k in ('ConfigPending', 'ConfigError', 'HealthPending', 'PendingSlots')),
            'pending/error charge vote')
    require(row['Pack0Valid'] == row['Pack1Valid'] == 1 and row['PackAgeMs'] <= 45000
            and row['InputValid'] & 257 == 257, 'invalid/stale dual gauge or bus')
    soc = max(row['Pack0SOC'], row['Pack1SOC'])
    require(all(0 <= row[f'Pack{i}SOC'] <= 100 for i in (0, 1)), 'invalid SOC')
    require(row['SafetyValid'] == 31 and not any(row[k] for k in ('SafetyAlarmMask', 'DTPT', 'LPD')),
            'safety incomplete/active')
    hold = row['HealthHold']
    require(hold in (0, 1), 'invalid hold flag')
    require((soc > 75 if hold else soc < 80), 'SOC/hold contradicts 80/75 policy')
    require(tuple(row[k] for k in ('EnableReadback', 'HealthDesired', 'HealthApplied', 'HealthReason'))
            == ((0, 0, 1, 1) if hold else (1, 1, 1, 0)), 'actuator contradicts daily vote')
    return 'hold' if hold else 'charging'


def ui_state(text, state):
    import re
    if 'AC Power' not in text:
        raise ValueError('pmset AC absent')
    if state == 'hold':
        if not re.search(r';\s*not charging(?:;|\s|$)', text):
            raise ValueError('pmset health hold absent')
    elif not re.search(r';\s*charging(?:;|\s|$)', text) or 'not charging' in text or 'discharging' in text:
        raise ValueError('pmset charging absent')
