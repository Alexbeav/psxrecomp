/* Untimed decoder/dispatch fixtures use direct RAM instruction words.
 * Keep cache interfaces typed and inactive; cache behavior has its own real
 * runtime tests. Unexpected unrelated link seams still abort. */
#ifndef PSX_TEST_INACTIVE_ICACHE_SEAMS_H
#define PSX_TEST_INACTIVE_ICACHE_SEAMS_H
#include <stdint.h>
int g_psx_icache_active = 0;
uint32_t g_psx_icache_tv[1024], g_psx_icache_data[1024];
uint8_t g_psx_icache_held[1024];
int psx_icache_enabled(void) { return 0; }
void psx_icache_attach_ram(const uint8_t *ram, uint32_t bytes) {
    (void)ram; (void)bytes;
}
#endif
