/* Exercise the production TAS checkpoint manifest writer and identity gate
 * with synthetic data. Compiled at O0/O2 by test_source_tas_stateio.py. */
#include "source_tas_stateio.h"
#include <stdlib.h>

static int failures = 0;
static void check(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main(int argc, char **argv) {
    const char *dir;
    char state_path[512], manifest_path[512], text[4096], parsed_state[512], reason[128];
    TasStateManifest m, parsed;
    uint8_t ram[8192];
    size_t i;
    uint64_t digest, v = 0;
    static const char *const SHA = "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF";

    if (argc != 2) return 2;
    dir = argv[1];
    snprintf(state_path, sizeof state_path, "%s/tas-state-000300.pst", dir);
    snprintf(manifest_path, sizeof manifest_path, "%s/tas-state-000300.pst.json", dir);

    for (i = 0; i < sizeof ram; ++i) ram[i] = (uint8_t)(i * 31u + 7u);
    digest = source_tas_stateio_ram_digest(ram, sizeof ram);
    check(digest != 0, "digest nonzero");
    check(digest == source_tas_stateio_ram_digest(ram, sizeof ram), "digest stable");
    check(digest != source_tas_stateio_ram_digest(ram, sizeof ram - 1), "digest length sensitive");

    m.frame = 300u; m.cycle = UINT64_C(262837430); m.ram_digest = digest;
    m.bios_checksum = 0x11223344u; m.entry_pc = 0x800603F8u; m.state_bytes = 3551234ull;

    check(source_tas_stateio_manifest_write(manifest_path, &m, state_path, SHA) == 1,
          "write manifest");
    check(source_tas_stateio_read_text(manifest_path, text, sizeof text) > 0, "read manifest");
    check(source_tas_stateio_manifest_parse(text, &parsed, parsed_state, sizeof parsed_state) == 1,
          "parse manifest");
    check(parsed.frame == m.frame && parsed.cycle == m.cycle && parsed.ram_digest == m.ram_digest,
          "round-trip scalars");
    check(parsed.bios_checksum == m.bios_checksum && parsed.entry_pc == m.entry_pc &&
          parsed.state_bytes == m.state_bytes, "round-trip identity");
    check(strcmp(parsed_state, state_path) == 0, "round-trip state path");
    check(source_tas_stateio_parse_u64(text, "frame", &v) == 1 && v == 300u, "parse_u64 frame");

    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest,
          m.bios_checksum, m.entry_pc, reason, sizeof reason) == 1, "accept exact");
    check(reason[0] == '\0', "accept reason cleared");

    check(source_tas_stateio_manifest_accept(&parsed, 301u, m.cycle, digest,
          m.bios_checksum, m.entry_pc, reason, sizeof reason) == 0 && strstr(reason, "frame"),
          "reject frame");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle + 2u, digest,
          m.bios_checksum, m.entry_pc, reason, sizeof reason) == 0 && strstr(reason, "cycle"),
          "reject cycle");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest ^ 1u,
          m.bios_checksum, m.entry_pc, reason, sizeof reason) == 0 && strstr(reason, "RAM"),
          "reject digest");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest,
          0xDEADBEEFu, m.entry_pc, reason, sizeof reason) == 0 && strstr(reason, "BIOS"),
          "reject foreign bios");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest,
          m.bios_checksum, 0x80010000u, reason, sizeof reason) == 0 && strstr(reason, "entry"),
          "reject foreign entry");
    check(source_tas_stateio_manifest_accept(NULL, 300u, m.cycle, digest,
          m.bios_checksum, m.entry_pc, reason, sizeof reason) == 0 && strstr(reason, "missing"),
          "reject missing manifest");

    check(source_tas_stateio_manifest_parse("{\n  \"schema\": \"psx-tas-stateio-v1\"\n}\n",
          &parsed, NULL, 0) == 0, "reject truncated manifest");
    check(source_tas_stateio_manifest_parse("", &parsed, NULL, 0) == 0, "reject empty manifest");
    check(source_tas_stateio_manifest_parse(
          "{\"schema\":\"other-v9\",\"frame\":1,\"cycle\":1,\"ram_digest\":\"0\","
          "\"bios_checksum\":1,\"entry_pc\":1,\"state_bytes\":1}", &parsed, NULL, 0) == 0,
          "reject foreign schema");
    check(source_tas_stateio_save_at() == 0u, "save_at disabled by default");

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: TAS checkpoint manifest round-trip and identity gate");
    return 0;
}
