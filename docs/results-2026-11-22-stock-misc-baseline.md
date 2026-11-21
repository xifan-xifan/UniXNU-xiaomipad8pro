# Stock misc capture review

Both retained 4 MiB misc images match manifest SHA-256 `d9148e265e838ebc3013ac8537cfbc1a3ef615055fe87562b46fd3b5dfa16540`. The first 32 bytes contain `bootonce-bootloader` followed by zeros. Bytes 32 through 4095 are zero. Restoring either capture would reintroduce that command. The captures predate the Recovery loop.

Live ADB confirmed completed HyperOS OS3.0.309.0.WPYCNXM on A. Its unprivileged shell cannot read misc. The existing maintenance RAM boot proved host control back to ABL and HyperOS from an ordinary bootloader entry. Recovery-to-ABL-to-maintenance and readback of misc in that state remain unqualified. A readback in that state can test whether a persistent boot request explains the loop before a write is considered. No device storage was written.
