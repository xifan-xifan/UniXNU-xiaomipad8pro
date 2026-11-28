# 2026-11-28 ul10: Wi-Fi joined (WPA2 keys installed, DHCP, traffic both ways) without a new flash

Nothing was written to the tablet's partitions for this: the installed pair stayed `boot_a` `df6d5a18…` plus
`esp-7ccdd855-k273-ul10.img` `5b357cb9…`, seed `b2583ea517732505`, store on `linux_data` LBA 117735999.

## What was missing, and what was put in place

The radio had been scanning (31 networks) but level 7 sat at `step 701 waiting for the join profile`. The join profile is the owner's
file `/private/var/db/piano-wifi/profile` (root:wheel 600 in a 700 directory, `ssid=` / `security=wpa2-psk` / `psk=`). The network name
and passphrase came from the owner's own PC (the wireless profile of the access point it is connected to), and the passphrase lives only
in `private/` on the PC, which `.gitignore` excludes through `*.psk`; it is never in a commit, a log line or a command line here.

The installed `/usr/local/libexec/piano-wifi-join` on this tablet is the Apple/CoreWLAN variant, and on this boot mode it refuses to do
anything: `exit status=1 stage=airportd-kick-missing`, because it wants `machdep.piano_wifi_native_boot=1` (this ESP leaves it 0) and a
`/var/run/piano-airportd-kick` receipt for the current boot. The repository's other variant, `tooling/native/piano-wifi-join/piano-wifi-join.c`,
is the one for the Ethernet/supplicant path: it parses the same file with the same rules and writes the 124-byte struct into
`machdep.piano_wifi_join`. It was built on the Mac (Darwin 27.2, `cc -O2`, ad-hoc `codesign -s -`), and installed as
`/usr/local/libexec/piano-wifi-join-native`; running it gave

    piano-wifi-join: handed over a network name of 11 byte(s), WPA2-PSK with a passphrase of 8 character(s)
    set (generation 1): a network name of 11 byte(s), WPA2-PSK with a passphrase, the access point not given, ... source sysctl

## Why the first seven joins failed

With the profile in the kernel the ladder associated, the access point offered the handshake, and nothing answered it:

    PianoWifi: level 8: EAPOL frame 12 from the access point: 113 bytes (PDU 99), message 1 of the four-way handshake
    PianoWifi: level 7: the association ended: a deauthentication, reason 15 (the four-way handshake timed out), 3995 ms after the station came up
    PianoWifi: level 8 at the association's end: received 12 entries, 12 frames (1356 bytes), 12 EAPOL (to the port 0, no client 12)
    PianoWifi: level 8 at the association's end: the port: no client (0 connect(s), 0 bad) ... keys: 0 sent, 0 installed

seven times, with the ladder retrying on its own ("joining again in 30 s, from the profile"). `portEnqueue` drops when `gCtlUnit` is 0,
so the frames were dropped because the user-space 802.1X client was not on the port at those moments. The kext's port takes one client
(`portConnect` answers `EBUSY` when a unit is already held), which is also why a second, manually started supplicant is refused - that is
correct behaviour, and `lsof -p <supplicant>` is how the holder is seen:

    piano-wif 261 root 3u systm ... [ctl com.unixnu.driver.PianoWifi.8021x id 12 unit 1]

Once the boot supplicant held the unit, the next join from the ladder went through with no other change.

## Joined, on the tablet's console and in the network itself

    PianoWifi: step 714: associated
    PianoWifi: level 8 while associated (190050 ms after the association): received 62 entries, 454 frames (122370 bytes), 30 EAPOL ...
    PianoWifi: level 8 while associated: the port: a client (1 connect(s), 0 bad), 9 message(s) out (0 failed); keys: 2 sent, 2 installed, 0 refused, 0 unanswered

Two keys installed is the four-way handshake finished (pairwise and group), and the association held for over three minutes.

    en0: inet 192.168.31.98 netmask 0xffffff00 broadcast 192.168.31.255; status: active
    ipconfig getpacket en0: ACK from 192.168.31.1, yiaddr 192.168.31.98, lease 0xa8c0 (43200 s)
    route -n get default: gateway 192.168.31.1

Traffic, from two directions:

    ping -c 3 192.168.31.1      3 transmitted, 3 received, 0.0% loss, 5.0/5.6/6.1 ms
    curl http://192.168.31.1    code=200 in 0.018 s
    (Mac) ping -c 2 192.168.31.98   2 transmitted, 2 received, 0.0% loss, 10.8/11.2 ms
    (PC)  ssh -p 2222 root@192.168.31.98   hostname: XiaomiPad8Pro

so frames cross the air in both directions, unicast, with an external machine reaching the tablet.

## Not achieved in this session: names and the internet, for a reason outside the driver

Name resolution fails here: the access point answers the tablet's query with `status: REFUSED`, and the public resolvers set through
`networksetup -setdnsservers` (223.5.5.5, 119.29.29.29) time out - after which the setting was returned to the router's own with
`networksetup -setdnsservers Ethernet Empty`. The same LAN shows the same to the Mac: `dig www.baidu.com` answers nothing, `ping
223.5.5.5` is 100 % loss and `nc -z -G 5 223.5.5.5 443` fails. The PC next to it cannot reach the internet either: `git push` fails with
`ssh: Could not resolve hostname github.com: Name or service not known`, and its local proxy at `127.0.0.1:10808` answers
`HTTP/1.1 200 Connection established` for every CONNECT and then relays nothing, so even `www.baidu.com:443` through it yields no bytes.
DNS and WAN for this whole network are down at the moment, so "Wi-Fi reaches the internet" cannot be claimed or refuted by any device here.
What is claimed above is what this device demonstrably does: association, WPA2 keys, DHCP, and routed unicast traffic on the LAN.

One ordering fact worth keeping: the ladder reaches its first association (about 40 s into the boot) before the 802.1X client is on the
port, so the first joins burn the handshake. The ladder retries every 30 s by itself, so a join succeeds once the client is attached -
`piano-wifi-join-native` plus a held port is enough, no reboot loop needed.