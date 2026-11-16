#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int g_input_instruction_histogram_active;
void (*g_input_instruction_histogram_callback)(uint32_t pc);
int g_ls_replay_active;
uint64_t psx_cycle_count=1000;
uint32_t g_psx_cyc_batch;
uint32_t *g_psx_cyc_local_acc;
uint32_t g_psx_icache_tv[1024];
int g_psx_icache_active=1;
int g_precise_mode;
uint32_t i_stat,i_mask,g_slice_last_block,g_slice_last_committed;
uint64_t g_slice_fired,g_slice_irq_taken;
static int in_exception;
int psx_get_in_exception(void){return in_exception;}
static unsigned outputs;
static unsigned partial_outputs;
static FILE *input_route_observer_output(const char *name) {
    if(strstr(name,"partial-")==name){partial_outputs++;name+=8;}
    assert(strstr(name,"instructions-")==name || strstr(name,"instruction-first-")==name || strstr(name,"instruction-sites-")==name || strstr(name,"instruction-context-")==name || strstr(name,"dma-at-")==name || strstr(name,"sio-at-")==name);
    if(strstr(name,"instructions-")==name)outputs++;
    return tmpfile();
}
static void debug_server_freeze_dump_dma_trace_json(FILE *f,uint32_t count) {
    assert(count==128);fputs("{}",f);
}
static void debug_server_freeze_dump_sio_pc_json(FILE *f,uint32_t count) {
    assert(count==128);fputs("[]",f);
}
#include "input_instruction_histogram_impl.h"
int main(void) {
    assert(input_instruction_sites_parse(NULL));
    const char *invalid[]={"","8000000","80000000,","80000002","80000000,80000000","8000000G","80000000x"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++)assert(!input_instruction_sites_parse(invalid[i]));
    assert(!input_instruction_sites_parse("00000000,00000004,00000008,0000000C,00000010,00000014,00000018,0000001C,00000020,00000024,00000028,0000002C,00000030,00000034,00000038,0000003C,00000040"));
    assert(input_instruction_sites_parse("80001000,80001004"));
    s_instruction_configured=1;s_instruction_first=10;s_instruction_last=11;
    input_instruction_histogram_boundary(9);
    input_instruction_histogram_record(0x80001000);
    assert(!g_input_instruction_histogram_active && s_instruction_events==0);
    input_instruction_histogram_boundary(10);
    for(unsigned i=0;i<100;i++)input_instruction_histogram_record(0x80001000);
    input_instruction_histogram_record(0x80001004);
    assert(s_instruction_events==101 && s_instruction_dropped==0 && psx_cycle_count==1000);
    assert(s_instruction_site_retained==101 && s_instruction_site_rows[100].pc==0x80001004 && s_instruction_site_rows[100].cycle==1000 && s_instruction_site_rows[100].sequence==100);
    unsigned observed_first=0;
    for(unsigned i=0;i<16384;i++)if(s_instruction_rows[i].used) {
        assert(s_instruction_rows[i].first_cycle==1000);
        assert(s_instruction_rows[i].first_sequence==(s_instruction_rows[i].pc==0x80001000?0:100));
        observed_first++;
    }
    assert(observed_first==2);
    g_ls_replay_active=1;input_instruction_histogram_record(0x80001000);g_ls_replay_active=0;
    assert(s_instruction_events==101);
    assert(s_instruction_site_retained==101);
    input_instruction_histogram_boundary(11);
    assert(outputs==1 && s_instruction_events==0 && g_input_instruction_histogram_active);
    for(unsigned i=0;i<1000003;i++)input_instruction_histogram_record(0x80001000);
    assert(s_instruction_events==1000000 && s_instruction_dropped==3);
    assert(s_instruction_site_retained==8192 && s_instruction_site_dropped==1000000-8192);
    input_instruction_histogram_boundary(12);
    assert(outputs==2 && !g_input_instruction_histogram_active);
    s_instruction_first=s_instruction_last=20;input_instruction_histogram_boundary(20);
    for(unsigned i=0;i<16385;i++)input_instruction_histogram_record(0x80000000+i*4);
    assert(s_instruction_events==16384 && s_instruction_dropped==1);
    input_instruction_histogram_boundary(21);
    s_instruction_first=s_instruction_last=30;input_instruction_histogram_boundary(30);
    uint32_t local=17;
    g_psx_cyc_batch=31;g_psx_cyc_local_acc=&local;
    g_psx_icache_tv[0]=0x80001002u;
    g_precise_mode=1;in_exception=1;i_stat=8;i_mask=9;
    g_slice_fired=43;g_slice_irq_taken=9;g_slice_last_block=0x80000100;g_slice_last_committed=0x80000200;
    input_instruction_histogram_record(0x80001000);
    assert(s_instruction_context_rows[0].precise==1 && s_instruction_context_rows[0].in_exception==1);
    assert(s_instruction_context_rows[0].istat==8 && s_instruction_context_rows[0].imask==9);
    assert(s_instruction_context_rows[0].slices==43 && s_instruction_context_rows[0].takes==9);
    assert(s_instruction_context_rows[0].last_block==0x80000100 && s_instruction_context_rows[0].last_committed==0x80000200);
    assert(g_slice_fired==43 && g_slice_irq_taken==9 && i_stat==8);
    assert(s_instruction_site_rows[0].cycle==1000);
    assert(s_instruction_site_rows[0].effective_cycle==1048);
    assert(s_instruction_site_rows[0].cache_tag==0x80001002u && s_instruction_site_rows[0].cache_active==1);
    assert(psx_cycle_count==1000 && g_psx_cyc_batch==31 && local==17 && g_psx_icache_tv[0]==0x80001002u);
    g_psx_cyc_local_acc=NULL;g_psx_cyc_batch=0;g_psx_icache_active=0;
    input_instruction_histogram_record(0x80001004);
    assert(s_instruction_site_rows[1].effective_cycle==1000 && s_instruction_site_rows[1].cache_active==0);
    for(unsigned i=0;i<16384;i++)if(s_instruction_rows[i].used && s_instruction_rows[i].pc==0x80001000) {
        assert(s_instruction_rows[i].first_cycle==1000 && s_instruction_rows[i].first_effective_cycle==1048);
        assert(s_instruction_rows[i].first_cache_tag==0x80001002u && s_instruction_rows[i].first_cache_active==1);
    }
    input_instruction_histogram_boundary(31);
    s_instruction_first=s_instruction_last=40;input_instruction_histogram_boundary(40);
    input_instruction_histogram_record(0x80001000);
    uint64_t before=psx_cycle_count;unsigned old_outputs=outputs;
    input_instruction_histogram_exit();
    assert(!g_input_instruction_histogram_active && partial_outputs==6);
    assert(outputs==old_outputs+1 && psx_cycle_count==before && s_instruction_events==1);
    input_instruction_histogram_exit();assert(partial_outputs==6);
    /* Configuration parsing is separately checked by the launcher. */
    (void)input_instruction_histogram_configure;
    puts("instruction histogram: counts, interval reset, event/entry caps, shadow exclusion and passive cycles PASS");
}
