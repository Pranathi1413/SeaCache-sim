#!/bin/bash
# Runs the original SeaCache code and our event-timing code on every matrix in data/,
# and keeps each run's output file in compare/results/original/ and compare/results/event/.
# Everything it makes (results, logs, binaries, the original code) is gitignored,
# so it stays in place when you switch branches.
#
#   compare/run_compare.sh                         run everything (3 runs at a time for the smaller matrices)
#   compare/run_compare.sh 4                       4 runs at a time
#   ONLY="p2p-Gnutella08 vsp" compare/run_compare.sh   only these matrices
#   FORCE=1 compare/run_compare.sh                 re-run matrices that already have results
#
# Runs that already finished are skipped, so it's fine to stop it (ctrl-c) and start it again.
# To stop the Mac from sleeping during a long run: caffeinate -i compare/run_compare.sh
# When it's done (or any time in between): python3 compare/summarize.py

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"              # our code: whatever is checked out, uncommitted changes included
ORIG_BRANCH="origin/original_seacache"  # the authors' code (only the build fixed)
JOBS="${1:-3}"
BIG_MB=200      # matrices bigger than this run one at a time, each run can need several GB of RAM
CXX="${CXX:-g++}"

RESULTS="$HERE/results"
LOGS="$HERE/logs"
BIN="$HERE/bin"
ORIG_SRC="$HERE/original_src"
mkdir -p "$RESULTS/original" "$RESULTS/event" "$LOGS/original" "$LOGS/event" "$BIN"

# ---- 1. get the original code out of its branch (no second clone, no branch switching) ----
git -C "$REPO" rev-parse --verify -q "$ORIG_BRANCH" > /dev/null || git -C "$REPO" fetch origin
rm -rf "$ORIG_SRC" && mkdir -p "$ORIG_SRC"
git -C "$REPO" archive "$ORIG_BRANCH" src config tiles | tar -x -C "$ORIG_SRC" || exit 1

# ---- 2. build both, with the same compiler and flags ----
echo "building original ($(git -C "$REPO" log -1 --format='%h %s' "$ORIG_BRANCH"))"
(cd "$ORIG_SRC" && $CXX -std=c++17 -O3 src/*.cpp -o "$BIN/scache_original") || exit 1
echo "building event ($(git -C "$REPO" branch --show-current) at $(git -C "$REPO" log -1 --format='%h'))"
(cd "$REPO" && $CXX -std=c++17 -O3 src/*.cpp -o "$BIN/scache_event") || exit 1

# remember exactly which code made these results
{
  echo "run started $(date)"
  echo "original: $ORIG_BRANCH $(git -C "$REPO" log -1 --format='%h %s' "$ORIG_BRANCH")"
  echo "event:    $(git -C "$REPO" branch --show-current) $(git -C "$REPO" log -1 --format='%h %s')"
  echo "event uncommitted changes:"
  git -C "$REPO" status --short -- src
} > "$RESULTS/versions.txt"

# ---- 3. a config for each, same settings as that version's own, only the folders changed ----
make_config() {   # config file, tiles folder, version
  python3 - "$1" "$2" "$3" "$RESULTS" "$HERE" <<'EOF'
import json, sys
config, tiles, version, results, here = sys.argv[1:]
c = json.load(open(config))
c["outputDir"] = f"{results}/{version}/"
c["tileDir"] = f"{tiles}/"
json.dump(c, open(f"{here}/config_{version}.json", "w"), indent=4)
EOF
}
make_config "$ORIG_SRC/config/config.json" "$ORIG_SRC/tiles" original
make_config "$REPO/config/config.json" "$REPO/tiles" event

# ---- 4. run ----
# one run: from the repo folder (the simulator looks for data/ there), timed,
# and marked done only if the output file has the final stats in it
run_one() {   # version matrix
  local version=$1 m=$2
  local done_file="$RESULTS/$version/.done_$m"
  if [ -e "$done_file" ] && [ "${FORCE:-0}" != 1 ]; then return; fi
  rm -f "$done_file"

  echo "$(date +%H:%M:%S) start  $version $m"
  local start=$(date +%s)
  (cd "$REPO" && "$BIN/scache_$version" "$m" "$m" "$HERE/config_$version.json") > "$LOGS/$version/$m.log" 2>&1
  local status=$? seconds=$(( $(date +%s) - start ))

  local out=$(ls "$RESULTS/$version/"*"__${m}_${m}_"*.txt 2>/dev/null | head -1)
  if [ "$status" = 0 ] && [ -n "$out" ] && grep -q "hitrate" "$out"; then
    touch "$done_file"
    echo "$(date +%H:%M:%S) done   $version $m (${seconds}s)"
  else
    echo "$(date +%H:%M:%S) FAILED $version $m (exit $status, ${seconds}s, see compare/logs/$version/$m.log)"
  fi
  echo "$m,$version,$status,$seconds" >> "$LOGS/times.csv"
}
export -f run_one
export RESULTS LOGS BIN HERE REPO

# matrices, smallest first
if [ -n "${ONLY:-}" ]; then
  matrices="$ONLY"
else
  matrices=$(ls -Sr "$REPO"/data/*.mtx | xargs -n1 basename | sed 's/\.mtx$//')
fi

small=""; big=""
for m in $matrices; do
  mb=$(( $(stat -f %z "$REPO/data/$m.mtx") / 1048576 ))
  if [ "$mb" -le "$BIG_MB" ]; then small="$small $m"; else big="$big $m"; fi
done

echo "running $(echo $small | wc -w | tr -d ' ') smaller matrices, $JOBS at a time"
for m in $small; do echo "original $m"; echo "event $m"; done |
  xargs -P "$JOBS" -L 1 bash -c 'run_one "$0" "$1"'

echo "running $(echo $big | wc -w | tr -d ' ') big matrices (over ${BIG_MB} MB), one at a time"
for m in $big; do echo "original $m"; echo "event $m"; done |
  xargs -P 1 -L 1 bash -c 'run_one "$0" "$1"'

echo "all done. summary: python3 compare/summarize.py"
