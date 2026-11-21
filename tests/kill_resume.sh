#!/usr/bin/env bash
set -euo pipefail

BIN=${1:?usage: kill_resume.sh <clipbank>}
BIN=$(readlink -f "$BIN")
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/clipbank-kill-XXXXXX")

cleanup() {
  if [ "${CLIPBANK_TEST_KEEP:-0}" = "1" ]; then
    echo "kill/resume workspace kept: $WORK"
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

export CLIPBANK_LIBRARY="$WORK/library"

gen() {
  local out=$1 duration=$2 size=$3
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=${size}:rate=10:duration=${duration}" \
    -c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 30 "$out"
}

dry_run_id() {
  "$BIN" add --dry-run "$1" | sed -n 's/^  run_id: //p'
}

gen "$WORK/normalize.mp4" 60 160x120
gen "$WORK/segment.mp4" 120 176x144

"$BIN" init >/dev/null

echo "== kill during normalize, then resume"
normalize_run=$(dry_run_id "$WORK/normalize.mp4")
test -n "$normalize_run"
python3 "$HERE/watch_kill.py" "$BIN" "$WORK/library" "$WORK/normalize.mp4" \
  "$normalize_run" normalizing \
  "$WORK/library/.tmp/$normalize_run/normalized.mp4" "$WORK/normalize-kill.log"
"$BIN" ls --json > "$WORK/after-normalize.json"
python3 - "$WORK/after-normalize.json" "$normalize_run" <<'PY'
import json, sys
runs = json.load(open(sys.argv[1], encoding="utf-8"))
run = next(r for r in runs if r["run_id"] == sys.argv[2])
assert run["state"] != "done", run
PY
"$BIN" add "$WORK/normalize.mp4" > "$WORK/normalize-resume.log"
grep -q "normalize: ok" "$WORK/normalize-resume.log"
"$BIN" verify --run "$normalize_run" >/dev/null

echo "== kill during segment, then resume"
segment_run=$(dry_run_id "$WORK/segment.mp4")
test -n "$segment_run"
python3 "$HERE/watch_kill.py" "$BIN" "$WORK/library" "$WORK/segment.mp4" \
  "$segment_run" segmenting \
  "$WORK/library/.tmp/$segment_run/clips/0000.mp4" "$WORK/segment-kill.log"
"$BIN" add "$WORK/segment.mp4" > "$WORK/segment-resume.log"
grep -q "segment:" "$WORK/segment-resume.log"
"$BIN" verify --run "$segment_run" >/dev/null

echo "== simulate a crash during hashing, then retry"
python3 - "$WORK/library/clipbank.db" "$normalize_run" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
run_id = sys.argv[2]
db.execute("UPDATE runs SET state='hashing', normalized_sha256=NULL WHERE run_id=?", (run_id,))
db.execute("UPDATE clips SET sha256=NULL WHERE run_id=?", (run_id,))
db.commit()
db.close()
PY
"$BIN" retry "$normalize_run" > "$WORK/hash-resume.log"
grep -q "hash: ok" "$WORK/hash-resume.log"
if grep -q "normalize:" "$WORK/hash-resume.log"; then
  echo "retry re-normalized unexpectedly" >&2
  exit 1
fi
if grep -q "segment:" "$WORK/hash-resume.log"; then
  echo "retry re-segmented unexpectedly" >&2
  exit 1
fi

echo "== final verify"
"$BIN" verify --all > "$WORK/verify.log"
grep -q "2/2 run(s) OK" "$WORK/verify.log"

echo "== kill/resume: PASS"
