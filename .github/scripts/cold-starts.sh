#!/usr/bin/env bash
# A2.3.2 and A2.3.3 (M2's exit on each platform; AS.3.3): three cold starts of each first-jump
# fixture on a server and its payload, the first navigation timed and held to its limit. The rounds'
# measurements and server logs go to RESULTS; the summary to the job's step summary. A gate, not only
# a table: it exits non-zero when a round's checks failed, a round never navigated, or a median is
# over its limit.
#
#   cold-starts.sh CONFORMANCE SERVER PAYLOAD RESULTS
set -uo pipefail
conformance=$1 server=$2 payload=$3 results=$4
mkdir -p "$results"
for fixture in self-mcpp-first-jump real-xlings-first-jump self-mcppls-first-jump; do
  for round in 1 2 3; do
    work="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/$fixture-$round"
    "$conformance" run --core-engine mcxx --server "$server" --payload "$payload" --fixture "conformance/fixtures/$fixture" \
        --workspace-dir "$work/workspace" --cache-dir "$work/cache" --timeout 300 --measure "$results/$fixture-cold-$round.json" || true
    mkdir -p "$results/logs/$fixture-$round" && cp "$work/cache/logs/"*.log "$results/logs/$fixture-$round/" 2>/dev/null || true
  done
done
py=$(command -v python3 || command -v python)
"$py" - "$results" >> "${GITHUB_STEP_SUMMARY:-/dev/stdout}" <<'PY'
import json, pathlib, statistics, sys
# The criterion each is held to: A2.3.2's 10 s, A2.3.3's baselines (the clangd path).
limits = { "self-mcpp-first-jump": 10.0, "real-xlings-first-jump": 12.3, "self-mcppls-first-jump": 15.1 }
# A gate, not only a table: a round whose checks failed (real-xlings' definitions answered []
# for 300 s went unnoticed while this only measured), or a median over its limit, fails the job.
missed = []
for fixture, limit in limits.items():
    runs = [json.loads(p.read_text()) for p in sorted(pathlib.Path(sys.argv[1]).glob(f"{fixture}-cold-*.json"))]
    print(f"### {fixture} (at most {limit} s)\n\n| round | first navigation (s) | ready (s) | failures |\n|---|---|---|---|")
    for i, r in enumerate(runs, 1):
        print(f"| {i} | {r.get('first-navigation')} | {r.get('ready')} | {r.get('failures')} |")
        if r.get("failures"):
            missed.append(f"{fixture} round {i}: {r.get('failures')} failures")
    known = [r.get("first-navigation") for r in runs if isinstance(r.get("first-navigation"), (int, float))]
    if runs and len(known) == len(runs):
        median = statistics.median(known)
        print(f"\nmedian first navigation: {median:.2f} s ({'within' if median <= limit else 'over'} {limit} s)\n")
        if median > limit:
            missed.append(f"{fixture}: median {median:.2f} s over {limit} s")
    else:
        print("\na round never navigated\n")
        missed.append(f"{fixture}: a round never navigated")
for m in missed:
    print(m, file=sys.stderr)
sys.exit(1 if missed else 0)
PY
