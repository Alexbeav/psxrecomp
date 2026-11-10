/* The verifier extracts the actual production entry-selection block. This
 * checks architectural dispatch, not handler/RFE execution or cycle accuracy. */
#include "cpu_state.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifndef PSX_TEST_IRQ_VECTOR_INCLUDE
#error "Run through verify_irq_vector.py to bind the production source block"
#endif
static unsigned reads;
static uint32_t vector_words[2];
static uint32_t read_vector(uint32_t addr) {
    reads++;
    return vector_words[(addr>>2)&1u];
}
static uint32_t production_entry(CPUState *cpu, uint32_t sr) {
#include PSX_TEST_IRQ_VECTOR_INCLUDE
    return target_pc;
}
int main(void) {
    unsigned failures=0,cases=0;
    for(unsigned kind=0;kind<3;kind++) for(unsigned bev=0;bev<2;bev++) {
        CPUState cpu={0},before;
        cpu.read_word=read_vector;
        for(unsigned r=0;r<32;r++)cpu.gpr[r]=0x12340000u+r;
        vector_words[0]=kind==2?0x08012345u:0x3c1a8001u;
        vector_words[1]=kind==0?0x275a8000u:kind==1?0x375a8000u:0x00000000u;
        before=cpu;reads=0;
        uint32_t target=production_entry(&cpu,bev?0x00400401u:0x401u);
        uint32_t expected=bev?0xbfc00180u:0x80000080u;
        cases++;
        if(target!=expected || reads || memcmp(&cpu,&before,sizeof(cpu))) {
            fprintf(stderr,"vector kind%u BEV%u: dispatch=%08X expected=%08X data_reads=%u k0=%08X\n",kind,bev,target,expected,reads,cpu.gpr[26]);failures++;
        }
    }
    if(failures)return 1;
    printf("PASS %u architectural IRQ entry cases; vector instruction execution stays with guest dispatcher\n",cases);
    return 0;
}
