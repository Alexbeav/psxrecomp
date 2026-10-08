/* Production full writer/reader, GPU/DMA and cache owner; other devices use
 * the existing admission fixture's explicit snapshot seams. No guest data. */
#define GPU_QUEUE_REAL_ICACHE
#include "test_gpu_queue_boot_admission.c"

void spu_snapshot_write(uint8_t *p) { memset(p, 0, 4); }
void cdrom_snapshot_write(uint8_t *p) { memset(p, 0, 4); }
void sio_snapshot_write(uint8_t *p) { memset(p, 0, 4); }
uint32_t mdec_snapshot_bytes(void) { return 4; }
void mdec_snapshot_write(uint8_t *p) { memset(p, 0, 4); }
uint32_t dirty_ram_checkpoint_pc(uint32_t fallback) { return fallback; }
void dirty_ram_checkpoint_write(uint8_t *p) { memset(p, 0, DIRTY_RAM_CHECKPOINT_BYTES); }
uint32_t dirty_ram_get_bitmap_word(uint32_t i) { assert(i == 0); return 0; }
void psx_scheduler_snapshot_write(uint8_t *p, uint32_t n) { memset(p, 0, n); }
int fntrace_is_game_started(void) { return 0; }
void interrupts_timing_wire_write(uint8_t *p) { memset(p, 0, 64); }
void timers_get_snapshot(uint16_t a[3], uint32_t b[3], uint16_t c[3], int32_t d[3], uint32_t e[3]) {
 memset(a, 0, 6); memset(b, 0, 12); memset(c, 0, 6); memset(d, 0, 12); memset(e, 0, 12);
}
static unsigned failures;
static void check_cache(int ok, const char *why) {
 if (!ok) { fprintf(stderr, "FAIL: %s\n", why); failures++; }
}
static size_t cache_section(const uint8_t *wire, size_t n, uint64_t *bytes) {
 size_t at = BOOT_STATE_HEADER_WIRE_BYTES;
 while (at + 16u <= n) {
  PstR r; uint32_t tag, flags;
  pst_r_init(&r, wire + at, 16);
  assert(pst_r_u32(&r, &tag) && pst_r_u32(&r, &flags) && pst_r_u64(&r, bytes));
  assert(!flags && *bytes <= n - at - 16u);
  if (tag == BS_SEC_ICACHE) return at + 16u;
  at += 16u + (size_t)*bytes;
 }
 abort();
}
int main(void) {
 reset_gpu_state_for_test(); dma_init(); psx_icache_reset();
 psx_icache_bind_memory((const uint8_t *)test_ram, sizeof test_ram, NULL);
 for (unsigned i = 0; i < 8; i++) {
  g_psx_icache_tv[i] = 0x80000000u + i * 4u;
  g_psx_icache_words[i] = 0x2408002au + i;
 }
 g_psx_cache_ctrl = 0x804u;
 psx_icache_isolated_store(0x80000000u, g_psx_cache_ctrl);
 assert(g_psx_icache_tv[0] == 2u && g_psx_icache_tv[4] == 0x80000010u);
 CPUState saved = {0}, live = {0}; saved.pc = 0x80002000u;
 uint8_t *wire = NULL; size_t n = 0;
 assert(boot_state_save_buffer_raw(&saved, 0, 0, &wire, &n));
 g_psx_cache_ctrl = 0; g_psx_icache_tv[4] = 1u; g_psx_icache_words[4] = 0;
 assert(boot_state_load_buffer(wire, n, 0, 0, &live));
 check_cache(g_psx_cache_ctrl == 0x804u, "full writer/reader must restore cache control");
 check_cache(g_psx_icache_tv[4] == 0x80000010u && g_psx_icache_words[4] == 0x2408002eu,
             "full writer/reader must retain stale instruction word");
 psx_icache_isolated_store(0x80000010u, g_psx_cache_ctrl);
 check_cache(g_psx_icache_tv[4] == 2u && g_psx_icache_words[4] == 0,
             "isolated store after restored snapshot must invalidate second line");
 uint64_t cache_bytes; size_t cache_at = cache_section(wire, n, &cache_bytes);
 if (cache_bytes == 8196u) for (unsigned version = 15u; version <= 16u; version++) {
  const size_t keep = version == 15u ? 4096u : 8192u;
  const size_t drop = (size_t)cache_bytes - keep;
  uint8_t *legacy = malloc(n - drop);
  memcpy(legacy, wire, cache_at + keep);
  memcpy(legacy + cache_at + keep, wire + cache_at + cache_bytes, n - cache_at - (size_t)cache_bytes);
  PstW w; pst_w_init(&w, legacy + cache_at - 8u, 8u); assert(pst_w_u64(&w, keep));
  live.pc = 0xdeadbeefu; g_psx_cache_ctrl = 0x1234u; mutations = 0;
  assert(!boot_state_load_buffer(legacy, n - drop, 0, 0, &live));
  assert(live.pc == 0xdeadbeefu && g_psx_cache_ctrl == 0x1234u && !mutations);
  pst_w_init(&w, legacy + 4, 4); assert(pst_w_u32(&w, version));
  assert(boot_state_load_buffer(legacy, n - drop, 0, 0, &live));
  check_cache(g_psx_cache_ctrl == 0u, "legacy v15/v16 must default absent cache control");
  check_cache(g_psx_icache_words[4] == (version == 15u ? test_ram[4] : 0x2408002eu),
              "legacy v15 reconstructs RAM words; v16 retains cached words");
  free(legacy);
 }
 free(wire);
 g_psx_cache_ctrl = 0x804u;
 assert(psx_icache_shadow_record_begin()); g_psx_cache_ctrl = 0x1234u;
 assert(psx_icache_shadow_replay_begin());
 check_cache(g_psx_cache_ctrl == 0x804u, "shadow replay must restore recorded cache control");
 psx_icache_shadow_replay_end();
 check_cache(g_psx_cache_ctrl == 0x1234u, "shadow end must restore suspended cache control");
 psx_icache_reset();
 check_cache(g_psx_cache_ctrl == 0u, "cache reset must clear isolated-store mode");
 if (failures) return 1;
 puts("PASS: actual writer/reader cache-control roundtrip, resumed isolation, shadow and reset");
 return 0;
}
