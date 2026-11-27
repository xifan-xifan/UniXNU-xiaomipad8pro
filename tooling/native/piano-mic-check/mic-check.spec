# SPDX-License-Identifier: MIT
# shellcheck source-path=SCRIPTDIR
# Source from the installed-system qual spec. Uses R, rssh, rsudo, pull, verdict and fexpect.
# mic_check_block REPO [raw|noise-reduced] runs after the desktop gate.
# piano-mic-unattended=1 enables automatic permission and acoustic replay checks.
# With the boot arg off, MIC_CHECK_REQUEST_ACCESS=1 permits an attended first run.

mic_check_gui() {   # STAGE RESULT_SUBDIR BOOT_UUID TAG [APP ARG ...]
  local stage=$1 name=$2 boot=$3 tag=$4 args rc=0
  shift 4
  printf -v args ' %q' "$@"
  # Open the actual bundle through LaunchServices to give TCC the app's GUI identity.
  # The text-mode rsudo wrapper encloses its command in single quotes. The app's
  # result line survives wrappers that discard exit status, including open -W.
  SSH_T=95 rsudo "
    if ! test \"\$(/usr/sbin/sysctl -n kern.bootsessionuuid)\" = \"$boot\"; then
      echo MIC_BOOT_CHANGED
      exit 1
    fi
    u=\$(/usr/bin/stat -f %u /dev/console)
    if ! test \"\$u\" -gt 0 || ! test \"\$u\" = \"\$(/usr/bin/stat -f %u \"$stage\")\"; then
      echo MIC_CONSOLE_UID_MISMATCH
      exit 1
    fi
    /bin/launchctl asuser \"\$u\" /usr/bin/sudo -n -u \"#\$u\" -- \
      /usr/bin/open -W -n --stdout \"$stage/$tag.stdout\" --stderr \"$stage/$tag.stderr\" \
        -a \"$stage/PianoMicCheck.app\" --args$args
    printf \"MIC_OPEN_RC=%s\\n\" \"\$?\"
  " </dev/null > "$R/$name/$tag.launch.txt" 2>&1 || rc=1
  rssh "/bin/cat '$stage/$tag.stdout'" </dev/null > "$R/$name/$tag.txt" 2>&1 || rc=1
  rssh "/bin/cat '$stage/$tag.stderr'" </dev/null > "$R/$name/$tag.stderr" 2>&1 || rc=1
  fexpect "$name/$tag.launch.txt" '^MIC_OPEN_RC=0$' "MIC $tag: GUI app launched" || rc=1
  fexpect "$name/$tag.txt" '^result=' "MIC $tag: app returned a result" || rc=1
  return "$rc"
}

mic_check_seed() {   # TOOLS STAGE RESULT_SUBDIR BOOT_UUID APP_CDHASH
  local tools=$1 stage=$2 name=$3 boot=$4 cdhash=$5 sha
  sha=$(/usr/bin/shasum -a 256 < "$tools/mic-tcc-seed.sh") || return 1
  sha=${sha%% *}
  if ! /usr/bin/base64 -i "$tools/mic-tcc-seed.sh" | SSH_T=30 rssh "
    umask 077
    /usr/bin/base64 -D > '$stage/mic-tcc-seed.sh' && echo MIC_SEED_STAGED
  " > "$R/$name/tcc-stage.txt" 2>&1 ||
    ! fexpect "$name/tcc-stage.txt" '^MIC_SEED_STAGED$' 'MIC TCC: seeder transferred'; then
    return 1
  fi
  # rsudo consumes stdin. Pin the executable in a fresh root-owned directory
  # before invoking it, and verify the receipt even when rsudo drops exit status.
  SSH_T=60 rsudo "
    umask 077
    d=\$(/usr/bin/mktemp -d /private/var/run/piano-mic-tcc.XXXXXXXX) || exit 1
    /usr/bin/install -m 0700 $stage/mic-tcc-seed.sh \"\$d/seed\" &&
      sum=\$(/usr/bin/shasum -a 256 < \"\$d/seed\") && test \"\${sum%% *}\" = $sha &&
      /bin/bash \"\$d/seed\" --apply $boot $stage $cdhash
    rc=\$?
    /bin/rm -f -- \"\$d/seed\"
    /bin/rmdir \"\$d\"
    exit \"\$rc\"
  " </dev/null > "$R/$name/tcc-seed.txt" 2>&1 || return 1
  fexpect "$name/tcc-seed.txt" '^TCC_MIC_APP_OK$' 'MIC TCC: signed bundle grant read back' &&
    fexpect "$name/tcc-seed.txt" '^TCC_MIC_SEED_COMPLETE$' 'MIC TCC: console tccd signalled'
}

mic_check_replay_take() {   # AUDIO_LISTEN STAGE RESULT_SUBDIR BOOT_UUID TAKE SECONDS
  local listener=$1 stage=$2 name=$3 boot=$4 mode=$5 seconds=$6
  local tag=replay-$mode pcm pid attempt ready=0 rc=0
  pcm=$R/$name/c920-$mode.wav
  C920_AUDIO_READY="$pcm.started" python3 "$listener" record --seconds "$seconds" --out "$pcm" \
    --device 'HD Pro Webcam C920' --progress > "$pcm.record.txt" 2>&1 &
  pid=$!
  for ((attempt = 0; attempt < 60; attempt++)); do
    kill -0 "$pid" 2>/dev/null || break
    if [ -f "$pcm.started" ] || grep -aqE '^out_time_us=[1-9][0-9]*$' "$pcm.record.txt"; then
      ready=1
      break
    fi
    sleep 0.2
  done
  if [ "$ready" = 0 ] || ! kill -0 "$pid" 2>/dev/null; then
    verdict "FAIL HARNESS MIC $tag: C920 produced no live PCM receipt ($name/c920-$mode.wav.record.txt)"
    wait "$pid" || true
    return 1
  fi
  mic_check_gui "$stage" "$name" "$boot" "$tag" --replay "$stage/$mode.m4a" || rc=1
  if ! kill -0 "$pid" 2>/dev/null; then
    verdict "FAIL HARNESS MIC $tag: C920 capture ended before the playback receipt"
    rc=1
  fi
  if ! wait "$pid"; then
    verdict "FAIL HARNESS MIC $tag: C920 capture failed ($name/c920-$mode.wav.record.txt)"
    rc=1
  fi
  fexpect "$name/$tag.txt" '^result=ok$' "MIC $tag: app completed replay" || rc=1
  fexpect "$name/$tag.txt" '^output_device=Speaker$' "MIC $tag: replay routed to the tablet Speaker" || rc=1
  fexpect "$name/$tag.txt" '^playback_complete=yes$' "MIC $tag: player completion callback" || rc=1
  return "$rc"
}

mic_check_block() (
  local repo=$1 input=${2:-raw} tools device name results stage archive sha mode marker label rc=0
  local boot bootargs unattended=0 cdhash tcc=tcc attempt
  local replay_seconds=18
  local -a flags
  if [ "${DRY:-0}" = 1 ]; then
    verdict 'INFO MIC: dry run, GUI recording and playback skipped'
    return 0
  fi
  case "$input" in
    raw) device='^input_device=Mic$' ;;
    noise-reduced) device='^input_device=Mic [(]noise reduced[)]$' ;;
    *) verdict "FAIL HARNESS MIC: unknown input choice $input"; return 1 ;;
  esac

  umask 077
  results=$(mktemp -d "$R/mic-check.XXXXXXXX") || return 1
  name=${results##*/}
  verdict "INFO MIC: evidence directory $results"
  tools=$repo/tooling/native/piano-mic-check
  # shellcheck source=mic-tcc-seed.sh
  source "$tools/mic-tcc-seed.sh" || return 1
  # These variables and substitutions run on the remote shell.
  # shellcheck disable=SC2016
  rssh "$(declare -f mic_check_bootargs)"$'\n''
    boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid) &&
    args=$(mic_check_bootargs) &&
    printf "MIC_BOOT=%s\nMIC_BOOTARGS=%s\n" "$boot" "$args"
  ' </dev/null > "$results/boot.txt" 2>&1 || return 1
  boot=$(sed -n 's/^MIC_BOOT=//p' "$results/boot.txt")
  if ! [[ "$boot" =~ ^[0-9A-Fa-f-]{36}$ ]] || ! grep -aq '^MIC_BOOTARGS=' "$results/boot.txt"; then
    verdict "FAIL HARNESS MIC: tablet boot receipt missing ($name/boot.txt)"
    return 1
  fi
  bootargs=$(sed -n 's/^MIC_BOOTARGS=//p' "$results/boot.txt")
  if mic_check_boot_enabled "$bootargs"; then
    unattended=1
  fi

  archive=$results/app.tgz
  trap 'rm -f -- "$archive"' EXIT
  if ! /usr/bin/codesign --verify --strict "$repo/out/piano-mic-check/PianoMicCheck.app" > "$results/build.txt" 2>&1 ||
     ! COPYFILE_DISABLE=1 /usr/bin/tar -czf "$archive" -C "$repo/out/piano-mic-check" PianoMicCheck.app; then
    verdict "FAIL HARNESS MIC: build and sign PianoMicCheck.app before staging ($name/build.txt)"
    return 1
  fi
  sha=$(/usr/bin/shasum -a 256 < "$archive") || return 1
  sha=${sha%% *}
  # shellcheck disable=SC2016
  stage=$(rssh 'umask 077; d=$(/usr/bin/mktemp -d /private/var/tmp/piano-mic-check.XXXXXXXX) && echo "MIC_STAGE=$d"' </dev/null |
    sed -nE 's@^MIC_STAGE=(/private/var/tmp/piano-mic-check[.][A-Za-z0-9]{8})$@\1@p' | tail -1) || stage=
  if [ -z "$stage" ]; then
    verdict 'FAIL HARNESS MIC: no remote stage directory'
    return 1
  fi
  printf '%s\n' "$stage" > "$results/remote-stage.txt"
  if ! /usr/bin/base64 -i "$archive" | SSH_T=90 rssh "
    umask 077
    cd '$stage' && /usr/bin/base64 -D > app.tgz &&
    sum=\$(/usr/bin/shasum -a 256 < app.tgz) && test \"\${sum%% *}\" = '$sha' &&
    /usr/bin/tar -xzf app.tgz && /usr/bin/codesign --verify --strict PianoMicCheck.app &&
    /bin/rm -f app.tgz && echo MIC_APP_STAGED
  " > "$results/stage.txt" 2>&1 ||
    ! fexpect "$name/stage.txt" '^MIC_APP_STAGED$' 'MIC: signed app and tone staged with verified SHA-256'; then
    verdict "FAIL HARNESS MIC: stage failed ($name/stage.txt)"
    return 1
  fi

  mic_check_gui "$stage" "$name" "$boot" tcc --tcc || return 1
  if [ "$unattended" = 1 ] && grep -aqxE 'mic_status=[02]' "$results/tcc.txt"; then
    cdhash=$(/usr/bin/codesign -d --arch arm64 --verbose=4 "$repo/out/piano-mic-check/PianoMicCheck.app" 2>&1 |
      sed -n 's/^CDHash=//p')
    if ! [[ "$cdhash" =~ ^[0-9a-f]{40}$ ]] || ! mic_check_seed "$tools" "$stage" "$name" "$boot" "$cdhash"; then
      verdict "FAIL HARNESS MIC: could not seed microphone permission ($name/tcc-*.txt)"
      return 1
    fi
    for attempt in 1 2 3; do
      tcc=tcc-after-$attempt
      mic_check_gui "$stage" "$name" "$boot" "$tcc" --tcc || return 1
      if grep -aqx 'mic_status=3' "$results/$tcc.txt"; then
        break
      fi
      sleep 1
    done
  elif [ "$unattended" = 0 ] && [ "${MIC_CHECK_REQUEST_ACCESS:-0}" = 1 ] &&
    grep -aqx 'mic_status=0' "$results/tcc.txt"; then
    tcc=permission
    mic_check_gui "$stage" "$name" "$boot" "$tcc" --request-access || return 1
  fi
  fexpect "$name/$tcc.txt" '^mic_status=3$' 'MIC: console app has microphone permission' || return 1
  fexpect "$name/$tcc.txt" '^result=ok$' 'MIC: authorization probe completed' || return 1

  for mode in quiet tone; do
    flags=()
    if [ "$mode" = tone ]; then
      flags=(--tone)
      [ "$unattended" = 1 ] || flags+=(--playback)
    fi
    mic_check_gui "$stage" "$name" "$boot" "$mode" 4 "$stage/$mode.m4a" ${flags[@]+"${flags[@]}"} || rc=1
    fexpect "$name/$mode.txt" '^result=ok$' "MIC $mode: AVAudioRecorder check completed" || rc=1
    fexpect "$name/$mode.txt" '^mic_status=3$' "MIC $mode: microphone authorized during capture" || rc=1
    fexpect "$name/$mode.txt" "$device" "MIC $mode: selected Piano input is the default device" || rc=1
    fexpect "$name/$mode.txt" '^input_format=[0-9]+ Hz [1-9][0-9]* ch' "MIC $mode: input format reported" || rc=1
    if [ "$mode" = tone ]; then
      fexpect "$name/$mode.txt" '^output_device=Speaker$' 'MIC tone: tablet Speaker is the default output' || rc=1
      fexpect "$name/$mode.txt" '^stimulus_complete=yes$' 'MIC tone: stimulus played through the default output' || rc=1
      fexpect "$name/$mode.txt" '^tone_detected=yes$' 'MIC tone: stimulus found in the saved recording' || rc=1
      if [ "$unattended" = 0 ]; then
        fexpect "$name/$mode.txt" '^playback_complete=yes$' 'MIC tone: saved recording played to completion' || rc=1
      fi
    fi
    if ! fexpect "$name/$mode.txt" '^recording_published=yes$' "MIC $mode: saved M4A produced" ||
       ! pull "$name/$mode.m4a" "$stage/$mode.m4a" ||
       [ ! -s "$results/$mode.m4a" ]; then
      verdict "FAIL MIC $mode: no non-empty verified recording ($name)"
      rc=1
      continue
    fi
    verdict "PASS MIC $mode: verified non-empty recording ($name/$mode.m4a)"
    python3 "$repo/tooling/native/piano-mic-check/analyze.py" "$results/$mode.m4a" --mode "$mode" --seconds 4 \
      > "$results/$mode.grade.txt" 2>&1 || rc=1
    if [ "$mode" = tone ]; then
      marker='^tone_detected=yes$'
      label='MIC tone: host detects the stimulus in the pulled M4A'
    else
      marker='^noise_floor_sane=yes$'
      label='MIC quiet: host measures a sane noise floor'
    fi
    fexpect "$name/$mode.grade.txt" "$marker" "$label" || rc=1
    fexpect "$name/$mode.grade.txt" '^grade=pass$' "MIC $mode: host grade passed" || rc=1
  done
  if [ "$unattended" = 1 ] && [ "$rc" = 0 ]; then
    for mode in quiet tone; do
      mic_check_replay_take "$repo/tooling/native/audio-listen.py" "$stage" "$name" "$boot" "$mode" "$replay_seconds" ||
        return 1
    done
    python3 "$tools/analyze.py" "$results/c920-tone.wav" --mode replay --control "$results/c920-quiet.wav" \
      --seconds "$replay_seconds" > "$results/replay.grade.txt" 2>&1 || rc=1
    fexpect "$name/replay.grade.txt" '^grade=pass$' 'MIC replay: C920 hears the saved tone above the quiet take' || rc=1
  fi
  return "$rc"
)
