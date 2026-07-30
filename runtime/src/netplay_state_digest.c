#include "netplay_state_digest.h"
#include "cdrom.h"
#include "crc32.h"
#include "dirty_ram_interp.h"
#include "gpu.h"
#include "psx_cycles.h"

#include <stdlib.h>
#include <string.h>

extern uint8_t* memory_get_ram_ptr(void);
extern uint32_t i_stat;
extern uint32_t i_mask;
extern void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                                uint16_t target[3], int32_t irq_line[3],
                                uint32_t frac[3]);
extern uint32_t gpu_snapshot_bytes(void);
extern void     gpu_snapshot_write(uint8_t *p);

#define NP_RAM_SIZE (2u * 1024u * 1024u)

uint32_t netplay_cdrom_digest(void)
{
    static uint8_t *buf;
    static uint32_t cap;
    uint32_t n = cdrom_snapshot_bytes();
    if (n == 0u)
        return 0u;
    if (n > cap) {
        uint8_t *nbuf = (uint8_t *)realloc(buf, n);
        if (!nbuf)
            return 0u;
        buf = nbuf;
        cap = n;
    }
    cdrom_snapshot_write(buf);
    return crc32_compute(buf, n);
}

uint32_t netplay_av_digest(void)
{
    /* GPU regs + full VRAM — GL/VK readback was forking this while core RAM
     * still matched (pin zlib sizes ~1.33M vs ~1.12M). */
    static uint8_t *gbuf;
    static uint32_t gcap;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t gn = gpu_snapshot_bytes();
    const uint16_t *vram = gpu_get_vram();

    if (gn > 0u) {
        if (gn > gcap) {
            uint8_t *nbuf = (uint8_t *)realloc(gbuf, gn);
            if (!nbuf)
                return 0u;
            gbuf = nbuf;
            gcap = gn;
        }
        gpu_snapshot_write(gbuf);
        crc = crc32_update(crc, gbuf, gn);
    }
    if (vram)
        crc = crc32_update(crc, (const uint8_t *)vram, 1024u * 512u * 2u);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t netplay_core_digest(const CPUState* cpu)
{
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

uint32_t netplay_master_digest(const CPUState* cpu)
{
    uint32_t crc;
    uint32_t core;
    uint32_t cd;

    if (!cpu) return 0;
    /* Fold core + CD as raw words so audit can compare partitions. */
    core = netplay_core_digest(cpu);
    cd = netplay_cdrom_digest();
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t*)&core, sizeof(core));
    crc = crc32_update(crc, (const uint8_t*)&cd, sizeof(cd));
    return crc ^ 0xFFFFFFFFu;
}
