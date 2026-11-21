#!/usr/bin/env python3
"""Start `clipbank add`, wait for a stage plus an artifact, then SIGKILL it.

Usage:
  watch_kill.py <clipbank> <library> <source> <run_id> <state> <artifact|-> <log>
Exit 0 only if the process was killed while the requested stage was observed.
"""

import os
import signal
import sqlite3
import subprocess
import sys
import time

binary, library, source, run_id, state_target, artifact, log_path = sys.argv[1:8]

log = open(log_path, "wb")
process = subprocess.Popen(
    [binary, "add", "--library", library, source],
    stdout=log,
    stderr=subprocess.STDOUT,
)

db = sqlite3.connect(os.path.join(library, "clipbank.db"), timeout=0.5)
db.isolation_level = None
deadline = time.time() + 300
killed = False
state = None

while time.time() < deadline:
    try:
        row = db.execute("SELECT state FROM runs WHERE run_id = ?", (run_id,)).fetchone()
    except sqlite3.Error:
        row = None
    if row is not None:
        state = row[0]
        if state == state_target:
            artifact_ready = artifact == "-" or (
                os.path.exists(artifact) and os.path.getsize(artifact) > 0
            )
            if artifact_ready:
                os.kill(process.pid, signal.SIGKILL)
                killed = True
                break
    if process.poll() is not None:
        break
    time.sleep(0.001)

return_code = process.wait()
log.close()
print(f"killed={killed} rc={return_code} state={state}")
sys.exit(0 if killed else 1)
