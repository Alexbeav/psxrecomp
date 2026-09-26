#ifndef PSX_SOURCE_CPU_BLOCK_BOUND_H
#define PSX_SOURCE_CPU_BLOCK_BOUND_H
/* A generated block's bcyc is its instruction count. It excludes cache
 * refills, data waits and coprocessor interlocks, so it cannot alone prove
 * that a pending IRQ lies beyond the block. This source-profile bound only
 * selects the existing instruction interpreter; it never charges cycles.
 * The caller restricts this to ordinary compiled RAM blocks without MMIO
 * visibility side effects. Their bytes have already passed native-text checks. */
/* access_cost: worst case for one data access. 240 = region36 (SPU word) +
 * ReadFudge2 + completion2 + DMA steal200. The 200-cycle steal can only occur
 * while a source DMA transfer is live; callers that know no transfer is live
 * (dma_source_dma_live all zero, not halted) may pass 40. */
static uint32_t source_cpu_block_bound_ex(CPUState *cpu,uint32_t pc,uint32_t count,uint32_t deadline,uint32_t access_cost) {
    uint64_t now=psx_get_cycle_count();
    uint64_t stalls=(cpu->gte_ts_done>now?cpu->gte_ts_done-now:0u)+
                    (cpu->muldiv_ts_done>now?cpu->muldiv_ts_done-now:0u);
    /* Per instruction: base1 + maximum cache refill7; data read maximum
     * access_cost; newly issued GTE maximum43 and multiply/divide maximum37.
     * Summing mutually exclusive costs is intentionally conservative. */
    uint64_t bound=(uint64_t)count*(8u+access_cost+43u+PSX_DIV_LATENCY)+stalls;
    if(bound<deadline)return (uint32_t)bound;
    uint32_t phys=pc&0x1fffffffu;
    if(count>0x80000u || phys>=0x200000u || (uint64_t)phys+4ull*count>0x200000u)return UINT32_MAX;
    bound=(uint64_t)count*8u+stalls;
    for(uint32_t i=0;i<count;i++) {
        uint32_t word;memcpy(&word,g_psx_ram+phys+4u*i,4);
        uint32_t op=word>>26,fn=word&63u;
        if((op>=0x20u && op<=0x26u) || op==0x32u)bound+=access_cost;
        if(op==0x12u && (word&(1u<<25)))bound+=psx_gte_cmd_latency(word);
        if(!op && fn>=0x18u && fn<=0x1bu)bound+=37u;
    }
    return bound>UINT32_MAX?UINT32_MAX:(uint32_t)bound;
}
static uint32_t source_cpu_block_bound(CPUState *cpu,uint32_t pc,uint32_t count,uint32_t deadline) {
    return source_cpu_block_bound_ex(cpu,pc,count,deadline,240u);
}
/* Step-3 candidate: refill charged per I-cache line the block will miss, from
 * the live tag state, instead of 7 per instruction. A miss fills the rest of
 * its 16-byte line (psx_icache_fetch_miss), so sequential words after the miss
 * word hit. Sound because the CPU alone fetches between the leader and the
 * block's words; a nested handler can evict lines, but every subsequent
 * instruction still passes the boundary callback where devices, halts and
 * IRQs are handled, so a longer block only delays nothing guest-visible.
 * Falls back to the per-instruction figure for blocks that could alias the
 * 4 KiB direct-mapped cache or run uncached (KSEG1: 4 per word). */
extern uint32_t g_psx_icache_tv[1024];
extern int g_psx_icache_active;
/* Step-4a: per-block instruction facts, computed once for clean game text
 * (whose bytes equal the recompiled image, so the facts are immutable) and
 * recomputed on every visit for anything else. Removes the O(N) word scans the
 * guard repeated on each of ~580 M leader visits. Direct-mapped by word index;
 * a colliding block simply replaces the entry. */
typedef struct { uint32_t addr, count, accesses, extra_latency; uint8_t gte_delay, valid; } SourceCpuBlockFacts;
#define SOURCE_CPU_FACTS_LOG2 16
static SourceCpuBlockFacts s_source_cpu_facts[1u<<SOURCE_CPU_FACTS_LOG2];
static uint64_t g_source_cpu_facts_hits, g_source_cpu_facts_fills, g_source_cpu_facts_uncached;
static void source_cpu_block_facts_scan(uint32_t phys,uint32_t count,SourceCpuBlockFacts *f) {
    f->accesses=0; f->extra_latency=0; f->gte_delay=0;
    for(uint32_t i=0;i<count;i++) {
        uint32_t word;memcpy(&word,g_psx_ram+phys+4u*i,4);
        uint32_t op=word>>26,fn=word&63u,rs=(word>>21)&31u,rt=(word>>16)&31u;
        if((op>=0x20u && op<=0x26u) || op==0x32u)f->accesses++;
        if(op==0x12u && (word&(1u<<25)))f->extra_latency+=psx_gte_cmd_latency(word);
        if(!op && fn>=0x18u && fn<=0x1bu)f->extra_latency+=PSX_DIV_LATENCY;
        if(op==0x12u && (rs==0u || rs==2u) && rt!=0u)f->gte_delay=1;
    }
}
static const SourceCpuBlockFacts *source_cpu_block_facts(uint32_t pc,uint32_t count,int cacheable) {
    static SourceCpuBlockFacts scratch;
    uint32_t phys=pc&0x1fffffffu;
    if(!cacheable) { g_source_cpu_facts_uncached++; source_cpu_block_facts_scan(phys,count,&scratch); return &scratch; }
    SourceCpuBlockFacts *f=&s_source_cpu_facts[(pc>>2)&((1u<<SOURCE_CPU_FACTS_LOG2)-1u)];
    if(f->valid && f->addr==pc && f->count==count) { g_source_cpu_facts_hits++; return f; }
    g_source_cpu_facts_fills++;
    source_cpu_block_facts_scan(phys,count,f); f->addr=pc; f->count=count; f->valid=1;
    return f;
}
/* icache bound from precomputed facts; refill checked per line on the first
 * word the block touches in that line (a fill marks a suffix of the line
 * valid, so a valid first word implies the rest of the line is valid). */
static uint32_t source_cpu_block_bound_facts(CPUState *cpu,uint32_t pc,uint32_t count,uint32_t access_cost,const SourceCpuBlockFacts *f) {
    uint64_t now=psx_get_cycle_count();
    uint64_t stalls=(cpu->gte_ts_done>now?cpu->gte_ts_done-now:0u)+
                    (cpu->muldiv_ts_done>now?cpu->muldiv_ts_done-now:0u);
    uint64_t bound=(uint64_t)count+stalls+(uint64_t)f->accesses*access_cost+f->extra_latency;
    if(pc>=0xA0000000u || g_psx_icache_active<0 || 4ull*count>4096ull) bound+=(uint64_t)count*7u;
    else if(g_psx_icache_active) {
        uint32_t end=pc+4u*count;
        for(uint32_t w=pc; w<end; w=(w&0xFFFFFFF0u)+16u) {       /* first touched word of each line */
            if(g_psx_icache_tv[(w&0xFFCu)>>2]!=w) bound+=7u;
        }
    }
    return bound>UINT32_MAX?UINT32_MAX:(uint32_t)bound;
}
static uint32_t source_cpu_block_bound_icache(CPUState *cpu,uint32_t pc,uint32_t count,uint32_t access_cost) {
    uint64_t now=psx_get_cycle_count();
    uint64_t stalls=(cpu->gte_ts_done>now?cpu->gte_ts_done-now:0u)+
                    (cpu->muldiv_ts_done>now?cpu->muldiv_ts_done-now:0u);
    uint32_t phys=pc&0x1fffffffu;
    if(count>0x80000u || phys>=0x200000u || (uint64_t)phys+4ull*count>0x200000u)return UINT32_MAX;
    uint64_t bound=(uint64_t)count+stalls;              /* base 1 per instruction */
    if(pc>=0xA0000000u || g_psx_icache_active<0 || 4ull*count>4096ull) bound+=(uint64_t)count*7u;
    else if(g_psx_icache_active) {
        uint32_t filled_line=0xFFFFFFFFu;
        for(uint32_t i=0;i<count;i++) {
            uint32_t w=pc+4u*i;
            if(g_psx_icache_tv[(w&0xFFCu)>>2]==w) continue;          /* hit */
            if((w&0xFFFFFFF0u)==filled_line) continue;                /* filled by this block's earlier miss */
            bound+=7u; filled_line=w&0xFFFFFFF0u;
        }
    }
    for(uint32_t i=0;i<count;i++) {
        uint32_t word;memcpy(&word,g_psx_ram+phys+4u*i,4);
        uint32_t op=word>>26,fn=word&63u;
        if((op>=0x20u && op<=0x26u) || op==0x32u)bound+=access_cost;
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
