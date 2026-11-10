#!/usr/bin/env python3
"""tools/collect_game_misses.py writes seeds in the title's link segment.

A seed is a PC, so it carries a segment (docs/SEGMENT_AWARE_CODE.md §5.3). The
recompiler compiles a seed as an entry only in the image's link segment and
reports any other spelling as a variant request (§5.4), so a tool that wrote
every dirty_ram miss as KSEG0 could never close a KUSEG-linked title's misses
(Kula World, Alien Resurrection). dirty_ram_stats rows are physical; the tool
takes the segment from --segment, --game-toml or a segmented game_text_lo,
refuses to guess from a physical one, and writes `segment_misses` rows with
their full PC. A row in a segment that does not map physical memory
(0x20000000-0x7FFFFFFF, KSEG2) is reported and not written: the recompiler
refuses such a seed. A `bios` row (the compiled BIOS ran a window's home
body for an alias PC) is listed for the BIOS seeds and not written.

A fake debug server answers the two commands the tool sends. Its
`segment_misses` rows use the format string of the runtime's handler, read
from runtime/src/debug_server.c, so the fixture cannot drift from the reply
the tool parses.
"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
TOOL = os.path.join(ROOT, "tools", "collect_game_misses.py")
ROW_FMT = ('{\\"pc\\":\\"0x%08X\\",\\"home\\":\\"0x%08X\\",\\"count\\":%llu,"\n'
           '                            "\\"kind\\":\\"%s\\"}"')

PER_PC = [  # physical, as dirty_ram_stats keys them
    {"pc": "0x000100E4", "hits": 9, "insns": 40},
    {"pc": "0x00010200", "hits": 3, "insns": 12},
    {"pc": "0x000C0000", "hits": 5, "insns": 7},   # outside the text range
]
SEGMENT_MISSES = [(0x800100F0, 0x000100F0, 4, "game"), (0xA0010124, 0x00010124, 1, "game"),
                  # 0x20000000-0x7FFFFFFF does not map physical memory: no seed.
                  (0x200100F0, 0x000100F0, 1, "game"),
                  # A BIOS shell alias inside the text range: a BIOS seed, not a game one.
                  (0x00030000, 0x80030000, 2, "bios")]


class FakeDebugServer(threading.Thread):
    def __init__(self, with_segment_misses=True):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.with_segment_misses = with_segment_misses
        self.stop = False

    def reply(self, cmd):
        if cmd == "dirty_ram_stats":
            return json.dumps({"id": 1, "ok": True, "per_pc": PER_PC})
        if cmd == "segment_misses" and self.with_segment_misses:
            # The runtime's own row format (C printf -> Python %).
            fmt = "".join(p.strip().strip('"') for p in ROW_FMT.split("\n"))
            rows = ",".join(fmt.replace('\\"', '"').replace("%llu", "%d") % r
                            for r in SEGMENT_MISSES)
            return '{"id":1,"ok":true,"total":5,"unique":2,"summary":[%s]}' % rows
        return json.dumps({"id": 1, "ok": False, "error": "unknown command"})

    def run(self):
        while not self.stop:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            with conn:
                data = b""
                while not data.endswith(b"\n"):
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                cmd = json.loads(data.decode())["cmd"]
                conn.sendall(self.reply(cmd).encode() + b"\n")

    def close(self):
        self.stop = True
        self.sock.close()


def check(cond, msg):
    if not cond:
        raise SystemExit("FAIL: " + msg)


def run_tool(port, out, *extra, lo="0x10000", hi="0x98000"):
    return subprocess.run([sys.executable, TOOL, str(port), lo, hi, out] + list(extra),
                          capture_output=True, text=True)


def seeds(path):
    with open(path) as f:
        return [int(l.split()[0], 16) for l in f if l.strip() and not l.startswith("#")]


def main():
    with open(os.path.join(ROOT, "runtime", "src", "debug_server.c"), encoding="utf-8") as f:
        check(ROW_FMT in f.read(), "the runtime's segment_misses row format changed; "
              "update collect_game_misses.py and this fixture together")

    server = FakeDebugServer()
    server.start()
    try:
        with tempfile.TemporaryDirectory() as tmp:
            # KUSEG-linked title, segment given directly.
            out = os.path.join(tmp, "kuseg.txt")
            r = run_tool(server.port, out, "--segment", "kuseg")
            check(r.returncode == 0, "tool failed: %s" % r.stderr)
            got = seeds(out)
            check(got[:2] == [0x000100E4, 0x800100F0] and
                  set(got) == {0x000100E4, 0x00010200, 0x800100F0, 0xA0010124},
                  "KUSEG seeds, segment misses at their full PC, hits first: %s"
                  % ["0x%08X" % s for s in got])
            with open(out) as f:
                text = f.read()
            check("home 0x000100F0 (variant request)" in text,
                  "a segment miss is written as a variant request: %r" % text)
            check("segment miss 0x200100F0 is in a segment that does not map" in r.stdout,
                  "a segment miss outside the physical segments is reported, not "
                  "written: %r" % r.stdout)
            check("BIOS segment miss 0x00030000 (home 0x80030000, 2 hits)" in r.stdout,
                  "a BIOS segment miss is listed for the BIOS seeds, not written: %r"
                  % r.stdout)
            # A second run appends nothing: known seeds are exact PCs.
            r = run_tool(server.port, out, "--segment", "kuseg")
            check(r.returncode == 0 and "No new addresses" in r.stdout and
                  len(seeds(out)) == 4, "a rerun must not duplicate seeds")

            # game.toml with load_address, and with only the EXE header.
            toml_a = os.path.join(tmp, "a.toml")
            with open(toml_a, "w") as f:
                f.write("[game]\nexe = 'none.psx'\nload_address = '0x00010000'\n")
            out = os.path.join(tmp, "toml_a.txt")
            r = run_tool(server.port, out, "--game-toml", toml_a)
            check(r.returncode == 0 and 0x000100E4 in seeds(out),
                  "--game-toml load_address gives KUSEG: %s %s" % (r.stdout, r.stderr))
            exe = os.path.join(tmp, "game.psx")
            header = bytearray(0x800)
            header[0:8] = b"PS-X EXE"
            struct.pack_into("<I", header, 0x18, 0x00010000)
            with open(exe, "wb") as f:
                f.write(bytes(header))
            toml_b = os.path.join(tmp, "b.toml")
            with open(toml_b, "w") as f:
                f.write("[game]\nexe = 'game.psx'\n")
            out = os.path.join(tmp, "toml_b.txt")
            r = run_tool(server.port, out, "--game-toml", toml_b)
            check(r.returncode == 0 and 0x000100E4 in seeds(out),
                  "--game-toml reads the EXE header's segment: %s %s" % (r.stdout, r.stderr))

            # A KSEG0 title spelled through its text range.
            out = os.path.join(tmp, "kseg0.txt")
            r = run_tool(server.port, out, lo="0x80010000", hi="0x80098000")
            check(r.returncode == 0 and
                  {0x800100E4, 0x80010200} <= set(seeds(out)) and
                  0x000100E4 not in seeds(out),
                  "a KSEG0 text range writes KSEG0 seeds: %s" % r.stdout)

            # A physical range alone cannot tell KUSEG from KSEG0.
            out = os.path.join(tmp, "guess.txt")
            r = run_tool(server.port, out)
            check(r.returncode != 0 and "--game-toml" in r.stderr and not os.path.exists(out),
                  "the tool must refuse to guess the segment: %s" % r.stderr)
    finally:
        server.close()

    # An older runtime without segment_misses still yields the dirty_ram seeds.
    server = FakeDebugServer(with_segment_misses=False)
    server.start()
    try:
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "old.txt")
            r = run_tool(server.port, out, "--segment", "kseg0")
            check(r.returncode == 0 and set(seeds(out)) == {0x800100E4, 0x80010200},
                  "no segment_misses command: dirty_ram seeds only: %s" % r.stdout)
    finally:
        server.close()
    print("collect_game_misses test passed")


if __name__ == "__main__":
    main()
