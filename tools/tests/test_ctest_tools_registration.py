"""Check the generated CTest tree, including commands and skip protection."""
import argparse
import json
import subprocess
from pathlib import Path

FILES = (
    "psx_chd", "menu_scan", "cursor_writers", "gpu_frame", "pad_trace",
    "pad_delivery", "aligned_lzss_banks", "static_fragment_variant_keys",
    "mips_tagged_relocations", "overlay_widescreen_callbacks", "duckstation_oracle",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-tree", required=True)
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--without-chd", action="store_true")
    args = parser.parse_args()
    result = subprocess.run(
        [args.ctest, "--test-dir", args.build_tree, "--show-only=json-v1"],
        check=True, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    tests = {test["name"]: test for test in json.loads(result.stdout)["tests"]}
    errors = []
    for stem in FILES:
        if stem == "psx_chd" and args.without_chd:
            continue
        name = "tools_" + stem
        test = tests.get(name)
        if test is None:
            errors.append(name + ": missing from generated tree")
            continue
        command = test.get("command", [])
        files = [i for i, arg in enumerate(command)
                 if Path(arg).name == "test_" + stem + ".py"]
        if len(files) != 1:
            errors.append(name + ": does not run its test file")
        elif command[files[0] + 1:] != ["-v"]:
            errors.append(name + ": does not run the complete test file")
        properties = {prop["name"]: prop["value"] for prop in test.get("properties", [])}
        if properties.get("DISABLED") or properties.get("WILL_FAIL"):
            errors.append(name + ": disables or inverts the real verdict")
        if "skipped" not in str(properties.get("FAIL_REGULAR_EXPRESSION", "")):
            errors.append(name + ": skipped cases can pass")
        if stem == "psx_chd" and not any(
            item.startswith("PSXRECOMP_LIBCHDR=") and item.split("=", 1)[1]
            for item in properties.get("ENVIRONMENT", [])
        ):
            errors.append(name + ": no configured CHD reader")
    if errors:
        raise SystemExit("\n".join(errors))
    print("Generated CTest tool entries and real verdicts checked")


if __name__ == "__main__":
    main()
