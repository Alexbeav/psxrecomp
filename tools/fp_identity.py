#!/usr/bin/env python3
"""fp_identity.py - A/B check that two runs executed the guest identically.

`run` starts a runtime with no pad input (turbo, headless by default), lets it
reach guest frame N, and saves the debug server's per-frame guest-write
fingerprint ring (`frame_fingerprint`, docs/TCP_COMMANDS.md) with the
dispatch-miss count. `compare` judges two such dumps on the guest-fact
columns. Any title works: give the launch as a command template or as
runtime + game.toml + disc. The runtime needs PSX_DEBUG_TOOLS=ON.

Run a build against itself first (A/A) to show the run is deterministic, then
the candidate against the base (A/B). Warm vs cold overlay cache on R4, from
the game repo's root:

  T=psxrecomp/tools/fp_identity.py
  L='tools/run_r4.sh {build} --debug-port {port} {headless}'
  python3 $T snapshot build /tmp/seed-warm        # after a warm-up run
  mkdir -p /tmp/seed-cold                         # empty seed = cold cache
  python3 $T run warm.json --launch "$L" --port 4781 --frames 12000 --seed /tmp/seed-warm
  python3 $T run cold.json --launch "$L" --port 4781 --frames 12000 --seed /tmp/seed-cold
  python3 $T compare warm.json cold.json

  python3 $T run out.json --runtime build/game-runtime --game build/game.toml \\
      --disc disc/game.cue --port 4781 --frames 6000

--launch placeholders: {build} (--build, default "build"), {port}, {headless}
("--headless", or nothing with --windowed) and {state_dir}. The template is
split like a shell command line but not run through a shell; on Windows a
backslash is a path separator, not an escape, so C:\\psx\\game.exe works
(quote paths with spaces). Arguments after `--` are appended unchanged.
--runtime launches
`RUNTIME --game TOML --disc DISC --no-launcher --debug-port PORT [--headless]`.

Environment: runs set PSX_DEBUG_FMV_QUIET=0 (FMV-quiet would keep MDEC-era
writes out of the judge columns) and PSX_OVERLAY_AUTOCOMPILE_OFF=1 (the
overlay state stays what --seed made it). --fmv-quiet and --autocompile opt
back in; --env K=V adds more. The dump records what the run used.

Overlay state is part of the input: a cold overlay cache runs code in the
interpreter that a warm one runs natively. --seed DIR replaces the state
directory's cache/ and overlay_captures.json(.d) with DIR's before launching
(an empty DIR gives a cold cache). Use the same DIR for both runs of an A/A
pair. The state directory is --state-dir, else the --runtime executable's
directory, else --build under --cwd when the template uses {build}.

Judging (docs/TCP_COMMANDS.md, `frame_fingerprint`):
  judge    cyc mmio mc sp sc wc ws qc  must agree at every shared frame
  locator  wr pc                       reported as where the runs part, never
                                       a failure (host device-service order)
Tolerances, each counted and listed in the output, never silent:
  VBlank straddle  a device write lands on the other side of a snapshot in one
                   run: at one frame ws/wc differ by at most --straddle-writes
                   (default 1) writes and every other judge column agrees; the
                   next shared frame must agree again. A straddle on the last
                   shared frame cannot be confirmed and makes the result
                   INCOMPLETE.
  FMV-quiet shift  only when FMV-quiet was on (or not recorded) in a run: a
                   straddled write that one run counted in qc and the other
                   in wc. wc+qc must still agree; wc, qc and
                   ws are judged against the offsets carried from the shift.
--strict (= --straddle-writes 0) turns both off.

`compare` prints one verdict and exits with its code:
  0 IDENTICAL   same frame set, every judge column equal (tolerances listed),
                0 dispatch misses in both
  1 MISMATCH    a shared frame differs on a judge column, or one run
                fingerprinted frames inside the other's range that the other
                did not
  3 INCOMPLETE  the shared frames agree but identity is not proven: different
                --frames, frames missing at either end of one run, a wrapped
                fingerprint ring, an empty run, no dispatch-miss count, a
                judge column the runtime did not report (runtimes before
                psxrecomp #420 lack ws and qc), or a straddle on the last
                shared frame. Also different FMV-quiet settings once a quiet
                frame occurred (qc > 0, or the frame sets differ): quiet
                frames are not fingerprinted and their writes count only in
                qc, so the frames and columns are not judged at all
  4 MISSES      everything else holds but a run had dispatch misses (resolve
                them before trusting an identity result)
  2             usage error
Every problem found is listed, not only the first. `run` exits 0 once the
dump is written, 3 when it did not write one (the runtime did not start, the
port was taken, the runtime exited, stalled, timed out or dropped the
connection, or seeding or writing failed), and 2 on usage errors. `snapshot`
exits 3 when the copy fails.
"""
import argparse
import json
import os
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from debug_client import query  # noqa: E402

EXIT_IDENTICAL, EXIT_MISMATCH, EXIT_USAGE, EXIT_INCOMPLETE, EXIT_MISSES = 0, 1, 2, 3, 4
HOST = "127.0.0.1"
EXACT = ("cyc", "mmio", "mc", "sp", "sc")   # judged with no tolerance
WRITES = ("wc", "ws", "qc")                 # judged, straddle-aware
JUDGE = EXACT + WRITES
LOCATORS = ("wr", "pc")
HASHES = ("mmio", "sp", "ws", "wr", "pc")
MASK64 = (1 << 64) - 1
RING_CAP = 32768
OVERLAY_STATE = ("cache", "overlay_captures.json", "overlay_captures.json.d")
DEFAULT_ENV = {"PSX_DEBUG_FMV_QUIET": "0", "PSX_OVERLAY_AUTOCOMPILE_OFF": "1"}
SCHEMA = 2


class UsageError(Exception):
    pass


class RunError(Exception):
    pass


# ---- run -------------------------------------------------------------------

def copy_overlay_state(src, dst):
    for name in OVERLAY_STATE:
        target = os.path.join(dst, name)
        if os.path.isdir(target) and not os.path.islink(target):
            shutil.rmtree(target)
        elif os.path.lexists(target):
            os.remove(target)
        source = os.path.join(src, name)
        if os.path.isdir(source):
            shutil.copytree(source, target, symlinks=True)
        elif os.path.exists(source):
            shutil.copy2(source, target)


def resolve_state_dir(args, cwd):
    if args.state_dir:
        return os.path.abspath(os.path.join(cwd, args.state_dir))
    if args.runtime:
        return os.path.dirname(os.path.abspath(os.path.join(cwd, args.runtime)))
    if args.launch and "{build}" in args.launch:
        return os.path.abspath(os.path.join(cwd, args.build))
    return None


def split_template(text, windows=None):
    """Split a --launch template into arguments, without a shell.

    POSIX: shell quoting and backslash escapes (shlex.split). Windows:
    backslashes are path separators, not escapes, so C:\\psx\\game.exe stays
    as written; quote a path with spaces as "C:\\Program Files\\...".
    """
    if windows is None:
        windows = os.name == "nt"
    if not windows:
        return shlex.split(text)
    lex = shlex.shlex(text, posix=True)
    lex.whitespace_split = True
    lex.commenters = ""
    lex.escape = ""
    return list(lex)


def launch_argv(args, state_dir, windows=None):
    headless = [] if args.windowed else ["--headless"]
    if not args.launch:
        return [args.runtime, "--game", args.game, "--disc", args.disc,
                "--no-launcher", "--debug-port", str(args.port),
                *headless, *args.extra]
    try:
        tokens = split_template(args.launch, windows)
    except ValueError as e:
        raise UsageError(f"--launch: {e}")
    if args.windowed and "--headless" in tokens:
        raise UsageError("--windowed, but the --launch template passes "
                         "--headless itself; write {headless} instead")
    values = {"build": args.build, "port": str(args.port),
              "headless": " ".join(headless), "state_dir": state_dir or ""}
    argv = []
    for token in tokens:
        try:
            value = token.format(**values)
        except (KeyError, IndexError, ValueError) as e:
            raise UsageError(f"--launch: bad placeholder in {token!r} ({e!r}); "
                             f"known: {{build}} {{port}} {{headless}} "
                             f"{{state_dir}}")
        if value:
            argv.append(value)
    if not argv:
        raise UsageError("--launch is empty")
    return argv + args.extra


def port_in_use(port):
    try:
        with socket.create_connection((HOST, port), timeout=0.5):
            return True
    except OSError:
        return False


def stop(proc):
    """Stop the launched process group: a launcher script may not exec.

    Returns None, or why the runtime could not be stopped."""
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)],
                           capture_output=True)
        else:
            os.killpg(proc.pid, signal.SIGTERM)
    except OSError:
        pass
    try:
        proc.wait(timeout=15)
        return None
    except subprocess.TimeoutExpired:
        pass
    try:
        if os.name == "nt":
            proc.kill()
        else:
            os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        pass
    try:
        proc.wait(timeout=10)
        return None
    except subprocess.TimeoutExpired:
        return (f"runtime pid {proc.pid} did not exit after SIGKILL; it may "
                f"still hold its port")


def ask(port, obj, timeout):
    return query(HOST, port, obj, timeout=timeout)


def drive(proc, args, log_path):
    """Turbo to frame N; return (fingerprint reply, dispatch_stats reply)."""
    port = args.port
    deadline = time.time() + args.boot_timeout
    while True:
        try:
            ask(port, {"cmd": "turbo", "enabled": 1}, 2)
            break
        except (OSError, ValueError):
            if proc.poll() is not None:
                raise RunError(f"runtime exited (status {proc.returncode}) "
                               f"before its debug server answered on port "
                               f"{port} (see {log_path})")
            if time.time() > deadline:
                raise RunError(f"no debug server on port {port} after "
                               f"{args.boot_timeout}s (built with "
                               f"PSX_DEBUG_TOOLS=ON? does the launch pass the "
                               f"port? see {log_path})")
            time.sleep(0.3)
    start = last_move = time.time()
    last = -1
    while True:
        if proc.poll() is not None:
            raise RunError(f"runtime exited (status {proc.returncode}) at "
                           f"frame {last} of {args.frames} (see {log_path})")
        try:
            frame = int(ask(port, {"cmd": "frame"}, 120).get("frame", 0))
        except (OSError, ValueError):
            frame = last
        if frame >= args.frames:
            break
        now = time.time()
        if frame != last:
            last, last_move = frame, now
        elif now - last_move > args.stall:
            raise RunError(f"stalled: frame {frame} did not advance for "
                           f"{args.stall}s (see {log_path})")
        if now - start > args.timeout:
            raise RunError(f"reached only frame {frame} of {args.frames} in "
                           f"{args.timeout}s (see {log_path})")
        time.sleep(0.2)
    replies = []
    for req, timeout in (({"cmd": "frame_fingerprint",
                           "count": min(args.frames + 1, RING_CAP),
                           "frame_lo": 0, "frame_hi": args.frames}, 300),
                         ({"cmd": "dispatch_stats"}, 120)):
        try:
            reply = ask(port, req, timeout)
        except (OSError, ValueError) as e:
            # A crash, a dropped connection or a socket timeout after frame
            # N: no dump, so INCOMPLETE rather than a traceback.
            try:
                proc.wait(timeout=2)    # a crash drops the socket first
            except subprocess.TimeoutExpired:
                pass
            state = ("exited (status %s)" % proc.returncode
                     if proc.returncode is not None else "did not answer")
            raise RunError(f"runtime {state} on {req['cmd']} after reaching "
                           f"frame {frame} ({e!r}; see {log_path})")
        if not isinstance(reply, dict):
            raise RunError(f"{req['cmd']} replied {reply!r}, not an object")
        if reply.get("ok") is False:
            raise RunError(f"{req['cmd']} failed: {reply.get('error', reply)}")
        replies.append(reply)
    fp, stats = replies
    if "entries" not in fp:
        raise RunError(f"frame_fingerprint returned no entries: {fp}")
    return fp, stats


def cmd_run(args):
    cwd = os.path.abspath(args.cwd)
    if not args.launch and not (args.runtime and args.game and args.disc):
        raise UsageError("run needs --launch TEMPLATE, or --runtime, --game "
                         "and --disc")
    if args.launch and (args.runtime or args.game or args.disc):
        raise UsageError("give --launch or --runtime/--game/--disc, not both")
    state_dir = resolve_state_dir(args, cwd)
    if args.seed:
        if not state_dir:
            raise UsageError("--seed needs --state-dir (or --runtime, or "
                             "{build} in --launch)")
        if not os.path.isdir(args.seed):
            raise UsageError(f"--seed {args.seed} is not a directory")
        if not os.path.isdir(state_dir):
            raise UsageError(f"state directory {state_dir} does not exist")
    out = os.path.abspath(args.out)
    if not os.path.isdir(os.path.dirname(out)):
        raise UsageError(f"output directory {os.path.dirname(out)} does not "
                         f"exist")
    argv = launch_argv(args, state_dir)
    env = dict(os.environ)
    record = dict(DEFAULT_ENV)
    if args.fmv_quiet:
        record["PSX_DEBUG_FMV_QUIET"] = "1"
    if args.autocompile:
        record["PSX_OVERLAY_AUTOCOMPILE_OFF"] = "0"
    for kv in args.env:
        if "=" not in kv:
            raise UsageError(f"--env {kv!r}: expected KEY=VALUE")
        key, value = kv.split("=", 1)
        record[key] = value
    env.update(record)
    if port_in_use(args.port):
        raise RunError(f"port {args.port} already has a listener; pick a free "
                       f"--port so the run cannot query another runtime")
    if args.seed:
        copy_overlay_state(args.seed, state_dir)
    log_path = out + ".log"
    label = args.label or (args.build if args.launch else args.runtime)
    with open(log_path, "w") as log:
        try:
            proc = subprocess.Popen(argv, cwd=cwd, env=env, stdout=log,
                                    stderr=subprocess.STDOUT,
                                    start_new_session=(os.name != "nt"))
        except OSError as e:
            raise RunError(f"cannot launch {argv[0]}: {e}")
        failure = None
        try:
            fp, stats = drive(proc, args, log_path)
        except RunError as e:
            failure = str(e)
        except BaseException:
            stop(proc)
            raise
        stuck = stop(proc)
        if stuck:
            failure = f"{failure}; {stuck}" if failure else stuck
        if failure:
            raise RunError(failure)
    entries = fp.get("entries", [])
    # Write then rename: an interrupted write never leaves a partial dump.
    tmp = out + ".tmp"
    try:
        with open(tmp, "w") as f:
            json.dump({"schema": SCHEMA, "build": label,
                       "frames": args.frames, "launch": argv, "cwd": cwd,
                       "state_dir": state_dir,
                       "seed": os.path.abspath(args.seed) if args.seed
                       else None,
                       "env": record, "port": args.port,
                       "miss_total": stats.get("miss_total"),
                       "miss_unique": stats.get("miss_unique"),
                       "ring_total": fp.get("total"),
                       "ring_available": fp.get("available"),
                       "entries": entries}, f)
        os.replace(tmp, out)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise
    print(f"{label}: {len(entries)} frames fingerprinted to frame "
          f"{args.frames}, dispatch misses total={stats.get('miss_total')} "
          f"unique={stats.get('miss_unique')} -> {out}")
    return EXIT_IDENTICAL


def cmd_snapshot(args):
    if not os.path.isdir(args.state_dir):
        raise UsageError(f"{args.state_dir} is not a directory")
    os.makedirs(args.seed_dir, exist_ok=True)
    copy_overlay_state(args.state_dir, args.seed_dir)
    kept = [n for n in OVERLAY_STATE
            if os.path.lexists(os.path.join(args.seed_dir, n))]
    print(f"overlay state of {args.state_dir} -> {args.seed_dir} "
          f"({', '.join(kept) if kept else 'empty: a cold seed'})")
    return EXIT_IDENTICAL


# ---- compare ---------------------------------------------------------------

def num(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def load_run(path):
    try:
        with open(path) as f:
            run = json.load(f)
    except (OSError, ValueError) as e:
        raise UsageError(f"cannot read {path}: {e}")
    if not isinstance(run, dict) or not isinstance(run.get("entries", []), list):
        raise UsageError(f"{path} is not an fp_identity dump")
    frames, present = {}, None
    try:
        for e in run.get("entries", []):
            cols = {k: num(e[k]) for k in JUDGE + LOCATORS if k in e}
            frames[num(e["frame"])] = cols
            present = set(cols) if present is None else present & set(cols)
    except (KeyError, TypeError, ValueError) as e:
        raise UsageError(f"{path}: malformed fingerprint entry ({e!r})")
    return run, frames, present or set()


def show(cols):
    """One entry's columns, hashes in hex as the server prints them."""
    return " ".join(f"{k}={v:#018x}" if k in HASHES else f"{k}={v}"
                    for k, v in cols.items())


def fmv_quiet(run):
    """True/False as the run set it; None when the dump did not record it."""
    value = (run.get("env") or {}).get("PSX_DEBUG_FMV_QUIET")
    return None if value is None else value[:1] != "0"


def quiet_text(setting):
    return {True: "on", False: "off", None: "not recorded (runtime default: on)"}[setting]


def frame_ranges(frames, limit=8):
    """Sorted frame numbers as compact runs: '1..100, 205, 300..310'."""
    runs = []
    for frame in frames:
        if runs and frame == runs[-1][1] + 1:
            runs[-1][1] = frame
        else:
            runs.append([frame, frame])
    text = [f"{lo}" if lo == hi else f"{lo}..{hi}" for lo, hi in runs[:limit]]
    if len(runs) > limit:
        text.append(f"... (+{len(runs) - limit} more runs)")
    return ", ".join(text)


def scan(common, fa, fb, judged, quiet, limit):
    """Walk the shared frames in order and judge them; see the module doc."""
    exact = [c for c in EXACT if c in judged]

    def offset(f):
        a, b = fa[f], fb[f]
        return (a["wc"] - b["wc"] if "wc" in judged else 0,
                a["qc"] - b["qc"] if "qc" in judged else 0,
                (a["ws"] - b["ws"]) & MASK64 if "ws" in judged else 0)

    def exact_diff(f):
        return [c for c in exact if fa[f][c] != fb[f][c]]

    def moved(d, ref):
        """Writes a straddle moved across the snapshot (0 = none)."""
        if quiet:
            return abs((d[0] + d[1]) - (ref[0] + ref[1]))
        return abs(d[0] - ref[0]) if d[1] == ref[1] else 0

    def shift(d, ref, cap):
        return (quiet and d != ref and d[0] + d[1] == ref[0] + ref[1]
                and 1 <= abs(d[0] - ref[0]) <= cap)

    base = (0, 0, 0)
    out = {"straddles": [], "shifts": [], "diverge": None, "unconfirmed": None}
    for i, f in enumerate(common):
        xd, d = exact_diff(f), offset(f)
        if not xd and d == base:
            continue
        note = None
        last = i + 1 == len(common)
        if not xd and limit and shift(d, base, limit):
            if last:
                out["unconfirmed"] = (f, "an FMV-quiet shift")
                break
            out["shifts"].append((f, abs(d[0] - base[0])))
            base = d
            continue
        m = 0 if xd else moved(d, base)
        if m and limit and m <= limit:
            if last:
                out["unconfirmed"] = (f, f"a {m}-write VBlank straddle")
                break
            g = common[i + 1]
            dn = offset(g)
            if not exact_diff(g) and (dn == base or shift(dn, base, limit)):
                out["straddles"].append((f, m))
                continue
            note = (f"a {m}-write straddle pattern, but frame {g} did not "
                    f"re-converge")
        elif m and limit:
            note = f"{m} writes moved, more than --straddle-writes {limit}"
        elif not xd and not limit and (m == 1 or shift(d, base, 1)):
            note = ("the one-write VBlank straddle pattern; --strict rejects "
                    "it")
        cols = xd + [c for c, x, y in zip(("wc", "qc", "ws"), d, base)
                     if x != y and c in judged]
        out["diverge"] = (f, cols, note)
        break
    return out


def compare(a_path, b_path, limit):
    loaded = {"A": load_run(a_path), "B": load_run(b_path)}
    runs = {tag: v[0] for tag, v in loaded.items()}
    fps = {tag: v[1] for tag, v in loaded.items()}
    present = {tag: v[2] for tag, v in loaded.items()}
    fa, fb = fps["A"], fps["B"]
    mismatch, incomplete, misses, notes = [], [], [], []
    quiet_set = {tag: fmv_quiet(runs[tag]) for tag in runs}

    for tag, path in (("A", a_path), ("B", b_path)):
        run, frames = runs[tag], fps[tag]
        span = f"{min(frames)}..{max(frames)}" if frames else "none"
        env = run.get("env") or {}
        print(f"{tag}: {path} build={run.get('build')} "
              f"frames={run.get('frames')} fingerprinted={len(frames)} "
              f"({span}) misses={run.get('miss_total')} "
              f"fmv_quiet={quiet_text(quiet_set[tag])} "
              f"autocompile_off={env.get('PSX_OVERLAY_AUTOCOMPILE_OFF', '?')} "
              f"seed={run.get('seed', '?')}")
        if not frames:
            incomplete.append(f"{tag} has no fingerprint entries")
        total, kept = run.get("ring_total"), run.get("ring_available")
        if total is not None and kept is not None and total > kept:
            incomplete.append(f"{tag}'s fingerprint ring wrapped ({total} "
                              f"snapshots, {kept} kept): its earliest frames "
                              f"were overwritten")
        if run.get("miss_total") is None:
            incomplete.append(f"{tag} has no dispatch-miss count")
        elif run["miss_total"]:
            misses.append(f"{tag} had {run['miss_total']} dispatch misses "
                          f"({run.get('miss_unique')} unique)")
        lacking = [c for c in JUDGE if c not in present[tag]]
        if frames and lacking:
            incomplete.append(f"{tag} lacks judge column(s) "
                              f"{','.join(lacking)} (runtimes before psxrecomp "
                              f"#420 report no ws/qc): not judged")

    if runs["A"].get("frames") != runs["B"].get("frames"):
        incomplete.append(f"the runs stopped at different frames: A at "
                          f"{runs['A'].get('frames')}, B at "
                          f"{runs['B'].get('frames')}")

    judged = [c for c in JUDGE if c in present["A"] and c in present["B"]]
    common = sorted(set(fa) & set(fb))
    quiet = any(q is not False for q in quiet_set.values())
    effective = {t: quiet_set[t] is not False for t in quiet_set}
    # FMV-quiet frames are not fingerprinted and their writes count only in
    # qc. With the setting different in the two runs, once a quiet frame has
    # happened the frame sets and every write column part for that reason
    # alone: a DIVERGE or a missing frame would be an artefact, not a fork.
    comparable = True
    if effective["A"] != effective["B"]:
        settings = (f"A {quiet_text(quiet_set['A'])}, "
                    f"B {quiet_text(quiet_set['B'])}")
        signs = [f"{t} counted quiet writes (qc > 0)" for t in fps
                 if "qc" in judged and any(fps[t][f]["qc"] for f in fps[t])]
        if set(fa) != set(fb):
            signs.append(f"the frame sets differ ({len(fa)} and {len(fb)} "
                         f"frames fingerprinted)")
        if "qc" not in judged:
            signs.append("qc is not reported, so quiet frames cannot be "
                         "ruled out")
        if signs:
            comparable = False
            incomplete.append(
                f"the runs used different FMV-quiet settings ({settings}) "
                f"and {'; '.join(signs)}: quiet frames are not fingerprinted "
                f"and their writes count only in qc, so frames and columns "
                f"were not judged. Rerun both with the same setting (the "
                f"default is off)")
        else:
            notes.append(f"FMV-quiet settings differ ({settings}), but no "
                         f"quiet frame occurred (qc = 0, same frame set): "
                         f"judged as usual")

    if comparable:
        for tag, other_tag in (("A", "B"), ("B", "A")):
            only = sorted(set(fps[tag]) - set(fps[other_tag]))
            other = fps[other_tag]
            if not only:
                continue
            if other:
                lo, hi = min(other), max(other)
                inside = [f for f in only if lo <= f <= hi]
                outside = [f for f in only if not lo <= f <= hi]
                where = f"outside its range {lo}..{hi}"
            else:
                inside, outside, where = [], only, "(it has none)"
            if inside:
                mismatch.append(f"{len(inside)} frames fingerprinted by {tag} "
                                f"are missing inside {other_tag}'s range: "
                                f"{frame_ranges(inside)}")
            if outside:
                incomplete.append(f"{other_tag} lacks {len(outside)} of "
                                  f"{tag}'s frames {where}: "
                                  f"{frame_ranges(outside)}")
        if quiet:
            why = ", ".join(f"{t} {quiet_text(q)}"
                            for t, q in quiet_set.items())
            notes.append(f"FMV-quiet ({why}): "
                         f"wc+qc must agree; wc, qc and ws are judged against "
                         f"the offsets carried from any FMV-quiet shift. Rerun "
                         f"without --fmv-quiet for the strict rule")
        else:
            notes.append("FMV-quiet off in both runs: wc, qc and ws judged "
                         "directly")
        result = scan(common, fa, fb, judged, quiet, limit)
    else:
        result = {"straddles": [], "shifts": [], "diverge": None,
                  "unconfirmed": None}

    if result["diverge"]:
        f, cols, note = result["diverge"]
        text = f"DIVERGE at frame {f}: judge columns {cols}"
        if note:
            text += f" ({note})"
        if result["shifts"]:
            text += (f" [after {len(result['shifts'])} FMV-quiet shift(s); "
                     f"wc/qc/ws compared against the carried offsets]")
        text += (f"\n    A {show(fa[f])}\n    B {show(fb[f])}"
                 f"\n    next: arm record_frame {f} in both runs and diff the "
                 f"ordered logs")
        mismatch.insert(0, text)
    if result["unconfirmed"]:
        f, what = result["unconfirmed"]
        incomplete.append(f"frame {f}, the last shared frame, differs on "
                          f"ws/wc/qc in the pattern of {what}, but no later "
                          f"frame shows the runs re-converge; rerun with a "
                          f"larger --frames")

    tolerated = []
    if result["straddles"]:
        n = len(result["straddles"])
        most = max(m for _, m in result["straddles"])
        tolerated.append(f"{n} VBlank straddle frame(s) (ws/wc off by <= "
                         f"{most} write(s), re-converged at the next frame): "
                         f"{frame_ranges([f for f, _ in result['straddles']])}")
    if result["shifts"]:
        tolerated.append(f"{len(result['shifts'])} FMV-quiet shift(s) (wc+qc "
                         f"agreed, offsets carried forward): "
                         f"{frame_ranges([f for f, _ in result['shifts']])}")
    notes += [f"tolerated: {t}" for t in tolerated]

    for loc in LOCATORS:
        if not comparable:
            notes.append(f"locator {loc}: not compared (FMV-quiet settings "
                         f"differ)")
            continue
        if loc not in present["A"] or loc not in present["B"]:
            notes.append(f"locator {loc}: not reported by both runs")
            continue
        parted = [f for f in common if fa[f][loc] != fb[f][loc]]
        notes.append(f"locator {loc}: " + (
            f"first differs at frame {parted[0]} ({len(parted)} of "
            f"{len(common)} frames); not a failure: device writes are "
            f"recorded in host service order" if parted
            else "equal at every shared frame"))

    shared = (f"{len(common)} shared frames ({common[0]}..{common[-1]})"
              if common else "no shared frames")
    if mismatch:
        verdict, code = "MISMATCH", EXIT_MISMATCH
    elif incomplete:
        verdict, code = "INCOMPLETE", EXIT_INCOMPLETE
    elif misses:
        verdict, code = "MISSES", EXIT_MISSES
    else:
        verdict, code = "IDENTICAL", EXIT_IDENTICAL
    if code == EXIT_IDENTICAL:
        tol = ("; tolerated " + " and ".join(
            f"{len(result[k])} {what}" for k, what in
            (("straddles", "VBlank straddle(s)"),
             ("shifts", "FMV-quiet shift(s)")) if result[k])
            if tolerated else "; no tolerance applied")
        print(f"IDENTICAL: {len(common)} frames ({common[0]}..{common[-1]}) "
              f"agree on {','.join(judged)}{tol}")
    elif comparable:
        print(f"{verdict}: {shared}, judged on {','.join(judged) or 'nothing'}")
    else:
        print(f"{verdict}: {shared}, not judged (FMV-quiet settings differ)")
    for problem in mismatch + incomplete + misses:
        print(f"  {problem}")
    for note in notes:
        print(f"  {note}")
    return code


def cmd_compare(args):
    limit = 0 if args.strict else args.straddle_writes
    if limit < 0:
        raise UsageError("--straddle-writes must be >= 0")
    return compare(args.a, args.b, limit)


# ---- command line ------------------------------------------------------------

class Parser(argparse.ArgumentParser):
    def error(self, message):
        self.print_usage(sys.stderr)
        raise UsageError(message)


def parser():
    top = Parser(prog="fp_identity.py", description=__doc__,
                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = top.add_subparsers(dest="command", parser_class=Parser)

    snap = sub.add_parser("snapshot", help="copy a state dir's overlay state "
                          "into a seed dir")
    snap.add_argument("state_dir")
    snap.add_argument("seed_dir")
    snap.set_defaults(func=cmd_snapshot)

    run = sub.add_parser("run", help="launch, turbo to frame N, dump the "
                         "fingerprint ring")
    run.add_argument("out", help="dump to write (OUT.log gets the runtime log)")
    run.add_argument("--launch", help="command template, e.g. "
                     "'tools/run_game.sh {build} --debug-port {port} {headless}'")
    run.add_argument("--build", default="build", help="value of {build}")
    run.add_argument("--runtime", help="runtime executable (instead of "
                     "--launch)")
    run.add_argument("--game", help="game.toml for --runtime")
    run.add_argument("--disc", help="disc image for --runtime")
    run.add_argument("--cwd", default=".", help="working directory of the "
                     "launch (default: current)")
    run.add_argument("--state-dir", help="directory holding cache/ and "
                     "overlay_captures.json")
    run.add_argument("--seed", help="overlay state to install before launching")
    run.add_argument("--port", type=int, default=4370)
    run.add_argument("--frames", type=int, default=6000)
    run.add_argument("--label", help="name recorded in the dump")
    run.add_argument("--windowed", action="store_true",
                     help="no --headless (renderer-dependent checks)")
    run.add_argument("--fmv-quiet", action="store_true",
                     help="leave FMV-quiet on (PSX_DEBUG_FMV_QUIET=1)")
    run.add_argument("--autocompile", action="store_true",
                     help="allow overlay autocompile "
                     "(PSX_OVERLAY_AUTOCOMPILE_OFF=0)")
    run.add_argument("--env", action="append", default=[],
                     metavar="KEY=VALUE", help="extra environment (repeatable)")
    run.add_argument("--boot-timeout", type=float, default=180,
                     help="seconds for the debug server to answer")
    run.add_argument("--stall", type=float, default=120,
                     help="seconds the frame counter may stand still")
    run.add_argument("--timeout", type=float, default=3600,
                     help="seconds to reach frame N")
    run.set_defaults(func=cmd_run)

    cmp_ = sub.add_parser("compare", help="judge two dumps")
    cmp_.add_argument("a")
    cmp_.add_argument("b")
    cmp_.add_argument("--straddle-writes", type=int, default=1,
                      help="most writes one VBlank straddle may move "
                      "(default 1; 0 = strict)")
    cmp_.add_argument("--strict", action="store_true",
                      help="no tolerance: same as --straddle-writes 0")
    cmp_.set_defaults(func=cmd_compare)
    return top


def main(argv):
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    top = parser()
    args = None
    try:
        args = top.parse_args(argv)
        if not getattr(args, "func", None):
            top.print_help()
            return EXIT_USAGE
        args.extra = extra
        if extra and args.command != "run":
            raise UsageError("arguments after -- only apply to run")
        return args.func(args)
    except UsageError as e:
        print(f"fp_identity.py: error: {e}", file=sys.stderr)
        return EXIT_USAGE
    except RunError as e:
        print(f"fp_identity.py: run failed: {e}", file=sys.stderr)
        return EXIT_INCOMPLETE
    except (OSError, subprocess.SubprocessError) as e:
        # Seeding, the log or the dump could not be written: no result, and
        # never exit 1, which means MISMATCH.
        what = getattr(args, "command", None) or "command"
        print(f"fp_identity.py: {what} failed: {e}", file=sys.stderr)
        return EXIT_INCOMPLETE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
