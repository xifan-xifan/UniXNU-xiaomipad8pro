#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
host_job_limit() {
  local limit=${CI_SUBJOBS:-2} cpus
  case $limit in
    [1-4]) ;;
    0*|*[!0-9]*)
      printf 'CI_SUBJOBS must be a positive integer\n' >&2
      return 2
      ;;
    *) limit=4 ;;
  esac
  cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
  [[ $cpus =~ ^[1-9][0-9]*$ ]] || cpus=1
  if (( limit > cpus )); then limit=$cpus; fi
  if [[ ${SELFTEST_SUBJOB_LIMIT:-} =~ ^[1-9][0-9]*$ ]] && (( limit > SELFTEST_SUBJOB_LIMIT )); then
    limit=$SELFTEST_SUBJOB_LIMIT
  fi
  printf '%d\n' "$limit"
}

# Run independent compile-and-test tasks from a wrapper with set -e enabled.
run_host_jobs() {
  local scratch=$1 run=$2
  shift 2
  local limit i test status failed=0
  local -a tests=("$@") pids
  limit=$(host_job_limit)
  if (( limit == 1 )); then
    for test in "${tests[@]}"; do
      "$run" "$test"
    done
    return
  fi

  for i in "${!tests[@]}"; do
    # Bash 3.2 has no wait -n. Refill any free slot, even while the oldest task is still running.
    while (( $(jobs -pr | wc -l) >= limit )); do
      sleep 0.1
    done
    ( "$run" "${tests[i]}" ) > "$scratch/job-$i.log" 2>&1 &
    pids[i]=$!
  done
  # Wait for every task before the caller removes its scratch directory. Replay each diagnostic intact.
  for i in "${!tests[@]}"; do
    if wait "${pids[i]}"; then
      cat "$scratch/job-$i.log"
    else
      status=$?
      printf '%s failed (exit %d)\n' "${tests[i]}" "$status" >&2
      cat "$scratch/job-$i.log" >&2
      if (( failed == 0 )); then failed=$status; fi
    fi
  done
  return "$failed"
}
