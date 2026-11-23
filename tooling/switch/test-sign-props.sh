#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of sign-on-remote.sh --props-from (SIGN_DRY=1: nothing leaves this host): the stock props, rollback and the fixed boot
# partition size are taken from pack-selector.py's .avb.json; a file for another partition, a zero rollback or no props is refused.
set -u
S=$(cd "$(dirname "$0")/../bootimg" && pwd)/sign-on-remote.sh
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0
ok() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }
mk() { python3 -c "import json,sys;json.dump({'avb':{'partition_name':sys.argv[2],'partition_size':int(sys.argv[3]),'rollback_index':int(sys.argv[4]),'props':json.loads(sys.argv[5])}},open(sys.argv[1],'w'))" "$@"; }
mk "$T/good.json" boot 100663296 1769904000 '{"com.android.build.boot.os_version":"15","com.android.build.boot.security_patch":"2026-02-01"}'
out=$(SIGN_DRY=1 bash "$S" --props-from "$T/good.json" x.img 2>&1); rc=$?
ok $([ $rc = 0 ] && echo "$out" | grep -q "rollback_index 1769904000" && echo "$out" | grep -q "security_patch:2026-02-01" && echo "$out" | grep -q "os_version:15" && echo "$out" | grep -q "partition 100663296"; echo $?) "stock props, rollback and the boot partition size taken ($out)"
mk "$T/vb.json" vendor_boot 100663296 1769904000 '{"a":"b"}'
SIGN_DRY=1 bash "$S" --props-from "$T/vb.json" x.img > /dev/null 2>&1; ok $([ $? != 0 ]; echo $?) "another partition refused"
mk "$T/rb0.json" boot 100663296 0 '{"a":"b"}'
SIGN_DRY=1 bash "$S" --props-from "$T/rb0.json" x.img > /dev/null 2>&1; ok $([ $? != 0 ]; echo $?) "rollback 0 refused"
mk "$T/np.json" boot 100663296 1769904000 '{}'
SIGN_DRY=1 bash "$S" --props-from "$T/np.json" x.img > /dev/null 2>&1; ok $([ $? != 0 ]; echo $?) "no props refused"
mk "$T/sz.json" boot 67108864 1769904000 '{"a":"b"}'
SIGN_DRY=1 bash "$S" --props-from "$T/sz.json" x.img > /dev/null 2>&1; ok $([ $? != 0 ]; echo $?) "another partition size refused"
echo "$([ $fails = 0 ] && echo PASS || echo FAIL): sign-on-remote --props-from tests, $fails failures"
[ $fails = 0 ]
