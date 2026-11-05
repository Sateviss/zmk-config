#!/usr/bin/env bash
# Quiet GitHub Actions helper for this repo's ZMK build workflow.
#
#   ci.sh wait [ref]      wait for the push-triggered run of ref (default HEAD) to finish;
#                         prints ONE line, plus a short triage if it failed.
#                         Run it with Bash run_in_background:true -> you are notified on exit.
#   ci.sh dispatch [br]   start a workflow_dispatch run on branch br (default current), then wait.
#   ci.sh fail <run-id>   triage a failed run: failed jobs/steps + matching error lines.
#   ci.sh grep <run-id> <regex> [job-substring]
#                         grep the FULL log of a run (e.g. a CONFIG_ value from the
#                         "Kconfig file" step) without ever printing the whole log.
#
# Full logs are saved to $CI_LOGS (default ~/zmk-ws/ci-logs) and never printed.
set -uo pipefail
WF=build.yml
CI_LOGS=${CI_LOGS:-$HOME/zmk-ws/ci-logs}; mkdir -p "$CI_LOGS"
ERR_RE='error:|Error:|error [0-9]|undefined reference|^CMake Error|devicetree error|FATAL|::error'

line() { gh run view "$1" --json conclusion,url,headBranch,displayTitle \
  -q '"\(.conclusion|ascii_upcase) run '"$1"' [\(.headBranch)] \(.displayTitle) \(.url)"'; }

wait_run() { # $1 run id
  local st i=0
  while :; do
    st=$(gh run view "$1" --json status -q .status 2>/dev/null) || st=?
    [ "$st" = completed ] && break
    i=$((i+1)); [ $i -gt 120 ] && { echo "TIMEOUT run $1 still $st after 60 min"; exit 2; }
    sleep 30
  done
  line "$1"
  [ "$(gh run view "$1" --json conclusion -q .conclusion)" = success ] || { fail "$1"; exit 1; }
}

find_run() { # $1 sha, $2 event -> run id (waits up to ~2 min for the run to appear)
  local id i
  for i in $(seq 24); do
    id=$(gh run list -w "$WF" --commit "$1" -e "$2" -L 1 --json databaseId -q '.[0].databaseId')
    [ -n "$id" ] && { echo "$id"; return; }
    sleep 5
  done
  echo "NO RUN for $1 ($2) after 2 min - pushed? workflow file changed?" >&2; exit 2
}

fail() { # $1 run id
  local log=$CI_LOGS/run-$1-failed.log
  echo "-- failed jobs / steps:"
  gh run view "$1" --json jobs -q '.jobs[] | select(.conclusion=="failure") |
    "  \(.name): " + ([.steps[] | select(.conclusion=="failure") | .name] | join(", "))'
  gh run view "$1" --log-failed > "$log" 2>&1 || true
  [ -s "$log" ] || { sleep 10; gh run view "$1" --log-failed > "$log" 2>&1 || true; }
  echo "-- error lines (full failed-step log: $log, $(wc -l < "$log") lines):"
  # log format: job<TAB>step<TAB>timestamp text -> keep "step: text"
  grep -E "$ERR_RE" "$log" | awk -F'\t' '{sub(/^[0-9T:.-]+Z /,"",$3); print "  " $2 ": " $3}' \
    | grep -v 'ninja: build stopped' | awk '!seen[$0]++' | head -"${ERR_LINES:-30}"
}

case "${1:-}" in
  wait)
    sha=$(git rev-parse "${2:-HEAD}") || exit 2
    id=$(find_run "$sha" push) || exit 2
    wait_run "$id" ;;
  dispatch)
    br=${2:-$(git branch --show-current)}
    gh workflow run "$WF" --ref "$br" >/dev/null || exit 2
    sleep 5
    id=$(gh run list -w "$WF" -b "$br" -e workflow_dispatch -L 1 --json databaseId -q '.[0].databaseId')
    [ -n "$id" ] || { echo "NO dispatched run found on $br" >&2; exit 2; }
    wait_run "$id" ;;
  fail) fail "${2:?run id}" ;;
  grep)
    id=${2:?run id}; re=${3:?regex}; log=$CI_LOGS/run-$id-full.log
    [ -s "$log" ] || gh run view "$id" --log > "$log" 2>&1
    grep -E "$re" "$log" | { [ -n "${4:-}" ] && grep -F "$4" || cat; } \
      | awk -F'\t' '{sub(/^[0-9T:.-]+Z /,"",$3); print $1 " | " $3}' | head -"${GREP_LINES:-40}" ;;
  *) sed -n '2,13p' "$0"; exit 2 ;;
esac
