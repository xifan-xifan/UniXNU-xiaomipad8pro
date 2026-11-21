# Watchdog candidate BdsDxe comparison

Compared `out/bds-gate.efi` and `out/bds-enter.efi` from the watchdog firmware inventory. The extracted BdsDxe images have the same size and section layout. The only four differing bytes fall in the `.rdata` build timestamp at offsets `0x53365`, `0x53368`, `0x5336a`, and `0x5336b`. The timestamp changes from `Nov 21 2026 18:54:11` to `Nov 21 2026 19:52:36`. All executable bytes and the remaining data match. This comparison covers the extracted PE images. The watchdog handoff still requires tablet evidence.
