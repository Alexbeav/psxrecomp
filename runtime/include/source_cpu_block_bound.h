#ifndef PSX_SOURCE_CPU_BLOCK_BOUND_H
#define PSX_SOURCE_CPU_BLOCK_BOUND_H
/* A generated block's bcyc is its instruction count. It excludes cache
 * refills, data waits and coprocessor interlocks, so it cannot alone prove
 * that a pending IRQ lies beyond the block. This source-profile bound only
 * selects the existing instruction interpreter; it never charges cycles.
 * The caller restricts this to ordinary compiled RAM blocks without MMIO
 * visibility side effects. Their bytes have already passed native-text checks. */
static uint32_t source_cpu_block_bound(CPUState *cpu,uint32_t pc,uint32_t count,uint32_t deadline) {
    uint64_t now=psx_get_cycle_count();
    uint64_t stalls=(cpu->gte_ts_done>now?cpu->gte_ts_done-now:0u)+
                    (cpu->muldiv_ts_done>now?cpu->muldiv_ts_done-now:0u);
    /* Per instruction: base1 + maximum cache refill7; data read maximum
     * region36 (SPU word) + ReadFudge2 + completion2 + DMA steal200;
     * newly issued GTE maximum43 and multiply/divide maximum37. Summing
     * mutually exclusive costs is intentionally conservative. */
    uint64_t bound=(uint64_t)count*(8u+240u+43u+37u)+stalls;
    if(bound<deadline)return (uint32_t)bound;
    uint32_t phys=pc&0x1fffffffu;
    if(count>0x80000u || phys>=0x200000u || (uint64_t)phys+4ull*count>0x200000u)return UINT32_MAX;
    bound=(uint64_t)count*8u+stalls;
    for(uint32_t i=0;i<count;i++) {
        uint32_t word;memcpy(&word,g_psx_ram+phys+4u*i,4);
        uint32_t op=word>>26,fn=word&63u;
        if((op>=0x20u && op<=0x26u) || op==0x32u)bound+=240u;
        if(op==0x12u && (word&(1u<<25)))bound+=psx_gte_cmd_latency(word);
        if(!op && fn>=0x18u && fn<=0x1bu)bound+=37u;
    }
    return bound>UINT32_MAX?UINT32_MAX:(uint32_t)bound;
}

/* Existing generated MFC2/CFC2 bodies assign the GPR eagerly. Their block
 * count includes branch delay slots, so inspect the validated live bytes and
 * select the precise value owner before executing such a block. This also
 * covers a move in JR's delay slot whose consumer is in the caller. Selection
 * changes neither guest bytes nor cycles. CU2/profile admission is the caller's
 * responsibility, just as for the timing bound above. */
static int source_cpu_block_gte_value_delay(uint32_t pc,uint32_t count) {
    uint32_t phys=pc&0x1fffffffu;
    if(count>0x80000u || phys>=0x200000u || (uint64_t)phys+4ull*count>0x200000u)return 1;
    for(uint32_t i=0;i<count;i++) {
        uint32_t word;memcpy(&word,g_psx_ram+phys+4u*i,4);
        uint32_t op=word>>26,rs=(word>>21)&31u,rt=(word>>16)&31u;
        if(op==0x12u && (rs==0u || rs==2u) && rt!=0u)return 1;
    }
    return 0;
}
#endif
