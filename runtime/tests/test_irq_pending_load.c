/* A load completed in the handler must survive a
 * same-thread interrupt return. Production interrupts/traps drive the poll. */
#define fx_k_exception fixture_kernel_entry
#include "rfe_guest_fixture.h"
#undef fx_k_exception
static uint32_t retired_in_handler;
static void kernel_entry(CPUState *c) {
    /* Two authored handler NOP boundaries retire the pre-interrupt load. */
    psx_load_value_begin(c);
    psx_load_value_begin(c);
    retired_in_handler=c->gpr[22];
    fixture_kernel_entry(c);
}
static void handler(CPUState *c) {i_stat &= ~1u;c->pc=FX_K_RFE;}
int main(void) {
    const uint32_t old=0x80190002u,loaded=0x801A0000u,resume=0x80010020u;
    const FxEntry entries[]={{FX_VECTOR,kernel_entry},{FX_K_RET,fx_k_ret},
        {FX_K_RFE,fx_k_return_from_exception},{0x80020000u,handler}};
    CPUState c;
    fx_boot(&c,entries,sizeof(entries)/sizeof(entries[0]),0x80020000u,resume,0x801FF000u,0x80040000u);
    c.cop0[12]=0x401u;c.gpr[22]=old;c.pc=0;
    psx_load_value_arm(&c,22,loaded);
    i_mask=i_stat=1;psx_cycle_count=10000;
    psx_check_interrupts_at(&c,resume);
    printf("entries=%llu retired=%08X returned=%08X pending_rt=%u epc=%08X\n",
        (unsigned long long)fx_exception_entries,retired_in_handler,c.gpr[22],c.load_value_rt,c.cop0[14]);
    FX_CHECK(fx_exception_entries==1 && retired_in_handler==loaded);
    FX_CHECK(c.gpr[22]==loaded && c.load_value_rt==0);
    return 0;
}
