"""Replay every PS1B-182 oracle GPU log through test_source_gpu_oracle_replay.

The logs hold retail command streams (including A0h texture data), so they are
never committed. Set PSX_ORACLE_GPU_LOG_DIR to their folder. Without it, or with
no logs there, the test exits 77 and CTest reports it SKIPPED, never passed.
"""
import lzma, os, shutil, subprocess, sys
from pathlib import Path

exe = sys.argv[1]
where = os.environ.get("PSX_ORACLE_GPU_LOG_DIR", "")
logs = Path(where) if where else None
found = sorted(logs.glob("*.tsv.xz")) if logs and logs.is_dir() else []
if not found:
    print("SKIP: PSX_ORACLE_GPU_LOG_DIR unset or holds no *.tsv.xz logs")
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
