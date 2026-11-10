"""Line-delimited JSON bridge for the native Studio host; no UI dependencies."""
from dataclasses import asdict
import json
import os
from pathlib import Path
import sys
import traceback
import threading
import settings
from selection import group_files
from backend import Runner, build_batch


def emit(value):
    print(json.dumps(value, ensure_ascii=True), flush=True)


def main():
    request = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    root = Path(request["data_root"])
    if request.get("cache_root"):
        os.environ["WORKBENCH_PUBLIC_CACHE_ROOT"] = request["cache_root"]
    action = request["action"]
    preferences = settings.load(root)
    if action == "settings":
        emit(dict(event="settings", settings=asdict(preferences)))
        return
    if action == "save":
        preferences = settings.Settings(**request["settings"])
        settings.save(root, preferences)
        emit(dict(event="settings", settings=asdict(preferences)))
        return
    jobs = group_files(request["files"], request.get("multiple", False))
    if action == "selection":
        emit(dict(event="selection", games=[dict(id=i, name=g.name, discs=[str(p) for p in g.discs], state="queued", elapsed_seconds=0) for i,g in enumerate(jobs)]))
        return
    if action == "artwork":
        import artwork
        for index, game in enumerate(jobs):
            try:
                serial = artwork.detect(game.discs[0])
                image = artwork.cover(serial, root) if serial else None
                emit(dict(event="artwork", index=index, serial=serial, path=str(image) if image else ""))
            except Exception:
                pass  # Artwork is optional and never changes build readiness.
        return
    runner = Runner(log=lambda line: emit(dict(event="log", text=str(line))), event=emit)
    # Build payloads live independently of the extracted application folder.
    persistent = Path(os.environ.get("LOCALAPPDATA", str(root))) / "RPT"
    os.environ.setdefault("WORKBENCH_PUBLIC_CACHE_ROOT", str(persistent / "s"))
    os.environ.setdefault("WORKBENCH_PUBLIC_WORK_ROOT", str(persistent / "w"))
    os.environ.setdefault("WORKBENCH_SHARED_COMPILER_ROOT", str(persistent / "compilers"))
    finished = threading.Event()
    def cancel_watch():
        while not finished.wait(.1):
            if (root / "cancel-build").exists():
                runner.cancel()
                return
    watcher = threading.Thread(target=cancel_watch, daemon=True)
    watcher.start()
    try:
        result = build_batch(jobs, preferences, root, runner)
        emit(dict(event="result", result=result))
    finally:
        finished.set()
        watcher.join()



if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        emit(dict(event="error", text=str(error)))
        raise SystemExit(1)
