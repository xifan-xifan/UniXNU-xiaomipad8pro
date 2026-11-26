# Host tool specification for USB network transfer testing

Run from the Mac against a boot the harness already owns. Use `KIND=observe` for the baseline and keep the boot running through all post-transfer reads. The workload uses one SSH connection per trial, streams bytes without staging files on the tablet, and records the outcome in both USB directions. See [USB network reset analysis](../../docs/drivers/usb-net-reset.md) for interpretation.

## Inputs and preflight

- Take the boot label and mode (installed or RAM), the Mac USB interface, the tablet USB interface, and key-based SSH connection arguments from the existing harness. Resolve the tablet's IPv6 link-local address and Mac interface scope at runtime. Keep endpoint identities, login details and raw SSH argv out of artifacts.
- Verify the route to the scoped destination uses the selected USB interface. Discover the two interfaces from the active NCM device on the Mac and `PianoUsbNet` in the tablet's IORegistry. Do not infer a fixed `en` unit. Check that both ends report active and that the harness can still read the independent ACM monitor. On a fresh boot, start the transfer trials after at least 120 seconds of uptime and after the interface and route stabilize.
- Here `ssh_usb` means the harness's direct scoped IPv6 SSH invocation with `-6 -T -o BatchMode=yes -o Compression=no -o ControlMaster=no -o ControlPath=none`. Disable proxying and fallback to any other interface or address. Keep the installed cipher choice fixed across an A/B and record only its name. No PTY or shell startup output is permitted on stdout. Retain SSH's exit status and sanitized stderr separately.
- Validate the remote command path and stdin first. A diagnostic command must print a known token. Sending `abc` with no newline to `ssh_usb 'LC_ALL=C /usr/bin/cksum'` must return `1219131554 3`. Compare with the local utility. Take an initial read of the `usb` monitor and confirm the driver emits its network and event lines. Run a 64 KiB smoke in both directions before the isolated reproduction or the full matrix.

## Workload

Stagger trials and keep one active at a time. The full matrix is **64, 128, 256, 512 KiB and 1, 2, 4, 8, 16, 32, 64 MiB**, in ascending order, both directions at each size. Use three trials per size/direction and alternate the first direction on each repetition. For the deciding read, run a single 4 MiB IN trial between synchronized snapshots with unrelated USB/SSH traffic quiescent, then repeat it if the transfer failed but the evidence was incomplete. Preserve a 64 KiB control from the same boot.

Let `blocks = requested_bytes / 65536`. The test payload is exactly that many 64 KiB zero-filled blocks. Disable SSH compression so zeros place traffic on the wire. Compute the local reference CRC and byte count from the same generator independently of the network:

```sh
/bin/dd if=/dev/zero bs=65536 count="$blocks" | LC_ALL=C /usr/bin/cksum
```

**IN / pull (tablet → Mac)**. Execute the generator on the tablet and stream its stdout into the Mac's checksum and byte-count consumer.

```sh
ssh_usb "/bin/dd if=/dev/zero bs=65536 count=$blocks" </dev/null |
    LC_ALL=C /usr/bin/cksum
```

The consumer must read until EOF even after SSH reports a reset, so it records the bytes actually delivered. Keep stderr separate from stdout and record the checksum and count for partial transfers too.

**OUT / push (Mac → tablet)**. Stream the local generator into SSH stdin. The tablet's checksum and byte count are the receipt, written on stdout after EOF.

```sh
/bin/dd if=/dev/zero bs=65536 count="$blocks" |
    ssh_usb 'LC_ALL=C /usr/bin/cksum'
```

Validate `blocks` as an integer selected from the matrix before putting it in the remote command. The shell examples show the data path. The harness must reap both processes and preserve both statuses (e.g. Bash `PIPESTATUS` captured immediately, or direct subprocess waits). A checksum computed over a short stream can exit zero. A generator that wrote every byte into a pipe does not establish delivery. With no valid remote receipt, record delivered bytes as unknown and keep the separate local producer outcome. Enforce a 15 s connect timeout and a 300 s trial deadline measured from invocation. Record harness timeout separately from SSH/reset, including whether the harness intervened before the socket closed. Reap any terminated trial before opening a new connection.

## Observation and result

1. For the isolated 4 MiB pull, collect the full before/after snapshot set in the analysis. Read both sides' `netstat -s -p tcp`, scoped interface statistics and the tablet's `usb` monitor. Use a timestamped console window and tablet SSH logs. Match the start and end markers to the exact trial and take the post snapshot before a health-check connection. For the matrix, use these snapshots at the first failure in each direction and around the 4 MiB sentinel. Confirm the snapshot and console captures really cover the boot and trial.
2. Record one result per trial with boot label/mode, size in bytes, direction (`in` or `out`), trial number, elapsed time, delivered bytes, expected and observed CRC, producer/SSH/consumer status where available, and a failure category. Record counter deltas and sanitized event excerpts keyed to the same trial ID. Keep raw interface addresses, MAC addresses, host names, identities, secrets and any packet payload out of stored results. If gathering optional wire headers, emit direction, relative time, flags and relative TCP sequence/ACK values only.
3. **Pass** requires a zero SSH exit, successful generator/consumer and matching receiver CRC and exact byte count. **Transfer failure** includes a validly routed, established trial with reset, short count, bad CRC, or an established transfer exceeding its deadline despite working controls. **Invalid trial** includes a failed preflight, wrong route, missing capture for a diagnostic repeat, unexpected stdout or a known host pipeline failure. An invalid diagnostic repeat does not erase a separately valid transfer failure. Preserve each trial result.
4. After a failure and its post-snapshot, use the existing ACM console and a fresh small SSH command to establish its scope. If the NCM link remains down, capture that state and mark the remaining trials unavailable on this boot. Compare a future opt-in build with the baseline using the same cable, interfaces, workload and boot mode. Qualification requires the entire matrix to pass with the option enabled and the diagnostic counters to show the proposed mechanism was exercised.

## TX flow-control RAM A/B

- Use matching RAM boots with `piano-usb-net-txflow=0` for the control and `piano-usb-net-txflow=1` for the candidate. Confirm the actual boot argument and the `PianoDwc3: net: piano-usb-net-txflow=` publication line before the smoke. Run the 64 KiB–64 MiB matrix above in both directions at full speed. Use the paced pull only as a diagnostic control.
- After the link settles, read the `usb` monitor before and after each trial. The candidate must deliver the exact byte count and CRC with zero SSH exit for every trial. Require `drops: send` and `queue drops` to stay at 0 through the matrix, with `stalls` and `restarts` both advancing during at least one unpaced pull while the link stays up. A run without a stall leaves the retry path unverified.
- Record the same counters on the control boot, including the first failed unpaced pull if reproduced. Verify fresh small SSH and a current `usb` reply after the last trial.
