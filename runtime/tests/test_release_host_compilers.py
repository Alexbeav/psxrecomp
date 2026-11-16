#!/usr/bin/env python3
"""Source rules the release hosts' compilers need and Windows GCC does not check.

The game repositories build the setup host on Ubuntu 20.04 (GCC 9.4), macOS
(Apple clang) and Windows. Two faults reached a release run that no test on a
Windows GCC host can see:

- macOS: host/psxrecomp_codegen_host.c called kill() without <signal.h>. Apple
  clang rejects an implicit declaration; glibc declares kill() through
  <sys/wait.h>, so Linux built.
- Ubuntu 20.04: libwebp builds its AVX2 files when the compiler accepts -mavx2,
  but GCC before 11 has no _mm256_cvtsi256_si32, and the product failed to link.
"""

from pathlib import Path
import re


def posix_includes(source: str) -> str:
    block = re.search(r"#if defined\(_WIN32\)\n.*?\n#else\n(.*?)\n#endif", source, re.DOTALL)
    if not block:
        raise AssertionError("the platform include block of the setup host was not found")
    return block.group(1)


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    host = (root / "host/psxrecomp_codegen_host.c").read_text(encoding="utf-8").replace("\r\n", "\n")
    includes = posix_includes(host)
    if re.search(r"\bkill\(", host) and "<signal.h>" not in includes:
        raise AssertionError("the setup host calls kill() without <signal.h> in its POSIX includes")

    webp = (root / "runtime/texture_image_dependency.cmake").read_text(encoding="utf-8")
    call = webp.find("psxrecomp_webp_avx2_compat()")
    subdir = webp.find('add_subdirectory("${psx_libwebp_SOURCE_DIR}"')
    if subdir < 0 or call < subdir:
        raise AssertionError("the AVX2 compatibility check does not run after libwebp's targets exist")
    for needle in (
        "PSX_HOST_HAS_MM256_CVTSI256_SI32",
        "PSX_HOST_HAS_AVX2_INTRINSICS AND NOT PSX_HOST_HAS_MM256_CVTSI256_SI32",
        "-D_mm256_cvtsi256_si32(a)=_mm_cvtsi128_si32(_mm256_castsi256_si128(a))",
        "webpdspdecode",
    ):
        if needle not in webp:
            raise AssertionError(f"texture_image_dependency.cmake lost its AVX2 compatibility rule: {needle}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
