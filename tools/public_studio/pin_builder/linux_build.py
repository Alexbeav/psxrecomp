"""Linux x64 private product, built and validated on the Linux build host.

The Windows build owns generation. Its generated C is platform-neutral, so the
Linux product reuses the run's workspace byte for byte and compiles it natively
on athena-rocky (Rocky 9, glibc 2.34; Alex 09-29). The pinned cmake-clang-v1
Linux pack was tried first: its Ubuntu 22.04 sysroot needs glibc 2.35 and the
product would not start on Rocky 9. linux_build.sh does the host-side work;
this module moves the run over SSH, runs it and binds the result.
"""
import json
import os
import re
import shlex
import shutil
import subprocess
import uuid
from pathlib import Path, PurePosixPath

try:
    from . import pin_build as pb
except ImportError:
    import pin_build as pb

HOST = os.environ.get("PSX_LINUX_BUILD_HOST", "athena-rocky")
JOBS = 6  # athena runs out of memory above -j8 (athena-ssh-hang-is-oom)
RUNS = "/opt/psxwb"
# The build host's one build lock. Every build there takes it, whoever starts
# it: athena hangs from lack of memory when two builds overlap. Until
# 2026-10-02 Studio took a lock of its own (/opt/psxwb/.build.lock), which kept
# Studio's builds apart from each other and from nobody else's.
LOCK = os.environ.get("PSX_UNIX_BUILD_LOCK", "/var/tmp/athena-build.lock")
SSH =["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20"]
# The Windows product folder is not needed on Linux, and none of the build
# trees are: Linux builds its own emitters and product.
WORK_EXCLUDES = ("build-release", "build-chdr", "build-diagnostic", "psxrecomp/recompiler/build", "*.log")
# What the build host runs: the script, the gate, and the module that takes the
# starts' own files out of the product before it is packed. Each is standard
# library only and runs under the host's Python 3.9.
SHIPPED = ["linux_build.sh", "lean_boot_gate.py", "product_residue.py", "product_licences.py"]
PAYLOAD_MEMBERS = ("inputs", "licenses", "game.toml", "game_options.toml", "disc.cfg", "bios.cfg", "settings.toml")
# Builds are handed between machines through the private NAS, never copied
# machine to machine (Alex, 09-30). One drop folder per run and platform.
DROPS = Path(os.environ.get("PSX_WORKBENCH_DROPS", r"Z:\Share\psxrecomp\workbench-drops"))
NAS_ROOTS = {"athena": ("Z:\\Share", "/mnt/z/Share"), "mac": ("Z:\\Share", "/Volumes/Ulduar/Share")}


def host_path(path, host):
    """The same NAS location as that host mounts it."""
    windows, unix = NAS_ROOTS[host]
    text = str(path)
    if not text.lower().startswith(windows.lower()):
        raise pb.BuildStop(f"{path} is not on the NAS share, so {host} cannot see it.")
    return unix + text[len(windows):].replace("\\", "/")


def mac_path(path):
    return host_path(path, "mac")


def ssh(command, **kwargs):
    return subprocess.run([*SSH, HOST, command], **kwargs)


def owned_run_path(remote):
    """Reject anything except one immediate Studio run child of RUNS."""
    root, path = PurePosixPath(RUNS), PurePosixPath(remote)
    if (not root.is_absolute() or ".." in root.parts or str(root) != RUNS
            or str(path) != remote or path.parent != root
            or not re.fullmatch(r"studio-run-[A-Za-z0-9_-]{1,120}\.[A-Za-z0-9]{10}", path.name)):
        raise pb.BuildStop("Refusing a Linux build folder outside its owned run boundary.")


def create_owned_run(run_id):
    """Exclusive creation and a per-invocation marker prove ownership; names do not."""
    if not isinstance(run_id, str) or not re.fullmatch(r"run-[A-Za-z0-9_-]{1,120}", run_id):
        raise pb.BuildStop("Invalid Linux build run ID.")
    template = f"{RUNS}/studio-{run_id}.XXXXXXXXXX"
    owned_run_path(template)
    token = uuid.uuid4().hex
    root = shlex.quote(RUNS)
    command = (f'test "$(readlink -e -- {root})" = {root} && '
               f'root_id=$(stat -Lc %d:%i -- {root}) && '
               f'run=$(mktemp -d -- {shlex.quote(template)}) && '
               f'printf %s {token} > "$run/.studio-owner" && '
               'printf "%s\\n" "$run" "$root_id" "$(stat -Lc %d:%i -- "$run")"')
    done = ssh(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if done.returncode:
        raise pb.BuildStop(f"{HOST} could not create an owned build folder in {RUNS}: {done.stderr.strip()}")
    fields = done.stdout.splitlines()
    if len(fields) != 3 or not all(re.fullmatch(r"[0-9]+:[0-9]+", value) for value in fields[1:]):
        raise pb.BuildStop("The Linux host returned no valid physical run identity.")
    remote, root_id, run_id_on_host = fields
    owned_run_path(remote)
    if not remote.startswith(template[:-10]):
        raise pb.BuildStop("The created Linux folder does not match this build's run ID.")
    return remote, {"token": token, "root": RUNS, "root_identity": root_id,
                    "run_identity": run_id_on_host}


def remove_owned_run(remote, ownership):
    """Remove only the exact directory created by this invocation, with its marker intact."""
    owned_run_path(remote)
    if not isinstance(ownership, dict) or ownership.get("root") != RUNS:
        raise pb.BuildStop("Invalid Linux build ownership root.")
    token, root_id, run_id = (ownership.get(key) for key in ("token", "root_identity", "run_identity"))
    if (not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{32}", token)
            or not all(isinstance(value, str) and re.fullmatch(r"[0-9]+:[0-9]+", value)
                       for value in (root_id, run_id))):
        raise pb.BuildStop("Invalid Linux build ownership identity.")
    folder, marker = shlex.quote(remote), shlex.quote(f"{remote}/.studio-owner")
    root = shlex.quote(RUNS)
    command = (f'test "$(readlink -e -- {root})" = {root} && '
               f'test "$(stat -Lc %d:%i -- {root})" = {root_id} && '
               f'test "$(readlink -e -- {folder})" = {folder} && '
               f'test "$(stat -Lc %d:%i -- {folder})" = {run_id} && test -d {folder} && '
               f'test ! -L {marker} && test "$(cat -- {marker})" = {token} && rm -rf -- {folder}')
    done = ssh(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if done.returncode:
        raise pb.BuildStop(f"{HOST} refused cleanup of the owned build folder {remote}.")


def publish_drop(remote_archive, run_id, platform):
    """Have the build host copy its archive to the run's NAS drop; return the Windows path."""
    drop = DROPS / run_id / platform
    target = host_path(drop, "athena")
    done = ssh(f'mkdir -p "{target}" && cp "{remote_archive}" "{target}/"', capture_output=True, text=True)
    if done.returncode:
        raise pb.BuildStop(f"{HOST} could not publish the {platform} archive to {drop}: {done.stderr.strip()}")
    return drop


def push_tree(source, remote, members=None, excludes=()):
    """Stream a tree to the host with tar, preserving bytes."""
    names = [m for m in (members or ["."]) if (Path(source) / m).exists()]
    argv = ["tar", "-C", str(source), *[f"--exclude={e}" for e in excludes], "-cf", "-", *names]
    with subprocess.Popen(argv, stdout=subprocess.PIPE) as tar:
        done = ssh(f'mkdir -p "{remote}" && tar -C "{remote}" -xf -', stdin=tar.stdout)
        tar.stdout.close()
    if tar.returncode or done.returncode:
        raise pb.BuildStop(f"Could not copy {Path(source).name} to {HOST}.")


def pull_tree(remote, target, excludes=()):
    target.mkdir(parents=True, exist_ok=True)
    skip = " ".join(f"--exclude='{e}'" for e in excludes)
    with subprocess.Popen([*SSH, HOST, f'tar -C "{remote}" {skip} -cf - .'], stdout=subprocess.PIPE) as tar:
        done = subprocess.run(["tar", "-C", str(target), "-xf", "-"], stdin=tar.stdout)
        tar.stdout.close()
    if tar.returncode or done.returncode:
        raise pb.BuildStop(f"Could not copy the Linux results back from {HOST}.")


# The lines of a Unix build's build.ninja that say whether netplay is compiled in and which
# framework pin and lobby version are stamped. The host script writes them beside its result.
COMPILE_NAMES = "PSX_HAS_RECOMP_NET|PSX_HAS_LOBBY_CLIENT|PSX_FRAMEWORK_PIN|PSX_GAME_VERSION"


def compile_facts(lines_file, platform, receipt):
    """netplay, framework pin and lobby version of a Unix build, from its own compile lines.

    None when the host script wrote no such file. A Unix build whose netplay differs from the
    Windows build of the same run gets a warning: the kit is the switch for every platform.
    """
    try:
        from . import workbench_output
    except ImportError:
        import workbench_output
    try:
        text = Path(lines_file).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    facts = workbench_output.compile_facts(text)
    windows = (receipt.get("lean_compile") or {}).get("netplay")
    if windows and facts["netplay"] != windows:
        receipt.setdefault("warnings", []).append(
            f"Netplay is {facts['netplay']} in the {platform} build and {windows} in the Windows build of the "
            "same kit. The kit is the switch; find what the other host's build lacks.")
    return facts


def licence_record(path, platform, receipt):
    """What the build host's licence step did to licenses/toolchain (product_licences.py), with a warning
    for a statically linked library or a bundled program that has no text. None from a script before the step."""
    try:
        record = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if record.get("missing"):
        receipt.setdefault("warnings", []).append(
            f"The {platform} product has no licence text for: " + ", ".join(record["missing"])
            + ". It links or bundles them (PS1B-361).")
    return record


def provenance_identity(result):
    """The three BUILDINFO fields a Windows product also has, for a Unix result."""
    facts = result.get("compile") or {}
    return {"netplay": facts.get("netplay"), "framework_pin_stamped": facts.get("framework_pin"),
            "lobby_version": facts.get("lobby_version")}


def runner(remote, script, settings=""):
    """The command the build host runs for one build: `script` under the host's build lock.

    A Windows checkout may carry CRLF, so an LF copy is what bash runs.
    `settings` are further `NAME=value ` pairs for the script.
    """
    return (f'cd "{remote}" && tr -d "\\r" < tools/{script} > tools/run.sh && '
            f'RUN="{remote}" JOBS={JOBS} {settings}flock {LOCK} bash tools/run.sh')


def build(work, windows_payload, dest, evidence, receipt, run_id):
    """Build, validate and publish the Linux archive into `dest`."""
    tools = Path(__file__).resolve().parent
    pb.record_inputs([tools / name for name in SHIPPED], receipt)
    probe = ssh("true", capture_output=True, timeout=60)
    if probe.returncode:
        raise pb.BuildStop(f"Linux builds need SSH to {HOST}, which did not answer.")
    # Retained failures and other lanes are never swept. Delete only our
    # exclusively created directory after its archive is verified.
    remote, ownership = create_owned_run(run_id)
    receipt["linux_remote_run"] = {"path": remote, "ownership": ownership, "cleanup": "retained"}
    push_tree(work, f"{remote}/work", excludes=WORK_EXCLUDES)
    push_tree(windows_payload, f"{remote}/payload", members=PAYLOAD_MEMBERS)
    push_tree(tools, f"{remote}/tools", members=SHIPPED)
    try:
        pb.run_command([*SSH, HOST, runner(remote, "linux_build.sh")], evidence, os.environ.copy(), evidence,
                       receipt, "linux-build")
    finally:
        # Logs and boot receipts come back whether or not the build passed.
        pull_tree(f"{remote}/out", evidence / "linux", excludes=("*.tar.gz",))
    result = json.loads((evidence / "linux/linux-result.json").read_text(encoding="utf-8"))
    result["compile"] = compile_facts(evidence / "linux/linux-compile-lines.txt", "Linux", receipt)
    result["licences"] = licence_record(evidence / "linux/linux-licences.json", "Linux", receipt)
    if result["boot_gate"].get("status") != "passed":
        raise pb.BuildStop("The Linux headless starts did not pass. Read evidence/linux/linux-boots.")
    drop = publish_drop(f"{remote}/out/{result['archive']['name']}", run_id, "linux-x64")
    archive = drop / result["archive"]["name"]
    if pb.sha(archive) != result["archive"]["sha256"]:
        raise pb.BuildStop("The Linux archive on the NAS does not match the one athena built.")
    dest.mkdir()
    shutil.copy2(archive, dest / archive.name)
    remove_owned_run(remote, ownership)
    receipt["linux_remote_run"]["cleanup"] = "removed"
    result["host"], result["nas_drop"] = HOST, str(drop)
    receipt["linux"] = result
    return result


def write_provenance(dest, receipt):
    """BUILDINFO and SHA256SUMS beside the archive; the archive is the product."""
    linux, pin = receipt["linux"], receipt["pin"]
    info = {"schema": 2, "build_route": "workbench_private_lean", "platform": "linux-x64",
            "runtime_commit": pin["commit"], "runtime_tree": pin["tree"], "ui_commit": pin["recomp_ui_ref"],
            "build_host": linux["build_host"], "pin_snapshot": receipt.get("pin_snapshot"),
            "serials": [d["serial"] for d in receipt["disc_identities"]], "input_mode": receipt["input_mode"],
            "archive": linux["archive"], "executable": linux["executable"],
            "executable_sha256": linux["executable_sha256"], "overlay_compiler": linux["overlay_compiler"],
            **provenance_identity(linux),
            "boot_gate": linux["boot_gate"], "minimum_glibc": linux["build_host"]["glibc"],
            "full_game_correctness": "NOT_CLAIMED",
            "kit": receipt.get("kit"), "kit_review": receipt.get("kit_review"),
            "install": f"tar -xzf {linux['archive']['name']} && ./{linux['executable']}/{linux['executable']}"}
    pb.write_json(dest / "BUILDINFO.json", info)
    sums = pb.inventory(dest)
    (dest / "SHA256SUMS.txt").write_text("".join(f"{item['sha256']}  {item['path']}\n" for item in sums), encoding="utf-8")
