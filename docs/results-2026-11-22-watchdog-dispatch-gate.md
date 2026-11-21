# Watchdog coverage and DXE dispatch

The retained Mu Basecore calls `CoreDispatcher` before `gBds->Entry`. The dispatcher queues drivers in APRIORI order and invokes their entry points. The piano watchdog is enabled later, in `DeviceBootManagerAfterConsole`, before `PianoFastbootStart`. The archived tablet witness recorded successful watchdog setup before the loader's final memory-map request.

DarwinBoot's entry point obtains the loaded-image protocol, installs the loader protocol and prints a message. Moving it to the head of APRIORI changes its pre-watchdog initialization environment. Recovery from a stall during this interval remains unverified. Keep the order control offline and use the archived order when testing the handoff. Gate the loader call on successful watchdog setup. Use an independent marker to identify progress after ExitBootServices and into XNU.
