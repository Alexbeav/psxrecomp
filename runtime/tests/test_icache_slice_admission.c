#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "psx_icache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t ram[0x200000u / 4u];
static int in_exception;
extern int g_precise_mode;
int psx_get_in_exception(void) { return in_exception; }
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)ram; }

int main(void)
{
    CPUState cpu = {0};
    const uint32_t pc = 0x80001000u, index = (pc >> 2) & 1023u;
    psx_icache_bind_memory((const uint8_t *)ram, sizeof ram, NULL);
    psx_icache_reset();
    g_psx_icache_active = 1;
    g_psx_icache_tv[index] = pc;
    g_psx_icache_words[index] = 0x24080001u;
    g_precise_mode = 1;
    assert(!psx_slice_block_impl(&cpu, pc, 1, 0));
    g_precise_mode = 0;
    in_exception = 1;
    assert(!psx_slice_block_impl(&cpu, pc, 1, 0));
    assert(g_icache_execution_stats.nested_stale_blocks == 2);
    assert(g_icache_execution_stats.exception_stale_blocks == 1);
    assert(!g_icache_execution_stats.stale_blocks);
    assert(!g_icache_execution_stats.stale_blocks_in_exception);
    in_exception = 0;
    g_psx_icache_words[index] = ram[0x1000u / 4u];
    g_psx_icache_tv[index + 1] = pc + 4u;
    g_psx_icache_words[index + 1] = 0x24090001u;
    g_psx_precise_slice = 0;
    assert(!psx_slice_bios_block(&cpu, pc, 1, 0));
    assert(!g_icache_execution_stats.stale_blocks);
    puts("PASS: nested stale blocks stay compiled; BIOS guard checks its word count");
}
