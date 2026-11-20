# NativeUfs write audit (2026-11-21)

OS and SystemOS share the NativeUfsBlock implementation. It reports write protection and rejects outbound block requests unless `writable` was set in `configure`. Only the unverified development profile can set it. The standard builder selects that profile and offers `PIANO_UFS_READ_ONLY_ROOT=1` to suppress root writes. The verified profile requires matching block verification policies. `doFormatMedia` rejects formatting and synchronization of a read-only root window returns without flushing the device. Raw access to the same published block device still passes through its request handler. This is source evidence, not a runtime check of the composed image.

`hostWrite4k` bounds each write to a registered LUN 0 window. Besides development root windows it accepts the Data store and the separate log/export window. `data-persist=0` disables the Data store attachment and is insufficient by itself to establish that logging is volatile. The diagnostic image must be checked for callers that open the log window as well as for the actual flags on its NativeUfs object.

The retained launchd task definitions provide flags and arguments only. `fsck -fy` permits repair. The actual device selection, status propagation and ordering relative to the two phase mounts still require a trace with the final image. The historical DroidVM shims return success on these paths without establishing their preconditions. No tablet boot or disk write was attempted in this audit.

UbuntuPiano passed all four NativeUfs host suites in plain and ASan/UBSan modes using the development profile. The Windows checkout's CRLF shell script needed line-ending normalization in the input stream. No tracked test files were changed.
