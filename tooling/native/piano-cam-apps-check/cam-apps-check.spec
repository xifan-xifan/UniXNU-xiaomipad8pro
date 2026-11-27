# shellcheck shell=bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Source from the approved host actions file after its existing desktop gate.
cam_apps_check_installed() (
    set +x
    if [[ ${DRY:-0} == 1 ]]; then
        verdict 'INFO cam-apps-check dry run'
        return 0
    fi
    if [[ ${BOOT:-} != installed || ${KIND:-} != observe || -z ${R:-} || ${REC_C920:-0} != 0 ]]; then
        verdict 'VOID cam-apps-check requires installed observation with external recording off'
        return 2
    fi
    if [[ -e "$R/c920-steps.mp4.pid" ]]; then
        verdict 'VOID cam-apps-check requires external recording off'
        return 2
    fi

    local here binary python report status
    here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || return 2
    binary=${CAM_APPS_BIN:-out/piano-cam-apps-check/cam-apps-probe}
    python=${CAM_APPS_PYTHON:-/usr/bin/python3}
    if [[ ! -x "$binary" || ! -x "$python" ]]; then
        verdict 'VOID cam-apps-check requires the native probe and Python'
        return 2
    fi
    umask 077
    report=$R/cam-apps-evidence.json
    if CAM_APPS_GOP_SHOT=${CAM_APPS_GOP_SHOT-} PYTHONDONTWRITEBYTECODE=1 "$python" "$here/action.py" "$binary" "$report"; then
        status=0
    else
        status=$?
    fi
    if [[ $status != 0 ]]; then
        verdict 'VOID cam-apps-check could not collect fresh evidence'
        return 2
    fi
    local grades line
    if grades=$(PYTHONDONTWRITEBYTECODE=1 "$python" "$here/grade.py" "$report"); then
        status=0
    else
        status=$?
    fi
    while IFS= read -r line; do
        verdict "$line"
    done <<< "$grades"
    return "$status"
)
