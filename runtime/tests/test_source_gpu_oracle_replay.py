"""Replay every PS1B-182 oracle GPU log through test_source_gpu_oracle_replay.

The logs hold commands from retail routes, so they stay on the lab share and
are not committed. Set PSX_ORACLE_GPU_LOGS to their folder; without logs the
test is skipped (exit 77).
"""
import lzma, os, shutil, subprocess, sys
from pathlib import Path

DEFAULT = Path("Z:/Share/psxrecomp/evidence/T172/ps1b-182-oracle-gpu-logs-20260925")
exe = sys.argv[1]
logs = Path(os.environ.get("PSX_ORACLE_GPU_LOGS", DEFAULT))
found = sorted(logs.glob("*.tsv.xz")) if logs.is_dir() else []
if not found:
    print(f"SKIP: no oracle GPU logs under {logs}")
    sys.exit(77)
failed = 0
for log in found:
    proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=False)
    with lzma.open(log, "rb") as src:
        try:
            shutil.copyfileobj(src, proc.stdin, 1 << 22)
        finally:
            proc.stdin.close()
    out = proc.stdout.read().decode()
    code = proc.wait()
    print(f"== {log.name}\n{out}", end="")
    failed |= code != 0
sys.exit(1 if failed else 0)
