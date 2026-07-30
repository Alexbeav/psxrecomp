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
extern uint32_t spu_snapshot_bytes(void);
extern void     spu_snapshot_write(uint8_t *p);
extern uint8_t* spu_get_ram_ptr(void);
extern uint32_t spu_get_ram_bytes(void);
extern uint32_t mdec_snapshot_bytes(void);
extern void     mdec_snapshot_write(uint8_t *p);

static uint32_t digest_module(uint32_t (*bytes_fn)(void), void (*write_fn)(uint8_t *))
{
    static uint8_t *buf;
    static uint32_t cap;
    uint32_t n;
    if (!bytes_fn || !write_fn)
        return 0u;
    n = bytes_fn();
    if (n == 0u)
        return 0u;
    if (n > cap) {
        uint8_t *nbuf = (uint8_t *)realloc(buf, n);
        if (!nbuf)
            return 0u;
        buf = nbuf;
        cap = n;
    }
    write_fn(buf);
    return crc32_compute(buf, n);
}

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

void netplay_core_digest_parts(const CPUState* cpu, NetplayCoreParts* out)
{
    uint32_t crc_cpu = 0xFFFFFFFFu;
    uint32_t crc_clk = 0xFFFFFFFFu;
    uint32_t crc_tim = 0xFFFFFFFFu;
    uint32_t crc_ram = 0xFFFFFFFFu;
    uint32_t crc_drt = 0xFFFFFFFFu;
    uint32_t crc = 0xFFFFFFFFu;
    uint16_t counter[3], target[3];
    uint32_t mode[3], frac[3];
    int32_t irq_line[3];
    uint32_t wc;
    uint32_t i;
    uint8_t* ram;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!cpu)
        return;

    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)cpu->gpr, sizeof(cpu->gpr));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->pc, sizeof(cpu->pc));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->hi, sizeof(cpu->hi));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->lo, sizeof(cpu->lo));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->cop0[12], sizeof(uint32_t));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->cop0[13], sizeof(uint32_t));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->cop0[14], sizeof(uint32_t));
    crc = crc32_update(crc, (const uint8_t*)cpu->gpr, sizeof(cpu->gpr));
    crc = crc32_update(crc, (const uint8_t*)&cpu->pc, sizeof(cpu->pc));
    crc = crc32_update(crc, (const uint8_t*)&cpu->hi, sizeof(cpu->hi));
    crc = crc32_update(crc, (const uint8_t*)&cpu->lo, sizeof(cpu->lo));
    crc = crc32_update(crc, (const uint8_t*)&cpu->cop0[12], sizeof(uint32_t));
    crc = crc32_update(crc, (const uint8_t*)&cpu->cop0[13], sizeof(uint32_t));
    crc = crc32_update(crc, (const uint8_t*)&cpu->cop0[14], sizeof(uint32_t));

    {
        uint64_t cyc = psx_cycle_count;
        crc_clk = crc32_update(crc_clk, (const uint8_t*)&cyc, sizeof(cyc));
        crc_clk = crc32_update(crc_clk, (const uint8_t*)&i_stat, sizeof(i_stat));
        crc_clk = crc32_update(crc_clk, (const uint8_t*)&i_mask, sizeof(i_mask));
        crc = crc32_update(crc, (const uint8_t*)&cyc, sizeof(cyc));
        crc = crc32_update(crc, (const uint8_t*)&i_stat, sizeof(i_stat));
        crc = crc32_update(crc, (const uint8_t*)&i_mask, sizeof(i_mask));
    }

    timers_get_snapshot(counter, mode, target, irq_line, frac);
    crc_tim = crc32_update(crc_tim, (const uint8_t*)counter, sizeof(counter));
    crc_tim = crc32_update(crc_tim, (const uint8_t*)mode, sizeof(mode));
    crc_tim = crc32_update(crc_tim, (const uint8_t*)target, sizeof(target));
    crc_tim = crc32_update(crc_tim, (const uint8_t*)irq_line, sizeof(irq_line));
    crc_tim = crc32_update(crc_tim, (const uint8_t*)frac, sizeof(frac));
    crc = crc32_update(crc, (const uint8_t*)counter, sizeof(counter));
    crc = crc32_update(crc, (const uint8_t*)mode, sizeof(mode));
    crc = crc32_update(crc, (const uint8_t*)target, sizeof(target));
    crc = crc32_update(crc, (const uint8_t*)irq_line, sizeof(irq_line));
    crc = crc32_update(crc, (const uint8_t*)frac, sizeof(frac));

    ram = memory_get_ram_ptr();
    if (ram) {
        crc_ram = crc32_update(crc_ram, ram, NP_RAM_SIZE);
        crc = crc32_update(crc, ram, NP_RAM_SIZE);
    }

    wc = dirty_ram_get_bitmap_word_count();
    for (i = 0; i < wc; i++) {
        uint32_t w = dirty_ram_get_bitmap_word(i);
        crc_drt = crc32_update(crc_drt, (const uint8_t*)&w, sizeof(w));
        crc = crc32_update(crc, (const uint8_t*)&w, sizeof(w));
    }

    if (out) {
        out->cpu = crc_cpu ^ 0xFFFFFFFFu;
        out->clock_irq = crc_clk ^ 0xFFFFFFFFu;
        out->timers = crc_tim ^ 0xFFFFFFFFu;
        out->ram = crc_ram ^ 0xFFFFFFFFu;
        out->dirty = crc_drt ^ 0xFFFFFFFFu;
        out->core = crc ^ 0xFFFFFFFFu;
    }
}

uint32_t netplay_core_digest(const CPUState* cpu)
{
    NetplayCoreParts p;
    netplay_core_digest_parts(cpu, &p);
    return p.core;
}

uint32_t netplay_spu_digest(void)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t regs = digest_module(spu_snapshot_bytes, spu_snapshot_write);
    const uint8_t *ram = spu_get_ram_ptr();
    uint32_t ram_n = spu_get_ram_bytes();
    crc = crc32_update(crc, (const uint8_t *)&regs, sizeof(regs));
    if (ram && ram_n)
        crc = crc32_update(crc, ram, ram_n);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t netplay_mdec_digest(void)
{
    return digest_module(mdec_snapshot_bytes, mdec_snapshot_write);
}

uint32_t netplay_aux_digest(void)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t spu = netplay_spu_digest();
    uint32_t mdec = netplay_mdec_digest();
    crc = crc32_update(crc, (const uint8_t *)&spu, sizeof(spu));
    crc = crc32_update(crc, (const uint8_t *)&mdec, sizeof(mdec));
    return crc ^ 0xFFFFFFFFu;
}

uint32_t netplay_baseline_ext_digest(void)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t aux = netplay_aux_digest();
    uint32_t cd = netplay_cdrom_digest();
    crc = crc32_update(crc, (const uint8_t *)&aux, sizeof(aux));
    crc = crc32_update(crc, (const uint8_t *)&cd, sizeof(cd));
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
