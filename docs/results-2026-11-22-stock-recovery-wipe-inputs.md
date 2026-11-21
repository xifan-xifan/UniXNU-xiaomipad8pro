# Stock Recovery unattended wipe inputs

The retained Recovery ELF (SHA-256 `bbc71eeb828bbfa16749f239c3e6afbdb1c40715ea7d2310170a5b193abc3534`) imports getopt_long and the bootloader-message read, update and clear APIs. Its strings include wipe_data, wipe_data_and_storage, wipe_data_normal and wipe_data_skip_preinstall. Xiaomi pre/post wipe hooks mention a temporary factory-reset filesystem and preinstall preservation. The menu and bootloader-message paths therefore need to be traced separately before choosing a command that formats both userdata and metadata.

The stock-layout exit trial used an empty Recovery request. It established the transport and exit path. The destructive option dispatch, format targets and post-wipe restart still require verification.
