/* Exposure: prior PS1B-102 reference-behavior comments. Inputs: clean L1 and PSX-SPX only. */
#include <assert.h>
#include <stdio.h>
#include "cpu_state.h"
int l1_step(CPUState *, uint32_t, uint32_t, uint32_t *);
int source_gpu_runtime_active(void) { return 0; }
int psx_ws_backdrop_preload(void) { return 0; }
int psx_ws_is_cull_xclip_load_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_plane_nx_site(uint32_t pc) { (void)pc; return 0; }
void psx_pgxp_load(CPUState *c,uint32_t i,uint32_t a,uint32_t v) {(void)c;(void)i;(void)a;(void)v;}
void psx_pgxp_move(CPUState *c,uint32_t i,uint32_t v) {(void)c;(void)i;(void)v;}
void psx_pgxp_alu(CPUState *c,uint32_t i,uint32_t a,uint32_t b,uint32_t v) {(void)c;(void)i;(void)a;(void)b;(void)v;}
uint32_t psx_read_word(uint32_t a) { assert(a == 0xa0040000u); return 0xbbbb0002u; }
int main(void) {
    CPUState cpu = {0}; uint32_t next;
    cpu.gpr[11]=0xaaaa0001u; cpu.gpr[17]=0xa0040000u;
    /* L1 a, relocated to game RAM to exercise the formerly eager path. */
    assert(l1_step(&cpu,0x8001053cu,0x8e2b0000u,&next)==0);
    assert(l1_step(&cpu,0x80010540u,0x01606021u,&next)==0);
    assert(l1_step(&cpu,0x80010544u,0x01606825u,&next)==0);
    printf("L1 a: slot=%08x after=%08x\n",cpu.gpr[12],cpu.gpr[13]);
    return cpu.gpr[12]!=0xaaaa0001u || cpu.gpr[13]!=0xbbbb0002u;
}
