/* The instruction cache keeps the words a refill loaded (PSX-CPU-005, PS1B-109).
 * Discworld II replaces the exception vector at 0x80000080 while the cache
 * still holds the kernel's vector; the next SYSCALL runs the cached vector, so
 * the game's handler does not run. This fixture checks the cache model alone. */
#include "cpu_state.h"
#include "psx_icache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_ls_replay_active = 0;
static uint64_t cycles;
void psx_advance_cycles(uint32_t count) { cycles += count; }
/* PSX_OVERLAY_DLL_BUILD makes the step boundary an extern; no observer here. */
void psx_cpu_step_boundary(CPUState *cpu, uint32_t address) { (void)cpu; (void)address; }
void overlay_flush_cycles(void)
{
    fputs("overlay_flush_cycles: not modelled by this test\n", stderr);
    abort();
}
uint64_t psx_get_cycle_count(void)
{
    fputs("psx_get_cycle_count: not modelled by this test\n", stderr);
    abort();
}

#ifdef PSX_ICACHE_SNAPSHOT_WIRE_BYTES
#define TEST_CACHE_BYTES PSX_ICACHE_SNAPSHOT_WIRE_BYTES
static void cache_save(uint8_t *p) { psx_icache_snapshot_write(p); }
static int cache_load(const uint8_t *p) { return psx_icache_snapshot_read(p,TEST_CACHE_BYTES); }
#else
#define TEST_CACHE_BYTES 4096u
static void cache_save(uint8_t *p) { memcpy(p,g_psx_icache_tv,4096u); }
static int cache_load(const uint8_t *p) { memcpy(g_psx_icache_tv,p,4096u); psx_icache_rederive_data(); return 1; }
#endif

static uint8_t ram[2u * 1024u * 1024u];
static void poke(uint32_t physical, uint32_t word)
{
    memcpy(&ram[physical], &word, 4);
}
static uint32_t peek(uint32_t physical)
{
    uint32_t word;
    memcpy(&word, &ram[physical], 4);
    return word;
}
/* What the interpreter executes at address: cache word or memory, then refill. */
static uint32_t execute(CPUState *cpu, uint32_t address)
{
    uint32_t word = psx_icache_instruction(address, peek(address & 0x1FFFFFu));
    psx_icache_fetch_interp(cpu, address);
    return word;
}
static int failures;
static void expect(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main(int argc, char **argv)
{
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    psx_icache_attach_ram(ram, sizeof ram);
    psx_icache_reset();
    if (argc == 2 && !strcmp(argv[1],"off-environment")) {
        if (psx_icache_enabled()) return 2;
        g_psx_icache_active=1;poke(0x7000,0x11223344u);
        (void)execute(&cpu,0x80007000u);poke(0x7000,0x55667788u);
        uint8_t wire[TEST_CACHE_BYTES];cache_save(wire);
        psx_icache_reset();expect(cache_load(wire),"off environment snapshot accepted");
        g_psx_icache_active=-1;uint64_t before_off=cycles;
        expect(psx_icache_instruction(0x80007000u,peek(0x7000))==0x55667788u,"off environment loaded cache reads RAM");
        expect(g_psx_icache_active==0,"off environment initializes disabled selector");
        expect(cycles==before_off,"off environment lookup has no clock change");
        return failures ? 1 : 0;
    }
    g_psx_icache_active = 1;

    /* The kernel's vector runs once and fills the line. */
    poke(0x80, 0x3C1A0000u); poke(0x84, 0x275A0C80u);
    poke(0x88, 0x03400008u); poke(0x8C, 0x00000000u);
    expect(execute(&cpu, 0x80000080u) == 0x3C1A0000u, "cold fetch reads RAM");
    /* The game stores its own vector; a store does not touch the cache. */
    poke(0x80, 0x3C1A8001u); poke(0x84, 0x275A2674u);
    poke(0x88, 0x03400008u); poke(0x8C, 0x00000000u);
    expect(execute(&cpu, 0x80000080u) == 0x3C1A0000u, "held line runs the old word 0");
    expect(execute(&cpu, 0x80000084u) == 0x275A0C80u, "held line runs the old word 1");
    /* KSEG1 is uncached: it always reads RAM and leaves the line alone. */
    expect(execute(&cpu, 0xA0000080u) == 0x3C1A8001u, "KSEG1 fetch reads RAM");
    expect(execute(&cpu, 0x80000080u) == 0x3C1A0000u, "KSEG1 fetch keeps the line");
    /* Code at the same index evicts the line; the next fetch refills from RAM. */
    poke(0x1080, 0x00000000u);
    (void)execute(&cpu, 0x80001080u);
    expect(execute(&cpu, 0x80000080u) == 0x3C1A8001u, "refill after eviction reads RAM");
    expect(execute(&cpu, 0x80000084u) == 0x275A2674u, "refill loads the rest of the line");

    /* A partial refill (entry at word 2) loads words 2 and 3 only. */
    psx_icache_reset();
    poke(0x2000, 0x11111111u); poke(0x2004, 0x22222222u);
    poke(0x2008, 0x33333333u); poke(0x200C, 0x44444444u);
    (void)execute(&cpu, 0x80002008u);
    poke(0x2000, 0x55555555u); poke(0x200C, 0x66666666u);
    expect(execute(&cpu, 0x8000200Cu) == 0x44444444u, "word after the entry is held");
    expect(execute(&cpu, 0x80002000u) == 0x55555555u, "word before a partial refill is not held");
    /* That miss refilled the whole line from RAM. */
    expect(execute(&cpu, 0x8000200Cu) == 0x66666666u, "a miss at word 0 reloads words 0-3");

    /* A loaded state brings tags only: valid lines take RAM's words. */
    psx_icache_reset();
    poke(0x3000, 0xAAAAAAAAu);
    (void)execute(&cpu, 0x80003000u);
    poke(0x3000, 0xBBBBBBBBu);
    psx_icache_rederive_data();
    expect(execute(&cpu, 0x80003000u) == 0xBBBBBBBBu, "rederived line holds RAM");

    /* Execute a held word after RAM was replaced, save/load, execute again. */
    psx_icache_reset();
    g_psx_icache_active=1;
    poke(0x5000,0x11223344u);
    (void)execute(&cpu,0x80005000u);
    poke(0x5000,0x55667788u);
    expect(execute(&cpu,0x80005000u)==0x11223344u,"stale instruction exists before snapshot");
    uint8_t saved_cache[TEST_CACHE_BYTES];
    cache_save(saved_cache);
    psx_icache_reset();
    expect(cache_load(saved_cache),"cache snapshot load");
    expect(execute(&cpu,0x80005000u)==0x11223344u,"snapshot preserves stale instruction instead of current RAM");
    expect(peek(0x5000)==0x55667788u,"snapshot never patches RAM");
#ifdef PSX_ICACHE_SNAPSHOT_WIRE_BYTES
    uint8_t unchanged[TEST_CACHE_BYTES],after_bad[TEST_CACHE_BYTES],bad[TEST_CACHE_BYTES];
    cache_save(unchanged); memcpy(bad,saved_cache,sizeof bad); bad[8192]=2;
    expect(!psx_icache_snapshot_read(bad,sizeof bad),"invalid held flag rejected");
    cache_save(after_bad);expect(!memcmp(unchanged,after_bad,sizeof unchanged),"invalid cache never mutates state");
    expect(!psx_icache_snapshot_read(saved_cache,sizeof saved_cache-1),"truncated cache rejected");
    expect(psx_icache_snapshot_read(saved_cache,PSX_ICACHE_LEGACY_WIRE_BYTES),"legacy tags remain readable");
    expect(execute(&cpu,0x80005000u)==0x55667788u,"legacy tags explicitly reconstruct current RAM");
#endif

    /* A warm line and a loaded full snapshot must still obey cache-off. */
    psx_icache_reset();g_psx_icache_active=1;
    poke(0x6000,0x13572468u);(void)execute(&cpu,0x80006000u);
    poke(0x6000,0x24681357u);
    uint8_t warm_cache[TEST_CACHE_BYTES];cache_save(warm_cache);
    g_psx_icache_active=0;uint64_t before_off=cycles;
    expect(execute(&cpu,0x80006000u)==0x24681357u,"warm cache disabled reads RAM");
    expect(cycles==before_off,"warm cache disabled has no refill cost");
    psx_icache_reset();g_psx_icache_active=0;
    expect(cache_load(warm_cache),"snapshot restores while cache disabled");
    before_off=cycles;
    expect(execute(&cpu,0x80006000u)==0x24681357u,"snapshot cache disabled reads RAM");
    expect(cycles==before_off,"snapshot cache disabled has no refill cost");
    g_psx_icache_active=-1;
    expect(execute(&cpu,0x80006000u)==0x13572468u,"uninitialized selector resolves enabled snapshot");
    expect(g_psx_icache_active==1,"instruction lookup initializes selector consistently");

    /* With the model off no refill happens, so nothing is ever held. */
    psx_icache_reset();
    g_psx_icache_active = 0;
    poke(0x4000, 0x12345678u);
    (void)execute(&cpu, 0x80004000u);
    poke(0x4000, 0x9ABCDEF0u);
    expect(execute(&cpu, 0x80004000u) == 0x9ABCDEF0u, "inactive cache never holds");

    if (failures) return 1;
    puts("psx_icache_contents_test: ok");
    return 0;
}
