#!/usr/bin/env python3
"""Keep generated setup instructions aligned with each host platform."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_setup_host.sh"
STAGER = ROOT / "tools" / "stage_setup_sdk.sh"
README_TEMPLATE = ROOT / "tools" / "new_project_layout" / "templates" / "README.md.in"
WORKFLOW_TEMPLATE = ROOT / "docs" / "ci" / "templates" / "setup-release.yml"


def main() -> None:
    text = PACKAGER.read_text(encoding="utf-8")
    platform_copy = re.search(
        r'case "\$\{ARTIFACT\}" in(?P<body>.*?)\nesac', text, re.DOTALL
    )
    assert platform_copy, "setup README has no platform-specific instructions"
    body = platform_copy.group("body")

    windows = re.search(r"windows-\*\)(?P<body>.*?)\n\s*;;", body, re.DOTALL)
    assert windows, "Windows setup instructions are missing"
    assert "downloads\n   cmake-clang-v1" in windows.group("body")
    assert "local cmake-clang-v1-*.zip" in windows.group("body")

    posix = re.search(r"linux-\*\|macos-\*\)(?P<body>.*?)\n\s*;;", body, re.DOTALL)
    assert posix, "Linux/macOS setup instructions are missing"
    posix_text = posix.group("body")
    for requirement in ("CMake", "Ninja", "Python 3", "C/C++ compiler", "PATH"):
        assert requirement in posix_text, f"POSIX instructions omit {requirement}"
    assert "downloads the toolchain pack" not in posix_text
    assert "cmake-clang-v1" not in posix_text

    stager = STAGER.read_text(encoding="utf-8")
    assert "Linux/macOS use native build tools from PATH" in stager
    template = README_TEMPLATE.read_text(encoding="utf-8")
    assert "uses the same platform build tools as per-title launchers" in template
    assert "shares the portable toolchain used by per-title launchers" not in template
    workflow = WORKFLOW_TEMPLATE.read_text(encoding="utf-8")
    assert "Linux/macOS use native tools from PATH" in workflow
    assert "audit_setup_package_platform_copy.py" in workflow

    print("setup package platform-copy test: PASS")


if __name__ == "__main__":
    main()
