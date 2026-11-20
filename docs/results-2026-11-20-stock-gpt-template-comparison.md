# HyperOS 3.0.309 GPT template comparison (2026-11-20)

The retained Xiaomi Fastboot archive contains GPT templates and `rawprogram0.xml` / `patch0.xml`. The XML describes 4 KiB sectors, the primary table at the start of LUN 0 and the backup table at its end. `patch0.xml` adjusts the userdata end, header locations and CRCs to the disk capacity.

For the measured 124,552,192-block LUN 0, applying the userdata end rule in memory gives LBA 124,552,186. Partition names, types and ranges then match the existing capture. All 34 populated entries have different unique partition GUIDs. Attributes also differ for `countrycode_a`, `vm-bootsys_a`, `vm-bootsys_b` and `vbmeta_system_a`. Entries 35–64 match. The normalized template array SHA-256 is `0242a060b7875e1860c1597f00bd656b78e2af428ec6a1fd29f004c2722da8ae`. The captured array SHA-256 is `ce27bad3adc56b4a174ef473ab24cab6282dcfea6a5e2d5c1ab643f4f599f075`.

Use the verified device captures as the source for exact GPT rollback. The stock templates establish partition geometry after capacity adjustment. This was a host-only comparison against the existing capture. The recovery sequence remains untested on hardware.
