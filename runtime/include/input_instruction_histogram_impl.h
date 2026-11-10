/* Private debug-server implementation. Test-configured passive fetch counts.
 * No opcodes, registers, input words or guest memory are changed. */
#include "input_instruction_histogram.h"
static void input_instruction_histogram_record(uint32_t pc);
typedef struct {uint32_t pc,count;int used;uint64_t first_sequence,first_cycle,first_effective_cycle;uint32_t first_cache_tag;int first_cache_active;} InputInstructionHistogramRow;
static InputInstructionHistogramRow s_instruction_rows[16384];
static uint32_t s_instruction_first,s_instruction_last,s_instruction_frame;
static int s_instruction_configured;
static int s_instruction_partial;
static void input_instruction_histogram_boundary(uint32_t frame);
static void input_instruction_histogram_exit(void) {
    if(!g_input_instruction_histogram_active)return;
    s_instruction_partial=1;
    input_instruction_histogram_boundary(UINT32_MAX);
}
static uint64_t s_instruction_events,s_instruction_dropped,s_instruction_cycle;
typedef struct {uint32_t pc;uint64_t sequence,cycle,effective_cycle;uint32_t cache_tag;int cache_active;} InputInstructionSiteRow;
typedef struct {int precise,in_exception;uint32_t istat,imask,last_block,last_committed;uint64_t slices,takes;} InputInstructionContextRow;
static InputInstructionContextRow s_instruction_context_rows[8192];
extern int g_precise_mode;
extern uint32_t i_stat,i_mask,g_slice_last_block,g_slice_last_committed;
extern uint64_t g_slice_fired,g_slice_irq_taken;
extern int psx_get_in_exception(void);
extern uint32_t g_psx_cyc_batch;
extern uint32_t *g_psx_cyc_local_acc;
extern uint32_t g_psx_icache_tv[1024];
extern int g_psx_icache_active;
/* Observe the pending clock without publishing it or servicing a device. */
static uint64_t input_instruction_effective_cycle(void) {
    return psx_cycle_count + g_psx_cyc_batch +
           (g_psx_cyc_local_acc ? (uint64_t)*g_psx_cyc_local_acc : 0u);
}
static uint32_t s_instruction_sites[16],s_instruction_site_count;
static InputInstructionSiteRow s_instruction_site_rows[8192];
static uint32_t s_instruction_site_retained,s_instruction_site_dropped;
static int input_instruction_sites_parse(const char *p) {
    s_instruction_site_count=0;
    if(!p)return 1;
    for(;;) {
        uint32_t pc=0;
        if(s_instruction_site_count==16)return 0;
        for(unsigned i=0;i<8;i++) {
            unsigned c=(unsigned char)*p++,v;
            if(c>='0' && c<='9')v=c-'0';
            else if(c>='A' && c<='F')v=c-'A'+10;
            else if(c>='a' && c<='f')v=c-'a'+10;
            else return 0;
            pc=(pc<<4)|v;
        }
        if(pc&3)return 0;
        for(unsigned i=0;i<s_instruction_site_count;i++)if(s_instruction_sites[i]==pc)return 0;
        s_instruction_sites[s_instruction_site_count++]=pc;
        if(!*p)return 1;
        if(*p++!=',')return 0;
    }
}
static int input_instruction_histogram_configure(void) {
    const char *p=getenv("PSX_INPUT_HISTOGRAM_RANGE");
    const char *sites=getenv("PSX_INPUT_HISTOGRAM_SITES");
    if(!p)return sites==NULL;
    unsigned first,last;char extra;
    if(sscanf(p,"%u,%u%c",&first,&last,&extra)!=2 || last<first ||
       last-first>7 || last>60000)return 0;
    if(!input_instruction_sites_parse(sites))return 0;
    if(!s_instruction_configured && atexit(input_instruction_histogram_exit))return 0;
    s_instruction_first=first;s_instruction_last=last;s_instruction_configured=1;
    g_input_instruction_histogram_callback=input_instruction_histogram_record;
    /* Configuration runs before guest execution. Interval zero ends at the
     * first runtime VBlank; it previously fell outside every fetch capture. */
    if(first==0)input_instruction_histogram_boundary(0);
    return 1;
}
static void input_instruction_histogram_record(uint32_t pc) {
    if(!g_input_instruction_histogram_active || g_ls_replay_active)return;
    if(s_instruction_events==1000000){s_instruction_dropped++;return;}
    for(unsigned i=0;i<s_instruction_site_count;i++)if(s_instruction_sites[i]==pc) {
        if(s_instruction_site_retained==8192)s_instruction_site_dropped++;
        else {
            unsigned n=s_instruction_site_retained++;
            s_instruction_site_rows[n]=(InputInstructionSiteRow){pc,s_instruction_events,psx_cycle_count,input_instruction_effective_cycle(),g_psx_icache_tv[(pc & 0xFFCu)>>2],g_psx_icache_active};
            s_instruction_context_rows[n]=(InputInstructionContextRow){g_precise_mode,psx_get_in_exception(),i_stat,i_mask,g_slice_last_block,g_slice_last_committed,g_slice_fired,g_slice_irq_taken};
        }
        break;
    }
    uint32_t slot=((pc>>2)*2654435761u)&16383u;
    for(unsigned n=0;n<16384;n++,slot=(slot+1)&16383u) {
        InputInstructionHistogramRow *row=&s_instruction_rows[slot];
        if(!row->used || row->pc==pc){
            if(!row->used){row->first_sequence=s_instruction_events;row->first_cycle=psx_cycle_count;row->first_effective_cycle=input_instruction_effective_cycle();row->first_cache_tag=g_psx_icache_tv[(pc & 0xFFCu)>>2];row->first_cache_active=g_psx_icache_active;}
            row->used=1;row->pc=pc;row->count++;s_instruction_events++;return;
        }
    }
    s_instruction_dropped++;
}
static void input_instruction_histogram_boundary(uint32_t frame) {
    if(g_input_instruction_histogram_active) {
        const char *prefix=s_instruction_partial?"partial-":"";
        char name[64];snprintf(name,sizeof(name),"%sinstructions-%06u.tsv",prefix,s_instruction_frame);
        FILE *f=input_route_observer_output(name);
        fprintf(f,"# retained=%llu dropped=%llu start_cycle=%llu end_cycle=%llu\npc\tcount\n",
                (unsigned long long)s_instruction_events,(unsigned long long)s_instruction_dropped,
                (unsigned long long)s_instruction_cycle,(unsigned long long)psx_cycle_count);
        for(unsigned i=0;i<16384;i++)if(s_instruction_rows[i].used)
            fprintf(f,"%08X\t%u\n",s_instruction_rows[i].pc,s_instruction_rows[i].count);
        if(fclose(f)){fprintf(stderr,"instruction histogram close failed\n");exit(4);}
        snprintf(name,sizeof(name),"%sinstruction-first-%06u.tsv",prefix,s_instruction_frame);
        f=input_route_observer_output(name);
        fprintf(f,"# first observed cache-fetch site per PC; not complete retired execution; effective clock includes unpublished charges\npc\tfirst_sequence\tfirst_cycle\teffective_cycle\tcache_tag\tcache_active\n");
        for(unsigned i=0;i<16384;i++)if(s_instruction_rows[i].used)
            fprintf(f,"%08X\t%llu\t%llu\t%llu\t%08X\t%d\n",s_instruction_rows[i].pc,
                (unsigned long long)s_instruction_rows[i].first_sequence,
                (unsigned long long)s_instruction_rows[i].first_cycle,
                (unsigned long long)s_instruction_rows[i].first_effective_cycle,
                s_instruction_rows[i].first_cache_tag,s_instruction_rows[i].first_cache_active);
        if(fclose(f)){fprintf(stderr,"histogram first-site close failed\n");exit(4);}
        if(s_instruction_site_count) {
            snprintf(name,sizeof(name),"%sinstruction-sites-%06u.tsv",prefix,s_instruction_frame);
            f=input_route_observer_output(name);
            fprintf(f,"# selected cache-fetch sites; retained=%u dropped=%u; cycles before fetch; effective clock includes unpublished charges\npc\tsequence\tcycle\teffective_cycle\tcache_tag\tcache_active\n",s_instruction_site_retained,s_instruction_site_dropped);
            for(unsigned i=0;i<s_instruction_site_retained;i++) {
                InputInstructionSiteRow *row=&s_instruction_site_rows[i];
                fprintf(f,"%08X\t%llu\t%llu\t%llu\t%08X\t%d\n",row->pc,(unsigned long long)row->sequence,(unsigned long long)row->cycle,(unsigned long long)row->effective_cycle,row->cache_tag,row->cache_active);
            }
            if(fclose(f)){fprintf(stderr,"instruction sites close failed\n");exit(4);}
            snprintf(name,sizeof(name),"%sinstruction-context-%06u.tsv",prefix,s_instruction_frame);
            f=input_route_observer_output(name);
            fprintf(f,"# passive context at selected pre-fetch sites; same sequence as instruction-sites; no device service\npc\tsequence\tprecise\tin_exception\ti_stat\ti_mask\tslice_block\tslice_committed\tslices\ttakes\n");
            for(unsigned i=0;i<s_instruction_site_retained;i++) {
                InputInstructionSiteRow *site=&s_instruction_site_rows[i];
                InputInstructionContextRow *row=&s_instruction_context_rows[i];
                fprintf(f,"%08X\t%llu\t%d\t%d\t%08X\t%08X\t%08X\t%08X\t%llu\t%llu\n",site->pc,(unsigned long long)site->sequence,row->precise,row->in_exception,row->istat,row->imask,row->last_block,row->last_committed,(unsigned long long)row->slices,(unsigned long long)row->takes);
            }
            if(fclose(f)){fprintf(stderr,"instruction context close failed\n");exit(4);}
        }
        snprintf(name,sizeof(name),"%sdma-at-%06u.json",prefix,s_instruction_frame);
        f=input_route_observer_output(name);
        debug_server_freeze_dump_dma_trace_json(f,128);
        if(fclose(f)){fprintf(stderr,"histogram DMA context close failed\n");exit(4);}
        snprintf(name,sizeof(name),"%ssio-at-%06u.json",prefix,s_instruction_frame);
        f=input_route_observer_output(name);
        debug_server_freeze_dump_sio_pc_json(f,128);
        if(fclose(f)){fprintf(stderr,"histogram SIO context close failed\n");exit(4);}
        g_input_instruction_histogram_active=0;
    }
    if(s_instruction_configured && frame>=s_instruction_first && frame<=s_instruction_last) {
        memset(s_instruction_rows,0,sizeof(s_instruction_rows));
        s_instruction_frame=frame;s_instruction_events=s_instruction_dropped=0;
        s_instruction_site_retained=s_instruction_site_dropped=0;
        s_instruction_cycle=psx_cycle_count;g_input_instruction_histogram_active=1;
    }
}
