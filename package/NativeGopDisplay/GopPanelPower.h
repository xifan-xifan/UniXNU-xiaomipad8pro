// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

namespace gopcompose {

// Used under the copy lock. A request can supersede the one in flight. Only the
// work loop publishes a completed wake, so a stale completion cannot light the panel.
struct PanelPower {
    bool wantedOn = true;
    bool pending = false;
    bool ready = true;
    bool wakeComplete = false;

    bool request(bool on)
    {
        if (on == wantedOn)
            return false;
        wantedOn = on;
        pending = true;
        wakeComplete = false;
        return true;
    }

    bool begin(bool &on)
    {
        if (!pending)
            return false;
        on = wantedOn;
        pending = false;
        ready = false;
        return true;
    }

    // Both a panel wake and a backlight-only wake release presentation.
    void finish(bool on)
    {
        ready = on;
        wakeComplete = ready && wantedOn && !pending;
    }

    bool canPresent() const { return wantedOn && !pending && ready; }
};

} // namespace gopcompose
