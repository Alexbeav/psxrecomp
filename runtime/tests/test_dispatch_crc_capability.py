"""Link real compatibility CRC against modern, legacy and BIOS mixed targets."""
import argparse
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--make-program", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="dispatch-crc-") as temporary:
        work = Path(temporary)
        declaration = "const uint32_t *psx_game_source_page_crc32(uint32_t *, uint32_t *, uint32_t *);\n"
        (work / "modern.c").write_text("#include <stdint.h>\n" + declaration + """
const uint32_t *psx_game_source_page_crc32(uint32_t *n, uint32_t *lo, uint32_t *len)
{
    static const uint32_t crc = 0x12345678u;
    if (n) *n = 1; if (lo) *lo = 4096; if (len) *len = 4096;
    return &crc;
}
""", encoding="utf-8")
        (work / "legacy.c").write_text("#include <stdint.h>\n" + declaration, encoding="utf-8")
        (work / "probe.c").write_text("#include <stdint.h>\n" + declaration + """
int dirty_ram_text_native_ok(uint32_t addr) { (void)addr; return 0; }
#ifdef PSX_HAS_GAME_DISPATCH
int psx_game_address_in_text(uint32_t addr) { (void)addr; return 0; }
#endif
int main(void) {
    uint32_t n = 99, lo = 99, len = 99;
    const uint32_t *crc = psx_game_source_page_crc32(&n, &lo, &len);
    if (EXPECT_MODERN) return !(crc && *crc == 0x12345678u && n == 1 && lo == 4096 && len == 4096);
    return !(crc == 0 && n == 0 && lo == 0 && len == 0 &&
             psx_game_source_page_crc32(0, 0, 0) == 0);
}
""", encoding="utf-8")
        cmake = f'''cmake_minimum_required(VERSION 3.20)
project(dispatch_crc C)
enable_testing()
include("{root.as_posix()}/cmake/psx_dispatch_crc.cmake")
set(compat "{root.as_posix()}/runtime/src/game_dispatch_compat.c")
foreach(kind modern legacy bios modern_again legacy_again bios_again)
    add_executable(${{kind}} probe.c "${{compat}}")
    target_include_directories(${{kind}} PRIVATE "{root.as_posix()}/runtime/include")
    if(kind MATCHES "^modern")
        target_sources(${{kind}} PRIVATE modern.c)
        target_compile_definitions(${{kind}} PRIVATE PSX_HAS_GAME_DISPATCH=1 EXPECT_MODERN=1)
        psxrecomp_dispatch_crc(${{kind}} "${{CMAKE_CURRENT_SOURCE_DIR}}/modern.c" "${{compat}}")
    elseif(kind MATCHES "^legacy")
        target_sources(${{kind}} PRIVATE legacy.c)
        target_compile_definitions(${{kind}} PRIVATE PSX_HAS_GAME_DISPATCH=1 EXPECT_MODERN=0)
        psxrecomp_dispatch_crc(${{kind}} "${{CMAKE_CURRENT_SOURCE_DIR}}/legacy.c" "${{compat}}")
    else()
        target_compile_definitions(${{kind}} PRIVATE EXPECT_MODERN=0)
        psxrecomp_dispatch_crc(${{kind}} "" "${{compat}}")
    endif()
    add_test(NAME ${{kind}} COMMAND ${{kind}})
endforeach()
'''
        (work / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
        subprocess.run([args.cmake, "-S", str(work), "-B", str(work / "build"),
                        "-G", args.generator, "-DCMAKE_C_COMPILER=" + args.cc,
                        "-DCMAKE_MAKE_PROGRAM=" + args.make_program], check=True)
        subprocess.run([args.cmake, "--build", str(work / "build"), "--parallel", "1"], check=True)
        ctest = Path(args.cmake).with_name("ctest" + Path(args.cmake).suffix)
        subprocess.run([str(ctest), "--test-dir", str(work / "build"),
                        "--output-on-failure", "--parallel", "1"], check=True)
    print("PASS: modern definitions, prototype-only legacy and BIOS CRC fallbacks coexist per target")


if __name__ == "__main__":
    main()
