#!/bin/bash
# Runs on the installed tablet. TCC attribution and csreq follow the harness tcc-seed.sh.
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
dir=${BASH_SOURCE[0]%/*}
op=$1
boot=$2
uid=$3
shift 3
[[ $EUID == 0 && $uid =~ ^[0-9]+$ && $uid -ge 501 && $dir =~ ^/private/var/run/piano-gui\.[[:xdigit:]]{32}$ ]]
[[ $(sysctl -n kern.bootsessionuuid) == "$boot" ]]
args=$(sysctl -n kern.bootargs)
awk '{ for (i=1; i<=NF; i++) if ($i ~ /^data-persist=/) { n++; ok=($i=="data-persist=2") } }
     END { exit !(n==1 && ok) }' <<< "$args"

db='/Library/Application Support/com.apple.TCC/TCC.db'
wrapper=/usr/libexec/sshd-keygen-wrapper
where="service='kTCCServiceAccessibility' AND client='$wrapper' AND client_type=1"
sql() { sqlite3 -batch -bail -noheader -separator ' ' -cmd '.timeout 5000' "$db" "$@"; }
restart_tccd() {
    local pid
    pid=$(pgrep -u "$uid" -x tccd) || return 1
    [[ $pid =~ ^[0-9]+$ ]] && kill -KILL "$pid"
}

asuser() { launchctl asuser "$uid" /usr/bin/sudo -n -u "#$uid" -- "$dir/gui" "$@"; }

energy_preferences() {
    local prefs
    prefs=$(/usr/bin/pmset -g custom) || return 1
    awk '
        /^Battery Power:[[:space:]]*$/ { source="battery"; battery++ }
        /^AC Power:[[:space:]]*$/ { source="ac"; ac++ }
        /^[^[:space:]].*:[[:space:]]*$/ && !/^(Battery|AC) Power:/ { source="" }
        $1=="powermode" {
            if (source=="" || NF!=2 || $2 !~ /^[012]$/ || seen[source]++) bad=1
            mode[source]=$2
        }
        END {
            if (bad || ac!=1 || battery!=1 || !seen["ac"] || !seen["battery"]) exit 1
            print mode["ac"], mode["battery"]
        }' <<< "$prefs"
}

energy_finish() {
    local ac battery current current_ac current_battery blocked=0
    closed=false restored=false
    # Disable late AX requests before closing the app and restoring powerd preferences.
    (umask 077; : > "$dir/energy-finished") || blocked=1
    closed=true
    if [[ -e "$dir/energy-opened" ]]; then
        if asuser energy-quit; then rm -f "$dir/energy-opened"; else closed=false; fi
    fi
    if [[ -f "$dir/energy-original" ]]; then
        read -r ac battery < "$dir/energy-original" || return 1
        [[ $ac =~ ^[012]$ && $battery =~ ^[012]$ ]] || return 1
        if current=$(energy_preferences); then
            read -r current_ac current_battery <<< "$current"
        else
            current_ac='' current_battery=''
        fi
        # Try both sources even when the first write fails.
        [[ $current_ac == "$ac" ]] || /usr/bin/pmset -c powermode "$ac" || true
        [[ $current_battery == "$battery" ]] || /usr/bin/pmset -b powermode "$battery" || true
        if [[ $(energy_preferences) == "$ac $battery" ]]; then restored=true; fi
    else
        restored=true
    fi
    [[ $blocked == 0 && $closed == true && $restored == true ]]
}

case $op in
    seed|cleanup|energy-begin|energy-open|energy-select|energy-finish)
        # A lost reply can leave a mutation running. Cleanup takes the same lock.
        lock_timeout=40
        if [[ $op == seed || ( $op == cleanup && ! -e "$dir/energy-original" ) ]]; then lock_timeout=20; fi
        exec /usr/bin/lockf -k -t "$lock_timeout" "$dir/session-lock" "$dir/session.sh" "$op-locked" "$boot" "$uid" "$@"
        ;;
esac

if [[ $op == energy-finish-locked ]]; then
    [[ $# == 1 && $1 =~ ^[a-f0-9]{32}$ ]]
    rc=0
    energy_finish || rc=2
    "$dir/gui" energy-finish "$1" "$closed" "$restored"
    exit "$rc"
fi

if [[ $op == cleanup-locked ]]; then
    energy_failed=0
    if [[ -e "$dir/energy-opened" || -e "$dir/energy-original" ]]; then
        energy_finish || energy_failed=1
    fi
    restart_failed=0
    if [[ -s "$dir/seed-owner" ]]; then
        read -r row stamp req < "$dir/seed-owner"
        [[ $row =~ ^[1-9][0-9]*$ && $stamp =~ ^[0-9]+$ && $req =~ ^([0-9a-f]{2})+$ ]]
        [[ -f $db && ! -L $db ]]
        deleted=$(sql "DELETE FROM access WHERE $where AND rowid=$row AND last_modified=$stamp
             AND auth_value=2 AND auth_reason=4 AND auth_version=1 AND flags=0
             AND csreq=X'$req'; SELECT changes();")
        if [[ $deleted == 1 ]]; then restart_tccd || restart_failed=1; fi
    fi
    if [[ $energy_failed == 1 ]]; then
        echo GUI_ENERGY_RECOVERY_UNCONFIRMED
        exit 2  # Keep the original preferences and helper so cleanup can be retried.
    fi
    rm -f "$dir/gui" "$dir/seed-owner" "$dir/seed-existing" "$dir/session.sh" "$dir/session-lock" "$dir"/req.* \
          "$dir/energy-original" "$dir/energy-original.incoming" "$dir/energy-opened" "$dir/energy-finished"
    rmdir "$dir"
    [[ $restart_failed == 0 ]]
    echo GUI_CLEANUP_OK
    exit
fi

[[ $(stat -f %u /dev/console) == "$uid" ]]
pgrep -u "$uid" -x Finder >/dev/null
launchctl print "gui/$uid" >/dev/null
registry=$(ioreg -n Root -d1 -w0)
[[ $registry == *IOConsoleUsers* && $registry != *'"CGSSessionScreenIsLocked"=Yes'* ]]
case $op in
    seed-locked)
        # The RAM seed also sets Camera rows. On installed Data, touch only the Accessibility row,
        # preserve an existing policy, and remember the exact temporary row for cleanup.
        [[ -f $db && ! -L $db && ! -e "$dir/seed-owner" && ! -e "$dir/seed-existing" ]]
        reqfile=$(mktemp "$dir/req.XXXXXXXX")
        trap 'unlink "$reqfile"' EXIT
        printf 'identifier "com.apple.sshd-keygen-wrapper" and anchor apple\n' | csreq -r- -b "$reqfile"
        req=$(xxd -p "$reqfile" | tr -d '\n')
        [[ $req =~ ^([0-9a-f]{2})+$ ]]
        stamp=$(date +%s)
        # Write the root-only ownership receipt before committing. A write failure
        # aborts the transaction. An existing policy leaves no ownership receipt.
        umask 077
        observed=$(sql <<SQL
            CREATE TEMP TABLE receipt (bytes INTEGER NOT NULL CHECK(bytes > 0));
            BEGIN IMMEDIATE;
            INSERT INTO access (service, client, client_type, auth_value, auth_reason, auth_version, csreq, flags, last_modified)
            SELECT 'kTCCServiceAccessibility', '$wrapper', 1, 2, 4, 1, X'$req', 0, $stamp
            WHERE NOT EXISTS (SELECT 1 FROM access WHERE $where);
            INSERT INTO receipt
                SELECT writefile('$dir/seed-owner', rowid || ' ' || last_modified || ' ' || lower(hex(csreq)) || char(10))
                FROM access WHERE rowid=last_insert_rowid() AND changes()=1;
            SELECT CASE
                WHEN EXISTS (SELECT 1 FROM receipt) THEN 'GUI_TCC_SEEDED'
                WHEN count(*)=1 AND sum(auth_value=2 AND csreq=X'$req')=1 THEN 'GUI_TCC_ALREADY_TRUSTED'
                ELSE 'GUI_TCC_EXISTING_POLICY'
            END FROM access WHERE $where;
            COMMIT;
SQL
        )
        case $observed in
            GUI_TCC_SEEDED)
                [[ -s "$dir/seed-owner" ]]
                restart_tccd
                ;;
            GUI_TCC_ALREADY_TRUSTED)
                printf 'existing-unchanged\n' > "$dir/seed-existing"
                ;;
            GUI_TCC_EXISTING_POLICY)
                echo "$observed"
                exit 2
                ;;
            *) exit 2 ;;
        esac
        echo "$observed"
        ;;
    wifi-state|wifi-disassociate)
        "$dir/gui" "$op" "$@"
        ;;
    energy-state)
        [[ $# == 1 && $1 =~ ^[a-f0-9]{32}$ ]]
        "$dir/gui" "$op" "$@"
        ;;
    energy-begin-locked)
        [[ $# == 3 && $1 =~ ^[a-f0-9]{32}$ && $2 =~ ^[012]$ && $3 =~ ^[012]$ ]]
        [[ ! -e "$dir/energy-finished" && ! -e "$dir/energy-original" ]]
        [[ $(energy_preferences) == "$2 $3" ]]
        umask 077
        printf '%s %s\n' "$2" "$3" > "$dir/energy-original.incoming"
        mv "$dir/energy-original.incoming" "$dir/energy-original"
        printf 'GUI {"op":"energy-begin","token":"%s","armed":true}\n' "$1"
        ;;
    energy-open-locked|energy-select-locked)
        [[ ${1:-} =~ ^[a-f0-9]{32}$ && -s "$dir/energy-original" && ! -e "$dir/energy-finished" ]]
        if [[ $op == energy-open-locked ]]; then
            (umask 077; : > "$dir/energy-opened")
        else
            [[ -e "$dir/energy-opened" ]]
        fi
        asuser "${op%-locked}" "$@"
        ;;
    display-sample)
        asuser "$op"
        ioreg -r -c PianoBacklight -l -w0
        ;;
    ax-trust|wifi-menu|wifi-find|wifi-click|wifi-off|wifi-on|display-open|display-read|display-set|cursor)
        asuser "$op" "$@"
        ;;
    *) exit 2 ;;
esac
