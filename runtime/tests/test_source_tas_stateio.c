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
    static const char *const CFG = "00000000000000ab00000000000000ab00000000000000ab00000000000000ab";
    static const char *const EXE = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char *const ROUTE = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

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
    snprintf(m.config_digest, sizeof m.config_digest, "%s", CFG);
    snprintf(m.exe_sha256, sizeof m.exe_sha256, "%s", EXE);
    snprintf(m.route_sha256, sizeof m.route_sha256, "%s", ROUTE);

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
    check(strcmp(parsed.config_digest, CFG) == 0 && strcmp(parsed.exe_sha256, EXE) == 0 &&
          strcmp(parsed.route_sha256, ROUTE) == 0, "round-trip v7 identity");
    check(source_tas_stateio_parse_u64(text, "frame", &v) == 1 && v == 300u, "parse_u64 frame");

    /* --- environment identity: whole PSX_* set, order independent --- */
    {
        static const char *const env_a[] = { "PSX_TIMER1_MODEL=octoshock-2.2.2",
                                             "PSX_ENABLE_BLOCK_CYCLES=1", NULL };
        static const char *const env_b[] = { "PSX_ENABLE_BLOCK_CYCLES=1",
                                             "PSX_TIMER1_MODEL=octoshock-2.2.2", NULL };
        static const char *const env_c[] = { "PSX_TIMER1_MODEL=octoshock-2.2.2",
                                             "PSX_ENABLE_BLOCK_CYCLES=0", NULL };
        static const char *const env_new[] = { "PSX_TIMER1_MODEL=octoshock-2.2.2",
                                               "PSX_ENABLE_BLOCK_CYCLES=1",
                                               "PSX_FUTURE_MODEL_ENABLED=1", NULL };
        static const char *const env_ignored[] = { "PSX_TIMER1_MODEL=octoshock-2.2.2",
                                                   "PSX_ENABLE_BLOCK_CYCLES=1",
                                                   "PSX_TAS_RESUME_STATE=whatever.pst", NULL };
        check(source_tas_stateio_env_digest(env_a) == source_tas_stateio_env_digest(env_b),
              "env digest order independent");
        check(source_tas_stateio_env_digest(env_a) != source_tas_stateio_env_digest(env_c),
              "env digest value sensitive");
        check(source_tas_stateio_env_digest(env_a) != source_tas_stateio_env_digest(env_new),
              "env digest covers a newly added flag");
        check(source_tas_stateio_env_digest(env_a) == source_tas_stateio_env_digest(env_ignored),
              "env digest skips allow-listed request flags");
    }

    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest,
          m.bios_checksum, m.entry_pc, CFG, EXE, ROUTE, reason, sizeof reason) == 1,
          "accept exact");
    check(reason[0] == '\0', "accept reason cleared");

    check(source_tas_stateio_manifest_accept(&parsed, 301u, m.cycle, digest,
          m.bios_checksum, m.entry_pc, CFG, EXE, ROUTE, reason, sizeof reason) == 0 &&
          strstr(reason, "frame"), "reject frame");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle + 2u, digest,
          m.bios_checksum, m.entry_pc, CFG, EXE, ROUTE, reason, sizeof reason) == 0 &&
          strstr(reason, "cycle"), "reject cycle");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest ^ 1u,
          m.bios_checksum, m.entry_pc, CFG, EXE, ROUTE, reason, sizeof reason) == 0 &&
          strstr(reason, "RAM"), "reject digest");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest,
          0xDEADBEEFu, m.entry_pc, CFG, EXE, ROUTE, reason, sizeof reason) == 0 &&
          strstr(reason, "BIOS"), "reject foreign bios");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest,
          m.bios_checksum, 0x80010000u, CFG, EXE, ROUTE, reason, sizeof reason) == 0 &&
          strstr(reason, "entry"), "reject foreign entry");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest,
          m.bios_checksum, m.entry_pc, "0000000000000000000000000000000000000000000000000000000000000000", EXE, ROUTE,
          reason, sizeof reason) == 0 && strstr(reason, "configuration"), "reject config digest");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest,
          m.bios_checksum, m.entry_pc, CFG, SHA, ROUTE,
          reason, sizeof reason) == 0 && strstr(reason, "binary"), "reject foreign binary");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest,
          m.bios_checksum, m.entry_pc, CFG, EXE, SHA,
          reason, sizeof reason) == 0 && strstr(reason, "route"), "reject foreign route");
    check(source_tas_stateio_manifest_accept(&parsed, 300u, m.cycle, digest,
          m.bios_checksum, m.entry_pc, NULL, EXE, ROUTE, reason, sizeof reason) == 0,
          "reject absent config context");
    check(source_tas_stateio_manifest_accept(NULL, 300u, m.cycle, digest,
          m.bios_checksum, m.entry_pc, CFG, EXE, ROUTE, reason, sizeof reason) == 0 &&
          strstr(reason, "missing"), "reject missing manifest");

    check(source_tas_stateio_manifest_parse("{\n  \"schema\": \"psx-tas-stateio-v1\"\n}\n",
          &parsed, NULL, 0) == 0, "reject truncated manifest");
    check(source_tas_stateio_manifest_parse("", &parsed, NULL, 0) == 0, "reject empty manifest");
    check(source_tas_stateio_manifest_parse(
          "{\"schema\":\"other-v9\",\"frame\":1,\"cycle\":1,\"ram_digest\":\"0\","
          "\"bios_checksum\":1,\"entry_pc\":1,\"state_bytes\":1}", &parsed, NULL, 0) == 0,
          "reject foreign schema");
    check(source_tas_stateio_save_at_match(300u) == 0, "save_at disabled by default");
    /* Comma-separated list: the E test ladder checkpoints at K and K+1 in one run. */
    putenv("PSX_TAS_SAVE_STATE_AT=300,301");
    check(source_tas_stateio_save_at_match(300u) == 1, "save_at list first member");
    check(source_tas_stateio_save_at_match(301u) == 1, "save_at list second member");
    check(source_tas_stateio_save_at_match(302u) == 0, "save_at list non-member");
    check(source_tas_stateio_save_at_match(0u) == 0, "save_at refuses frame 0");
    putenv("PSX_TAS_SAVE_STATE_AT=300");
    check(source_tas_stateio_save_at_match(300u) == 1, "save_at single value");
    check(source_tas_stateio_save_at_match(301u) == 0, "save_at single value non-member");
    putenv("PSX_TAS_SAVE_STATE_AT=");

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: TAS checkpoint manifest round-trip and identity gate");
    return 0;
}
