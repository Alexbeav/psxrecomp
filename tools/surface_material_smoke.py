#!/usr/bin/env python3
"""End-to-end smoke test for surface materials (docs/SURFACE_MATERIALS.md).

Needs a running runtime built with PSX_DEBUG_TOOLS=ON and the OpenGL renderer
(any BIOS; no disc). It drives the GPU hardware model through `gp0_inject`:

  1. uploads a 64x64 15bpp test texture into off-screen VRAM (768,0),
  2. draws a textured quad into off-screen VRAM (660..980, 270..500),
  3. reads the texture key back with surface_probe_on + surface_census,
  4. loads a water table for that key, redraws, captures twice (animation),
  5. checks the highlight, clears everything and checks the off render is
     pixel-identical to the first one.

Captures are written as PNGs (Pillow) to --out. Exit code 0 = all checks pass.

  ./runtime/build/PSXRecomp --bios bios/openbios.bin --renderer opengl --no-launcher &
  python3 tools/surface_material_smoke.py --out /tmp/surface_smoke
"""
import argparse
import json
import math
import os
import socket
import sys
import tempfile
import time

X0, Y0, X1, Y1 = 660, 270, 980, 500
TEXPAGE = 0x10C  # page x=768 y=0, 15bpp: off-screen on a 640-wide display


def q(port, obj):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall((json.dumps(obj) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        d = s.recv(65536)
        if not d:
            break
        buf += d
    s.close()
    r = json.loads(buf.decode())
    if not r.get("ok"):
        raise RuntimeError("%s -> %s" % (obj.get("cmd"), r))
    return r


def inject(port, words):
    chunks = [words[i:i + 500] for i in range(0, len(words), 500)]
    for k, c in enumerate(chunks):
        q(port, {"cmd": "gp0_inject", "stage": 1 if k < len(chunks) - 1 else 0,
                 "words": ["0x%X" % w for w in c]})


def texture_words():
    pix = []
    for y in range(64):
        for x in range(64):
            band = 0.5 + 0.5 * math.sin(x * 0.35 + y * 0.2)
            r, g, b = 2 + int(4 * band), 8 + int(10 * band), 18 + int(12 * band)
            if x % 16 == 0 or y % 16 == 0:
                r, g, b = 28, 28, 28
            pix.append((r | (g << 5) | (b << 10)) or 1)
    return [pix[i] | (pix[i + 1] << 16) for i in range(0, len(pix), 2)]


def upload(port):
    inject(port, [0xA0000000, 768, (64 << 16) | 64] + texture_words())


def quad(port):
    inject(port, [0xE1000000 | TEXPAGE | 0x400,
                  0xE3000000 | (256 << 10) | 640, 0xE4000000 | (511 << 10) | 1023, 0xE5000000,
                  0x2C808080, (Y0 << 16) | X0, 0x0000, (Y0 << 16) | X1, (TEXPAGE << 16) | 0x3F,
                  (Y1 << 16) | X0, 0x3F00, (Y1 << 16) | X1, 0x3F3F])


def grab(port):
    w, h = X1 - X0 + 1, Y1 - Y0 + 1
    out = [0] * (w * h)
    for by in range(0, h, 128):
        for bx in range(0, w, 128):
            cw, ch = min(128, w - bx), min(128, h - by)
            r = q(port, {"cmd": "gl_fbo_peek", "x": X0 + bx, "y": Y0 + by, "w": cw, "h": ch})
            hx = r["hex"]
            for i in range(cw * ch):
                out[(by + i // cw) * w + bx + i % cw] = int(hx[i * 4:i * 4 + 4], 16)
    return out


def save(pixels, path):
    try:
        from PIL import Image
    except ImportError:
        return
    w, h = X1 - X0 + 1, Y1 - Y0 + 1
    img = Image.new("RGB", (w, h))
    img.putdata([((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31)
                 for v in pixels])
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4370)
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "surface_smoke"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    p = a.port
    fails = []

    def check(cond, msg):
        print(("ok   " if cond else "FAIL ") + msg)
        if not cond:
            fails.append(msg)

    q(p, {"cmd": "surface_load", "path": ""})
    q(p, {"cmd": "surface_highlight"})
    upload(p)
    quad(p)
    off = grab(p)
    save(off, os.path.join(a.out, "off.png"))

    q(p, {"cmd": "surface_probe_on", "on": 1})
    upload(p)  # tracking starts at probe-on; re-upload like a scene load would
    quad(p)
    time.sleep(0.2)
    census = q(p, {"cmd": "surface_census", "limit": 4})
    keys = [k for k in census["keys"] if k["bbox"] == [X0, Y0, X1, Y1]]
    check(len(keys) == 1, "census finds exactly one key for the test quad")
    if not keys:
        return 1
    key = keys[0]["key"]
    probe = q(p, {"cmd": "surface_probe", "x": (X0 + X1) // 2, "y": (Y0 + Y1) // 2})
    check(any(h["key"] == key and h["status"] == "found" for h in probe["hits"]),
          "probe at the quad centre reports " + key)

    table = os.path.join(a.out, "water.toml")
    with open(table, "w") as f:
        f.write('format_version = 1\n[materials.smoke_water]\nkind = "water"\n'
                'ripple_amplitude = 1.5\nripple_scale = 0.35\nripple_speed = 2.0\n'
                'flow = [2.0, 0.5]\nglint = 0.6\nglint_power = 20\n'
                'tint = [0.8, 0.95, 1.0]\ntint_mix = 0.3\n'
                '[[surface]]\nkey = "%s-*"\nmaterial = "smoke_water"\n' % key.split("-")[0])
    q(p, {"cmd": "surface_load", "path": table})
    quad(p)
    on1 = grab(p)
    time.sleep(0.5)
    quad(p)
    on2 = grab(p)
    save(on1, os.path.join(a.out, "water_t0.png"))
    save(on2, os.path.join(a.out, "water_t1.png"))
    check(on1 != off, "water material changes the tagged quad")
    check(on1 != on2, "water animates with guest time")

    q(p, {"cmd": "surface_highlight", "key": key})
    quad(p)
    hl = grab(p)
    save(hl, os.path.join(a.out, "highlight.png"))
    check(hl != off and hl != on1, "highlight paints the key")

    q(p, {"cmd": "surface_highlight"})
    q(p, {"cmd": "surface_load", "path": ""})
    q(p, {"cmd": "surface_probe_on", "on": 0})
    st = q(p, {"cmd": "surface_status"})
    check(st["active"] == 0, "feature inactive after clearing")
    quad(p)
    off2 = grab(p)
    check(off2 == off, "render with the feature off is pixel-identical to the first render")

    print("captures in", a.out)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
