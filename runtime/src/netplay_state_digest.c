#include "netplay_state_digest.h"
#include "crc32.h"
#include "dirty_ram_interp.h"
#include "psx_cycles.h"

#include <string.h>

extern uint8_t* memory_get_ram_ptr(void);
extern uint32_t i_stat;
extern uint32_t i_mask;
extern void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                                uint16_t target[3], int32_t irq_line[3],
                                uint32_t frac[3]);

#define NP_RAM_SIZE (2u * 1024u * 1024u)

uint32_t netplay_master_digest(const CPUState* cpu) {
    uint32_t crc = 0xFFFFFFFFu;
    uint16_t counter[3], target[3];
    uint32_t mode[3], frac[3];
    int32_t irq_line[3];
    uint32_t wc;
    uint32_t i;
    uint8_t* ram;

    if (!cpu) return 0;

    crc = crc32_update(crc, (const uint8_t*)cpu->gpr, sizeof(cpu->gpr));
    crc = crc32_update(crc, (const uint8_t*)&cpu->pc, sizeof(cpu->pc));
    crc = crc32_update(crc, (const uint8_t*)&cpu->hi, sizeof(cpu->hi));
    crc = crc32_update(crc, (const uint8_t*)&cpu->lo, sizeof(cpu->lo));
    /* Status / Cause / EPC — exception path identity. */
    crc = crc32_update(crc, (const uint8_t*)&cpu->cop0[12], sizeof(uint32_t));
    crc = crc32_update(crc, (const uint8_t*)&cpu->cop0[13], sizeof(uint32_t));
    crc = crc32_update(crc, (const uint8_t*)&cpu->cop0[14], sizeof(uint32_t));

    {
        uint64_t cyc = psx_cycle_count;
        crc = crc32_update(crc, (const uint8_t*)&cyc, sizeof(cyc));
    }
    crc = crc32_update(crc, (const uint8_t*)&i_stat, sizeof(i_stat));
    crc = crc32_update(crc, (const uint8_t*)&i_mask, sizeof(i_mask));

    timers_get_snapshot(counter, mode, target, irq_line, frac);
    crc = crc32_update(crc, (const uint8_t*)counter, sizeof(counter));
    crc = crc32_update(crc, (const uint8_t*)mode, sizeof(mode));
    crc = crc32_update(crc, (const uint8_t*)target, sizeof(target));
    crc = crc32_update(crc, (const uint8_t*)irq_line, sizeof(irq_line));
    crc = crc32_update(crc, (const uint8_t*)frac, sizeof(frac));

    ram = memory_get_ram_ptr();
    if (ram)
        crc = crc32_update(crc, ram, NP_RAM_SIZE);

    wc = dirty_ram_get_bitmap_word_count();
    for (i = 0; i < wc; i++) {
        uint32_t w = dirty_ram_get_bitmap_word(i);
        crc = crc32_update(crc, (const uint8_t*)&w, sizeof(w));
    }

    return crc ^ 0xFFFFFFFFu;
}
