"""Host resource limits for retained diagnostics; never infer a guest verdict."""
import subprocess
import time


def wait_budgeted(process, directory, timeout, max_bytes=None, max_files=1000, interval=2,
                  stop_requested=None):
    started=time.monotonic()
    reason=None
    inventory={'bytes':0,'files':0}
    while True:
        exited=process.poll() is not None
        files=[p for p in directory.rglob('*') if p.is_file()]
        sizes=[]
        for p in files:
            try:sizes.append(p.stat().st_size)
            except FileNotFoundError:pass
        inventory={'bytes':sum(sizes),'files':len(sizes)}
        # The callback may return a reason string (e.g. 'harness_stop'); a bare True keeps 'operator_stop'.
        requested=stop_requested() if stop_requested else None
        if requested:reason=requested if isinstance(requested,str) else 'operator_stop'
        elif max_bytes is not None and (inventory['bytes']>max_bytes or inventory['files']>max_files):reason='host_storage_budget'
        elif not exited and time.monotonic()-started>=timeout:reason='host_timeout'
        if reason:
            if not exited:
                process.terminate()
                try:process.wait(timeout=15)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=15)
            break
        if exited:break
        try:process.wait(timeout=min(interval,max(0.01,timeout-(time.monotonic()-started))))
        except subprocess.TimeoutExpired:pass
    return {'exit_code':process.returncode,'stop_reason':reason,'last_inventory':inventory,
            'host_seconds':time.monotonic()-started}
