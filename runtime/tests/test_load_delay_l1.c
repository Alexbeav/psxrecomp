/* Exposure: prior PS1B-102 reference-behavior comments. Inputs: clean L1 and PSX-SPX only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "cpu_state_wire.h"
int l1_step(CPUState *, uint32_t, uint32_t, uint32_t *);
int source_gpu_runtime_active(void) { return 0; }
int psx_ws_backdrop_preload(void) { return 0; }
int psx_ws_is_cull_xclip_load_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_plane_nx_site(uint32_t pc) { (void)pc; return 0; }
void psx_pgxp_load(CPUState *c,uint32_t i,uint32_t a,uint32_t v) {(void)c;(void)i;(void)a;(void)v;}
void psx_pgxp_move(CPUState *c,uint32_t i,uint32_t v) {(void)c;(void)i;(void)v;}
void psx_pgxp_alu(CPUState *c,uint32_t i,uint32_t a,uint32_t b,uint32_t v) {(void)c;(void)i;(void)a;(void)b;(void)v;}
uint32_t psx_read_word(uint32_t a) {
    switch(a) {
    case 0xa0040000u: return 0xbbbb0002u;
    case 0xa0040004u: return 0xcccc0003u;
    case 0xa0040008u: return 0x8081f2f3u;
    default: assert(!"unexpected read"); return 0;
    }
}
uint16_t psx_read_half(uint32_t a) { return (uint16_t)(psx_read_word(a&~3u) >> (8*(a&3))); }
uint8_t psx_cyc_load_byte(CPUState *c,uint32_t a,uint32_t r,uint32_t m) {
    (void)c;(void)r;(void)m; return (uint8_t)(psx_read_word(a&~3u) >> (8*(a&3)));
}
int psx_ws_angle_site(uint32_t p,uint32_t i,uint32_t *v) {(void)p;(void)i;(void)v;return 0;}
int psx_ws_is_cull_bias_site(uint32_t p) {(void)p;return 0;}
int psx_ws_is_signed_x_bound_site(uint32_t p,uint32_t i) {(void)p;(void)i;return 0;}
static unsigned checks;
static void equal(uint32_t got,uint32_t expected) {
    ++checks;
    if(got!=expected) { fprintf(stderr,"check %u: %08x != %08x\n",checks,got,expected); assert(got==expected); }
}
static void step(CPUState *c,uint32_t word) {
    uint32_t next; assert(l1_step(c,c->pc,word,&next)==0);c->pc=next;
}
static CPUState fresh(void) {
    CPUState c={0};c.pc=0x8001053cu;c.gpr[11]=0xaaaa0001u;c.gpr[17]=0xa0040000u;return c;
}
int main(void) {
    CPUState cpu = {0}; uint32_t next;
    cpu.gpr[11]=0xaaaa0001u; cpu.gpr[17]=0xa0040000u;
    /* L1 a, relocated to game RAM to exercise the formerly eager path. */
    assert(l1_step(&cpu,0x8001053cu,0x8e2b0000u,&next)==0);
    assert(l1_step(&cpu,0x80010540u,0x01606021u,&next)==0);
    assert(l1_step(&cpu,0x80010544u,0x01606825u,&next)==0);
    printf("L1 a: slot=%08x after=%08x\n",cpu.gpr[12],cpu.gpr[13]);
    equal(cpu.gpr[12],0xaaaa0001u);equal(cpu.gpr[13],0xbbbb0002u);
    /* L1 b: two loads to one GPR suppress the first value. */
    cpu=fresh();step(&cpu,0x8e2b0000u);step(&cpu,0x8e2b0004u);
    step(&cpu,0x01606021u);step(&cpu,0x01606825u);
    equal(cpu.gpr[12],0xaaaa0001u);equal(cpu.gpr[13],0xcccc0003u);
    /* L1 i: pair forwarding must not expose the first merge. */
    cpu=fresh();cpu.gpr[11]=0x11223344u;
    step(&cpu,0x8a2b0005u);step(&cpu,0x9a2b0002u);
    step(&cpu,0x01606021u);step(&cpu,0x01606825u);
    equal(cpu.gpr[12],0x11223344u);equal(cpu.gpr[13],0x0003bbbbu);
    /* L1 j and k: cancel and extension for each ordinary load. */
    const uint32_t ops[]={0x20,0x21,0x23,0x24,0x25};
    const uint32_t expected[]={0xfffffff3u,0xfffff2f3u,0x8081f2f3u,0xf3u,0xf2f3u};
    for(unsigned i=0;i<5;i++) {
        uint32_t load=(ops[i]<<26)|(17u<<21)|(11u<<16)|8u;
        cpu=fresh();step(&cpu,load);step(&cpu,0x01606021u);step(&cpu,0x01606825u);
        equal(cpu.gpr[12],0xaaaa0001u);equal(cpu.gpr[13],expected[i]);
        cpu=fresh();step(&cpu,load);step(&cpu,0x240b0077u);step(&cpu,0x01606021u);
        equal(cpu.gpr[11],0x77);equal(cpu.gpr[12],0x77);
    }
    /* A fault in the successor commits the pending value at vector entry. */
    cpu=fresh();step(&cpu,0x8e2b0000u);
    assert(l1_step(&cpu,cpu.pc,0x8e2c0001u,&next)==1);
    equal(cpu.gpr[11],0xbbbb0002u);equal(cpu.cop0[14],0x80010540u);
    /* Pending state is per CPU, survives both live ages, and validates first. */
    for(unsigned age=0;age<2;age++) {
        cpu=fresh();step(&cpu,0x8e2b0000u);
        if(age)step(&cpu,0u);
        CPUState other=fresh();step(&other,0x01606021u);equal(other.gpr[12],0xaaaa0001u);
        uint8_t wire[CPU_STATE_WIRE_BYTES];assert(cpu_state_wire_write(wire,&cpu));
        CPUState restored={0};assert(cpu_state_wire_read(wire,sizeof wire,&restored));
        step(&restored,0x01606021u);equal(restored.gpr[12],age?0xbbbb0002u:0xaaaa0001u);
        CPUState unchanged=restored;wire[580]=32;
        assert(!cpu_state_wire_read(wire,sizeof wire,&restored));assert(!memcmp(&unchanged,&restored,sizeof restored));
    }
    printf("PASS: %u decoder and save checks\n",checks);return 0;
}
