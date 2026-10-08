"""Check the convention tool and the real guest-display PNG writer."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("marks", ROOT / "tools/replay_marks.py")
marks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(marks)


def expect_error(fn, kind=ValueError):
    try:
        fn()
    except kind:
        return
    raise AssertionError("invalid input accepted")


def replay(path, version=3, frames=4000):
    extension = b""
    for tag, value in ((0x302, b"cd_speed=1\n"), (0x306, bytes(4)), (0x307, bytes(4))):
        extension += struct.pack("<II", tag, len(value)) + value + bytes(-len(value) % 4)
    path.write_bytes(struct.pack("<8sIIIII", f"PSXRTI{version}".encode() + b"\0",
                                version, 12, frames, 0, len(extension)) + extension + bytes(frames * 12))


def png(path):
    raw = path.read_bytes()
    assert raw[:8] == b"\x89PNG\r\n\x1a\n"
    offset, pixels, dimensions = 8, b"", None
    while offset < len(raw):
        size, kind = struct.unpack_from(">I4s", raw, offset)
        data = raw[offset + 8:offset + 8 + size]
        assert zlib.crc32(kind + data) & 0xffffffff == struct.unpack_from(">I", raw, offset + 8 + size)[0]
        if kind == b"IHDR":
            dimensions = struct.unpack_from(">II", data)
        if kind == b"IDAT":
            pixels += data
        offset += size + 12
    assert offset == len(raw)
    return dimensions, zlib.decompress(pixels)


with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    for version, fps in ((3, 60), (4, 50)):
        path = root / f"v{version}.psxrpl"
        replay(path, version)
        before = hashlib.sha256(path.read_bytes()).hexdigest()
        result = marks.derive(path, fps)
        assert result["replay_sha256"] == before == hashlib.sha256(path.read_bytes()).hexdigest()
        data = json.loads(Path(result["sidecar"]).read_text(encoding="utf-8"))
        assert data["marks"] == [{"frame": 4000 - 30 * fps, "label": "gameplay", "estimated": True}]
        assert "not observed" in result["meaning"]
        original = Path(result["sidecar"]).read_bytes()
        expect_error(lambda: marks.derive(path, fps), FileExistsError)
        assert Path(result["sidecar"]).read_bytes() == original
    short = root / "short.psxrpl"
    replay(short, frames=100)
    expect_error(lambda: marks.derive(short, 60))
    assert not Path(str(short) + ".marks.json").exists()
    expect_error(lambda: marks.derive(short, 55))
    expect_error(lambda: marks.derive(short, 60, 0))
    short.write_bytes(b"bad")
    expect_error(lambda: marks.replay_frames(short))
    replay(short)
    short.write_bytes(short.read_bytes()[:-1])
    expect_error(lambda: marks.replay_frames(short))
    if len(sys.argv) == 2:
        run = subprocess.run([sys.argv[1], str(root)], capture_output=True,
                             text=True, encoding="utf-8", errors="replace")
        assert run.returncode == 0, (run.returncode, run.stdout, run.stderr)
        assert png(root / "display.png") == ((3, 2), bytes([0,10,20,30,11,20,30,12,20,30,
                                                            0,10,21,30,11,21,30,12,21,30]))
        assert png(root / "disabled.png") == ((1, 1), bytes(4))
print("PASS: 3/4 replay end convention, no overwrite, unchanged replay, decoded guest-display PNG")
