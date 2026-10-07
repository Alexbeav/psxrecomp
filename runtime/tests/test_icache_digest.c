#include "netplay_state_digest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint64_t psx_cycle_count;
uint32_t i_stat, i_mask, g_psx_icache_tv[1024], g_psx_icache_words[1024];
static uint8_t ram[2u << 20];
uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
uint32_t dirty_ram_get_bitmap_word_count(void) { return 0; }
uint32_t dirty_ram_get_bitmap_word(uint32_t index) { (void)index; assert(0); return 0; }
void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3], uint16_t target[3],
                         int32_t irq[3], uint32_t frac[3])
{
    memset(counter, 0, 6); memset(mode, 0, 12); memset(target, 0, 6);
    memset(irq, 0, 12); memset(frac, 0, 12);
}

int main(void)
{
    CPUState cpu = {0};
    NetplayCoreParts old, current, changed_old, changed_current;
    netplay_core_digest_parts_version(&cpu, &old, 1);
    netplay_core_digest_parts(&cpu, &current);
    g_psx_icache_words[0] = 0x24080001u;
    netplay_core_digest_parts_version(&cpu, &changed_old, 1);
    netplay_core_digest_parts(&cpu, &changed_current);
    assert(old.core == changed_old.core);
    assert(current.core != changed_current.core);
    assert(current.cpu == changed_current.cpu && current.ram == changed_current.ram);
    assert(current.clock_irq != changed_current.clock_irq);
    g_psx_icache_tv[0] = 0x80001000u;
    netplay_core_digest_parts_version(&cpu, &changed_old, 1);
    assert(old.core != changed_old.core);
    puts("PASS: legacy replay excludes cache words; current replay and rollback retain them");
}
