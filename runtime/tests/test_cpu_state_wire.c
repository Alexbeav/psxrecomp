#include "cpu_state_wire.h"
#include <assert.h>
#include <string.h>

int main(void) {
    CPUState cpu={0}, restored={0};
    uint8_t wire[CPU_STATE_WIRE_BYTES], copy[CPU_STATE_WIRE_BYTES];
    PstW w; pst_w_init(&w,wire,sizeof wire);
    /* Independent canonical field order; every register and timing byte differs. */
    for(unsigned i=0;i<131;i++)assert(pst_w_u32(&w,0x10203000u+i));
    assert(pst_w_u64(&w,0x0123456789abcdefull));
    assert(pst_w_u64(&w,0xfedcba9876543210ull));
    for(unsigned i=0;i<33;i++)assert(pst_w_u8(&w,(uint8_t)(i+1u)));
    assert(pst_w_u8(&w,31));assert(pst_w_u8(&w,32));assert(pst_w_u8(&w,17));
    assert(pst_w_u32(&w,0xabcdef12u));assert(w.written==sizeof wire);
    assert(cpu_state_wire_read(wire,sizeof wire,&cpu));
    assert(cpu.muldiv_ts_done==0x0123456789abcdefull);
    assert(cpu.gte_ts_done==0xfedcba9876543210ull);
    assert(cpu.read_absorb[32]==33 && cpu.read_absorb_which==31);
    assert(cpu.read_fudge==32 && cpu.ld_which_t==17 && cpu.ld_absorb==0xabcdef12u);
    memset(copy,0xa5,sizeof copy);assert(cpu_state_wire_write(copy,&cpu));
    assert(memcmp(wire,copy,sizeof wire)==0);
    assert(!cpu_state_wire_read(wire,524,&restored));
    for(unsigned i=573;i<=575;i++) {
        memcpy(copy,wire,sizeof copy);copy[i]=33;
        assert(!cpu_state_wire_read(copy,sizeof copy,&restored));
        assert(restored.pc==0); /* reject invalid indexes before mutation */
    }
    /* A changed deadline must affect the image: the old 524-byte codec lost it. */
    cpu.muldiv_ts_done++;assert(cpu_state_wire_write(copy,&cpu));
    assert(memcmp(wire,copy,sizeof wire)!=0);
    return 0;
}
