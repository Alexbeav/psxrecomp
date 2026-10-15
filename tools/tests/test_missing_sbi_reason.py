"""Execute the launcher's exact reason expression with synthetic identities."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def scratch_root():
    # Use WORKBENCH_RUN_ROOT only when that folder exists; else the default temp folder.
    root = os.environ.get("WORKBENCH_RUN_ROOT")
    return root if root and os.path.isdir(root) else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (args.source or root / "runtime/src/main.cpp").read_text(encoding="utf-8")
    body = source.split("int ae_disc_verify(", 1)[1].split("int ae_memcard_inspect(", 1)[0]
    match = re.search(r"const char\* why =\s*(.*?);\s*char row\[", body, re.S)
    assert match, "launcher reason expression missing"
    # The SBI File row is OK only for a mounted disc with its SBI loaded.
    # runtime/tests/test_disc_identity.cpp holds the warning to the same test
    # (PS1B-495); this pins the row's side of it to the source.
    assert "(id.toc_opened && id.sbi_loaded) ? RECOMP_SBI_OK : RECOMP_SBI_MISSING" in body, \
        "the SBI File row no longer reads toc_opened && sbi_loaded"
    program = '''
#include <cassert>
#include <cstring>
#include <string>
struct Identity {
    std::string sbi_warning, detail, netplay_detail;
    bool expected_serial_given = false, serial_matches = true;
};
struct Companion { bool ready = true; std::string message; };
static const char* reason(const Identity& id, const Companion& companion) {
    return ''' + match[1] + ''';
}
int main() {
    Identity id;
    Companion companion;
    id.sbi_warning = "Missing SBI: SCES-02105";
    id.expected_serial_given = true;
    id.serial_matches = false;
    assert(!std::strcmp(reason(id, companion), "the disc does not carry the expected serial")
           && "wrong serial must retain its blocking reason");
    id.detail = "the image has no ISO header";
    assert(!std::strcmp(reason(id, companion), id.detail.c_str()));
    companion.ready = false;
    companion.message = "required qualified companion missing";
    assert(!std::strcmp(reason(id, companion), companion.message.c_str()));
    companion.ready = true;
    id.detail.clear();
    id.serial_matches = true;
    assert(!std::strcmp(reason(id, companion), id.sbi_warning.c_str()));
    id.sbi_warning.clear();
    id.netplay_detail = "ordinary retained reason";
    assert(!std::strcmp(reason(id, companion), id.netplay_detail.c_str()));
}
'''
    with tempfile.TemporaryDirectory(prefix="missing_sbi_reason_", dir=scratch_root()) as scratch:
        work = Path(scratch)
        (work / "reason.cpp").write_text(program, encoding="utf-8")
        for command in ([args.cxx, "-std=c++17", "-O0", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                         str(work / "reason.cpp"), "-o", str(work / "controls")], [str(work / "controls")]):
            result = subprocess.run(command, cwd=work, text=True, encoding="utf-8",
                                    errors="replace", capture_output=True)
            print(result.stdout, end="")
            print(result.stderr, end="")
            if result.returncode:
                raise RuntimeError(f"exit {result.returncode}: {command}")
    print("MISSING_SBI_REASON_CONTROLS_PASS five actual expression controls")


if __name__ == "__main__":
    main()
