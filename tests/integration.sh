#!/usr/bin/env bash
set -euo pipefail

BIN=${1:?usage: integration.sh <clipbank>}
BIN=$(readlink -f "$BIN")
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/clipbank-it-XXXXXX")

cleanup() {
  if [ "${CLIPBANK_TEST_KEEP:-0}" = "1" ]; then
    echo "integration workspace kept: $WORK"
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

mkdir -p "$WORK/media/nested"
gen "$WORK/media/nested/short.mp4" 5 160x120
gen "$WORK/media/medium.mp4" 90 160x120
gen "$WORK/media/long.mp4" 190 160x120

echo "== init"
"$BIN" init >/dev/null
test -f "$WORK/library/clipbank.db"

echo "== add directory (recursive)"
"$BIN" add "$WORK/media" > "$WORK/add1.log"
grep -q "3 added, 0 skipped, 0 failed" "$WORK/add1.log"

echo "== duplicate add is skipped"
"$BIN" add "$WORK/media" > "$WORK/add2.log"
grep -q "0 added, 3 skipped, 0 failed" "$WORK/add2.log"
"$BIN" add "$WORK/media/long.mp4" > "$WORK/add3.log"
grep -q "0 added, 1 skipped, 0 failed" "$WORK/add3.log"

echo "== bad input exits 4"
printf 'not a video' > "$WORK/bad.mp4"
set +e
"$BIN" add "$WORK/bad.mp4" > "$WORK/bad.log" 2>&1
bad_rc=$?
set -e
test "$bad_rc" -eq 4

echo "== ls --json"
"$BIN" ls --json > "$WORK/ls.json"

echo "== verify --all"
"$BIN" verify --all > "$WORK/verify.log"
grep -q "3/3 run(s) OK" "$WORK/verify.log"
"$BIN" verify --all --json > "$WORK/verify.json"

echo "== manifest"
"$BIN" manifest -o "$WORK/manifest.json" >/dev/null

python3 - "$WORK" <<'PY'
import json
import os
import re
import sqlite3
import sys

work = sys.argv[1]
library = os.path.join(work, "library")
manifest = json.load(open(os.path.join(work, "manifest.json"), encoding="utf-8"))
verify = json.load(open(os.path.join(work, "verify.json"), encoding="utf-8"))
ls = json.load(open(os.path.join(work, "ls.json"), encoding="utf-8"))

assert manifest["schema"] == 1
assert manifest["profile"] == "detect"
assert manifest["profile_params"]["width"] == 1920
assert manifest["profile_params"]["height"] == 1080
assert manifest["profile_params"]["fps_num"] == 30
assert manifest["profile_params"]["segment_seconds"] == 60
assert manifest["profile_params"]["audio"] is False
assert re.match(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2}$", manifest["generated_at"])
assert len(manifest["sources"]) == 3, len(manifest["sources"])
assert len(manifest["clips"]) == 7, len(manifest["clips"])
assert len(ls) == 3

by_source = {}
for clip in manifest["clips"]:
    by_source.setdefault(clip["source_id"], []).append(clip)
counts = sorted(len(v) for v in by_source.values())
assert counts == [1, 2, 4], counts

durations = sorted(clip["end_ms"] - clip["start_ms"] for clip in manifest["clips"])
near = lambda value, target: abs(value - target) <= 100
assert sum(near(d, 5000) for d in durations) == 1, durations
assert sum(near(d, 30000) for d in durations) == 1, durations
assert sum(near(d, 60000) for d in durations) == 4, durations
assert sum(near(d, 10000) for d in durations) == 1, durations

for clip in manifest["clips"]:
    assert clip["width"] == 1920 and clip["height"] == 1080
    assert abs(clip["fps"] - 30.0) < 1e-9
    assert clip["start_ms"] >= 0 and clip["end_ms"] > clip["start_ms"]
    assert len(clip["sha256"]) == 64
    assert os.path.isfile(clip["path"]), clip["path"]
    assert os.path.isfile(os.path.join(manifest["library"], clip["rel_path"]))

for source in manifest["sources"]:
    assert source["vfr"] is False
    assert len(source["sha256"]) == 64
    assert len(source["source_id"]) == 12

db = sqlite3.connect(os.path.join(library, "clipbank.db"))
assert db.execute("PRAGMA journal_mode").fetchone()[0].lower() == "wal"
tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
assert {"sources", "runs", "clips"} <= tables
assert db.execute("SELECT COUNT(*) FROM runs WHERE state='done'").fetchone()[0] == 3
for (run_id,) in db.execute("SELECT run_id FROM runs WHERE state='done'"):
    assert os.path.isfile(os.path.join(library, run_id, "meta.json"))
    assert os.path.isfile(os.path.join(library, run_id, "run.log"))
    assert os.path.isfile(os.path.join(library, run_id, "thumb.jpg"))
    meta = json.load(open(os.path.join(library, run_id, "meta.json"), encoding="utf-8"))
    assert meta["schema"] == 1 and meta["run_id"] == run_id
db.close()

assert verify["ok"] is True
assert len(verify["runs"]) == 3
print("integration assertions: OK")
PY

echo "== integration: PASS"
