"""macOS x64 private product: cross-built on athena, validated on Pacifica.

Like Linux, the Mac product reuses the Windows run's generated sources byte for
byte. athena-rocky cross-compiles them with osxcross (Alex 09-29); Pacifica,
the Intel Mac, builds nothing and only runs the two headless starts
(pacifica-test-station-only). Transport helpers come from linux_build.
"""
import json
import os
import shutil
import subprocess
from pathlib import Path

try:
    from . import linux_build as lb
    from . import pin_build as pb
except ImportError:
    import linux_build as lb
    import pin_build as pb

OSXCROSS = "/opt/osxcross/target"
TEST_HOST = os.environ.get("PSX_MAC_TEST_HOST", "macbook")  # Pacifica
TEST_ROOT = "psxwb-test"  # under Pacifica's $HOME; only the newest two runs are kept


def on_mac(command, **kwargs):
    return subprocess.run([*lb.SSH, TEST_HOST, command], **kwargs)


def build(work, windows_payload, dest, evidence, receipt, run_id):
    """Cross-build on athena, validate on Pacifica, publish the archive into `dest`."""
    tools = Path(__file__).resolve().parent
    shipped = ["mac_build.sh", "toolchain-macos-osxcross.cmake", "product_licences.py"]
    pb.record_inputs([tools / name for name in shipped] + [tools / "lean_boot_gate.py"], receipt)
    for host, probe in ((lb.HOST, lb.ssh(f"test -x {OSXCROSS}/bin/osxcross-conf", capture_output=True, timeout=120)),
                        (TEST_HOST, on_mac("true", capture_output=True, timeout=120))):
        if probe.returncode:
            raise pb.BuildStop(f"macOS builds need {host} over SSH (and osxcross on {lb.HOST}); it did not answer.")
    remote, ownership = lb.create_owned_run(f"{run_id}-mac")
    receipt["mac_remote_run"] = {"path": remote, "ownership": ownership, "cleanup": "retained"}
    lb.push_tree(work, f"{remote}/work", excludes=lb.WORK_EXCLUDES)
    lb.push_tree(windows_payload, f"{remote}/payload", members=lb.PAYLOAD_MEMBERS)
    lb.push_tree(tools, f"{remote}/tools", members=shipped)
    runner = lb.runner(remote, "mac_build.sh", f"OSXCROSS={OSXCROSS} ")
    local = evidence / "macos"
    try:
        pb.run_command([*lb.SSH, lb.HOST, runner], evidence, os.environ.copy(), evidence, receipt, "mac-build")
    finally:
        lb.pull_tree(f"{remote}/out", local, excludes=("*.tar.gz",))
    result = json.loads((local / "mac-result.json").read_text(encoding="utf-8"))
    result["compile"] = lb.compile_facts(local / "mac-compile-lines.txt", "macOS", receipt)
    result["licences"] = lb.licence_record(local / "mac-licences.json", "macOS", receipt)
    # The NAS is the handoff (Alex, 09-30): athena publishes the archive there,
    # Pacifica tests it from there and Eagle copies it from there.
    drop = lb.publish_drop(f"{remote}/out/{result['archive']['name']}", run_id, "macos-x64")
    archive = drop / result["archive"]["name"]
    if pb.sha(archive) != result["archive"]["sha256"]:
        raise pb.BuildStop("The macOS archive on the NAS does not match the one athena built.")
    lb.remove_owned_run(remote, ownership)
    receipt["mac_remote_run"]["cleanup"] = "removed"
    shutil.copy2(tools / "lean_boot_gate.py", drop / "lean_boot_gate.py")
    result["boot_gate"] = validate_on_mac(drop, result, local, run_id)
    dest.mkdir()
    shutil.copy2(archive, dest / archive.name)
    result["host"], result["test_host"], result["nas_drop"] = lb.HOST, TEST_HOST, str(drop)
    receipt["macos"] = result
    return result


def send(path, remote):
    """Copy one file to the test Mac over SSH; `remote` is a path under its home folder."""
    with open(path, "rb") as stream:
        done = on_mac(f'cat > "$HOME/{remote}"', stdin=stream)
    if done.returncode:
        raise pb.BuildStop(f"Could not copy {Path(path).name} to {TEST_HOST} over SSH.")


def fetch(remote, names, target):
    """Bring `names` of the test Mac's folder `remote` (under its home) into `target`. A name that
    is not there is skipped: the caller reads what came back."""
    Path(target).mkdir(parents=True, exist_ok=True)
    listed = " ".join(f'"{name}"' for name in names)
    command = f'cd "$HOME/{remote}" && tar -cf - $(ls -d {listed} 2>/dev/null)'
    with subprocess.Popen([*lb.SSH, TEST_HOST, command], stdout=subprocess.PIPE) as tar:
        subprocess.run(["tar", "-C", str(target), "-xf", "-"], stdin=tar.stdout)
        tar.stdout.close()


def gate_script(test, source, name, exe):
    """What the test Mac runs: unpack the archive and run the gate's two starts. `source` holds
    the archive and lean_boot_gate.py and receives boots/ and gate.log: the NAS drop as the Mac
    mounts it, or `.` for the test folder itself."""
    return (f'cd "$HOME/{test}" && tar -xzf "{source}/{name}" && '
            f'/usr/bin/python3 -c "import os,sys; sys.path.insert(0,\'{source}\'); import lean_boot_gate as g; '
            f'from pathlib import Path; g.run_lean_boots(Path(\'{exe}\').resolve(), \'{exe}\', '
            f'Path(\'{source}/boots\').resolve(), Path(\'cwd\').resolve(), os.environ.copy())" '
            f'> "{source}/gate.log" 2>&1')


def validate_on_mac(drop, result, local, run_id):
    """Unpack the archive on Pacifica and run the same two headless starts as Windows.

    The NAS drop is the handoff: the Mac reads the archive from its own mount
    and writes the evidence there. A Mac without the share mounted (found
    2026-10-02: the build then stopped and named a gate.log that nobody had
    written) gets the same archive and the same gate over SSH, and the
    evidence is brought back to the drop. The gate record says which.
    """
    test = f"{TEST_ROOT}/{run_id}"
    nas = lb.mac_path(drop)
    exe, name = result["executable"], result["archive"]["name"]
    on_mac(f'mkdir -p {TEST_ROOT} && (cd {TEST_ROOT} && ls -1t | tail -n +2 | xargs rm -rf); '
           f'rm -rf "{test}" && mkdir -p "{test}/cwd"', check=False)
    shared = on_mac(f'test -r "{nas}/{name}"', capture_output=True, timeout=120).returncode == 0
    if shared:
        # Evidence is written straight to the NAS drop, so nothing is copied back.
        on_mac(gate_script(test, nas, name, exe), timeout=1800)
    else:
        send(drop / name, f"{test}/{name}")
        send(drop / "lean_boot_gate.py", f"{test}/lean_boot_gate.py")
        on_mac(gate_script(test, ".", name, exe), timeout=1800)
        fetch(test, ["boots", "gate.log"], drop)
    receipt_path = drop / "boots" / "lean-boot-receipt.json"
    gate = json.loads(receipt_path.read_text(encoding="utf-8")) if receipt_path.is_file() else {}
    if (drop / "gate.log").is_file():
        shutil.copy2(drop / "gate.log", local / "gate.log")
    if (drop / "boots").is_dir():
        shutil.copytree(drop / "boots", local / "boots", dirs_exist_ok=True)
    if gate.get("status") != "passed":
        where = f"Read {drop}\\gate.log." if (drop / "gate.log").is_file() else \
            f"No gate.log came back from {TEST_HOST}: the gate did not run there."
        raise pb.BuildStop(f"The macOS headless starts on {TEST_HOST} did not pass. " + where)
    on_mac(f'rm -rf "{test}"', check=False)
    gate["archive_transport"] = "the NAS share on the test Mac" if shared else \
        "SSH (the test Mac has no NAS share mounted)"
    return gate


def write_provenance(dest, receipt):
    """BUILDINFO and SHA256SUMS beside the archive; the archive is the product."""
    mac, pin = receipt["macos"], receipt["pin"]
    info = {"schema": 2, "build_route": "workbench_private_lean", "platform": "macos-x64",
            "runtime_commit": pin["commit"], "runtime_tree": pin["tree"], "ui_commit": pin["recomp_ui_ref"],
            "build_host": mac["build_host"], "test_host": mac["test_host"],
            "pin_snapshot": receipt.get("pin_snapshot"),
            "serials": [d["serial"] for d in receipt["disc_identities"]], "input_mode": receipt["input_mode"],
            "archive": mac["archive"], "executable": mac["executable"],
            "executable_sha256": mac["executable_sha256"], "overlay_compiler": mac["overlay_compiler"],
            **lb.provenance_identity(mac),
            "boot_gate": mac["boot_gate"], "full_game_correctness": "NOT_CLAIMED",
            "kit": receipt.get("kit"), "kit_review": receipt.get("kit_review"),
            "install": f"tar -xzf {mac['archive']['name']} && ./{mac['executable']}/{mac['executable']}"}
    pb.write_json(dest / "BUILDINFO.json", info)
    sums = pb.inventory(dest)
    (dest / "SHA256SUMS.txt").write_text("".join(f"{item['sha256']}  {item['path']}\n" for item in sums), encoding="utf-8")
