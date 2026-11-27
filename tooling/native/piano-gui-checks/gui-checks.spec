# shellcheck shell=bash
# Source from installed-observe actions, then call gui_checks_installed after the desktop gate.
gui_checks_installed() (
    set +x
    set -o pipefail
    local checks=${1:-default}
    if [[ $checks != default && $checks != energy-picker ]]; then
        verdict 'FAIL HARNESS unknown gui-checks mode'
        return 2
    fi
    if [[ ${DRY:-0} == 1 ]]; then
        verdict 'INFO gui-checks dry run'
        return 0
    fi
    if [[ ${BOOT:-} != installed || ${KIND:-} != observe || -z ${R:-} ]]; then
        verdict 'FAIL HARNESS gui-checks requires installed observation'
        return 2
    fi
    if [[ ${REC_C920:-0} != 0 || -e "$R/c920-steps.mp4.pid" ]]; then
        verdict 'FAIL HARNESS gui-checks requires camera stills only for Wi-Fi privacy'
        return 2
    fi

    local tools binary upload='' GUI_REMOTE='' GUI_BOOT GUI_UID GUI_TARGET GUI_RESULTS
    local KBDTYPE=0
    local identity token name src hash receipt gui_hash session_hash helper
    tools=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || return 2
    binary=${GUI_BIN:-out/piano-gui-checks/gui}
    if [[ ! -f "$binary" || ! -x "$binary" ]]; then
        verdict 'FAIL HARNESS gui-checks requires the native helper build'
        return 2
    fi
    if [[ $checks == default && ( ! -r ${GUI_SSID_FILE:-} || ! -r ${GUI_REDACT:-} ) ]]; then
        verdict 'FAIL HARNESS gui-checks requires the SSID file and Wi-Fi redactor'
        return 2
    fi
    GUI_RESULTS=$R/gui-checks
    umask 077
    mkdir -p -- "$GUI_RESULTS" || return 2

    # Called by run.py through the exported Bash helper.
    # shellcheck disable=SC2329
    gui_checks_remote() {
        local operation=$1
        shift
        rsudo "$GUI_REMOTE/session.sh $operation $GUI_BOOT $GUI_UID $*"
    }

    # shellcheck disable=SC2329
    gui_checks_cleanup() {
        local rc=${1:-$?} reply cleanup_timeout=45
        trap - EXIT
        trap '' HUP INT TERM
        set +e
        if [[ -n "$GUI_REMOTE" ]]; then
            # session.sh is published only after both root copies pass verification.
            if [[ $checks == energy-picker ]]; then cleanup_timeout=65; fi
            reply=$(SSH_T=$cleanup_timeout rsudo "if [ -x $GUI_REMOTE/session.sh ]; then
                $GUI_REMOTE/session.sh cleanup $GUI_BOOT $GUI_UID
              else
                /bin/rm -f $GUI_REMOTE/gui $GUI_REMOTE/session.incoming &&
                { [ ! -d $GUI_REMOTE ] || /bin/rmdir $GUI_REMOTE; } &&
                echo GUI_CLEANUP_OK
              fi" </dev/null 2>&1) || true
            if grep -Fqx GUI_CLEANUP_OK <<< "$reply"; then
                verdict 'INFO gui-checks remote cleanup confirmed'
            elif grep -Fqx GUI_ENERGY_RECOVERY_UNCONFIRMED <<< "$reply"; then
                verdict 'FAIL HARNESS gui-checks Energy Mode recovery unconfirmed'
                rc=1
            else
                verdict 'FAIL HARNESS gui-checks remote cleanup unconfirmed'
                rc=2
            fi
        fi
        if [[ -n "$upload" ]]; then
            reply=$(rssh "/bin/rm -f $upload/gui $upload/session.sh $upload/target &&
                { [ ! -d $upload ] || /bin/rmdir $upload; } && echo GUI_STAGING_REMOVED" </dev/null 2>&1) || true
            if grep -Fqx GUI_STAGING_REMOVED <<< "$reply"; then
                verdict 'INFO gui-checks staging cleanup confirmed'
            else
                verdict 'FAIL HARNESS gui-checks staging cleanup unconfirmed'
                rc=2
            fi
        fi
        exit "$rc"
    }

    # shellcheck disable=SC2016
    identity=$(rssh '
        b=$(/usr/sbin/sysctl -n kern.bootsessionuuid) &&
        u=$(/usr/bin/stat -f %u /dev/console) &&
        test "$u" -ge 501 && test "$u" = "$(/usr/bin/id -u)" &&
        /usr/bin/pgrep -u "$u" -x Finder >/dev/null &&
        printf "GUI_SESSION %s %s\n" "$b" "$u"
    ' </dev/null 2>&1 | grep -E '^GUI_SESSION [[:xdigit:]-]{36} [0-9]+$') || {
        verdict 'FAIL HARNESS gui-checks requires a live Finder session owned by the SSH account'
        return 2
    }
    if [[ ! "$identity" =~ ^GUI_SESSION\ ([[:xdigit:]-]{36})\ ([0-9]+)$ ]]; then
        verdict 'FAIL HARNESS gui-checks desktop identity is ambiguous'
        return 2
    fi
    GUI_BOOT=${BASH_REMATCH[1]} GUI_UID=${BASH_REMATCH[2]}

    # Know each candidate path before creating it so a lost reply still reaches cleanup.
    token=$(/usr/bin/uuidgen | tr -d '-') || return 2
    [[ "$token" =~ ^[[:xdigit:]]{32}$ ]] || return 2
    trap gui_checks_cleanup EXIT
    trap 'gui_checks_cleanup 130' INT
    trap 'gui_checks_cleanup 143' TERM
    trap 'gui_checks_cleanup 129' HUP
    upload=/private/var/tmp/piano-gui.$token
    receipt=$(rssh "/bin/mkdir -m 700 $upload && echo GUI_STAGING_READY" </dev/null 2>&1) || receipt=
    if ! grep -Fqx GUI_STAGING_READY <<< "$receipt"; then
        verdict 'FAIL HARNESS gui-checks staging directory unconfirmed'
        return 2
    fi
    for name in gui session.sh; do
        src=$tools/$name
        [[ "$name" != gui ]] || src=$binary
        hash=$(shasum -a 256 < "$src") || return 2
        hash=${hash%% *}
        [[ "$hash" =~ ^[0-9a-f]{64}$ ]] || return 2
        receipt=$(/usr/bin/base64 < "$src" | SSH_T=60 rssh "umask 077;
            /usr/bin/base64 -D > $upload/$name &&
            s=\$(/usr/bin/shasum -a 256 $upload/$name) &&
            printf \"GUI_STAGE_SHA256 %s\\n\" \"\${s%% *}\"" 2>&1) || receipt=
        if ! grep -Fqx "GUI_STAGE_SHA256 $hash" <<< "$receipt"; then
            verdict "FAIL HARNESS gui-checks staged $name SHA-256 differs or is missing"
            return 2
        fi
        if [[ "$name" == gui ]]; then gui_hash=$hash; else session_hash=$hash; fi
    done

    GUI_REMOTE=/private/var/run/piano-gui.$token
    receipt=$(rsudo "set -e
        /bin/mkdir -m 700 $GUI_REMOTE
        /usr/bin/install -o root -g wheel -m 0755 $upload/gui $GUI_REMOTE/gui
        /usr/bin/install -o root -g wheel -m 0700 $upload/session.sh $GUI_REMOTE/session.incoming
        a=\$(/usr/bin/shasum -a 256 $GUI_REMOTE/gui)
        b=\$(/usr/bin/shasum -a 256 $GUI_REMOTE/session.incoming)
        test \"\${a%% *}\" = $gui_hash
        test \"\${b%% *}\" = $session_hash
        /bin/chmod 0755 $GUI_REMOTE
        /bin/mv $GUI_REMOTE/session.incoming $GUI_REMOTE/session.sh
        echo GUI_INSTALL_OK" </dev/null 2>&1) || receipt=
    if ! grep -Fqx GUI_INSTALL_OK <<< "$receipt"; then
        verdict 'FAIL HARNESS gui-checks verified root install unconfirmed'
        return 2
    fi

    GUI_TARGET=$upload/target
    export GUI_REMOTE GUI_BOOT GUI_UID GUI_TARGET GUI_RESULTS GUI_SSID_FILE GUI_REDACT R DRY
    # run.py invokes the live harness helpers from Bash. Include their installed-mode dependencies.
    export BOOT C OBS_MODE USB_SILENCE_S USB_DEADLINE PRR_PAIR_ROLE LASTSZ LASTGROW
    export INST_BOOT_STARTED SSHOK KBDTYPE KBDDONE PIANO_SERIAL
    export -f rssh rsudo shot verdict gui_checks_remote
    for helper in log health sshgate kbdtype mode_native_fatal mode_window_open mode_now fbanswers prr_exec; do
        if declare -F "$helper" >/dev/null; then export -f "${helper?}"; fi
    done

    local failed=0
    if ! python3 "$tools/run.py" ax-trust ||
       ! fexpect gui-checks/ax-trust.txt '^GUI \{"trusted":true\}$'; then
        verdict 'VOID gui-checks console-user Accessibility receipt missing'
        return 2
    fi
    verdict 'PASS gui-checks console-user Accessibility verified'
    if [[ $checks == energy-picker ]]; then
        python3 "$tools/run.py" energy-picker || return $?
        return 0
    fi
    python3 "$tools/run.py" wifi || failed=1
    python3 "$tools/run.py" brightness-ax || failed=1
    python3 "$tools/run.py" brightness-hid || failed=1
    return "$failed"
)
