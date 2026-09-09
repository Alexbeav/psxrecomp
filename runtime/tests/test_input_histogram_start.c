/* Passive observer range/ordering regression. No guest execution is involved. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
uint64_t psx_cycle_count;
uint32_t g_psx_cyc_batch,*g_psx_cyc_local_acc,g_psx_icache_tv[1024];
int g_psx_icache_active=1,g_ls_replay_active,g_input_instruction_histogram_active;
void(*g_input_instruction_histogram_callback)(uint32_t);
static FILE*input_route_observer_output(const char *name){FILE *f=fopen(name,"rb");assert(!f);f=fopen(name,"wb");assert(f);return f;}
static void debug_server_freeze_dump_dma_trace_json(FILE*f,int n){(void)n;fputs("{}\n",f);}
static void debug_server_freeze_dump_sio_pc_json(FILE*f,int n){(void)n;fputs("{}\n",f);}
#include PSX_TEST_HISTOGRAM_IMPLEMENTATION
int main(int argc,char**argv){
 assert(argc==3);int kind=atoi(argv[2]);
 _putenv_s("PSX_INPUT_HISTOGRAM_RANGE",strcmp(argv[1],"none")?argv[1]:"");
 if(!strcmp(argv[1],"none"))_putenv_s("PSX_INPUT_HISTOGRAM_RANGE","");
 _putenv_s("PSX_INPUT_HISTOGRAM_SITES",kind==3?"00001000,00001000":"");
 int ok=input_instruction_histogram_configure();
 if(kind==2||kind==3){printf("rejected=%d\n",!ok);return ok?1:0;}
 if(!ok){fprintf(stderr,"valid range rejected: %s\n",argv[1]);return 1;}
 uint32_t tags[1024];memcpy(tags,g_psx_icache_tv,sizeof(tags));
 psx_cycle_count=7;input_instruction_histogram_record(0x1000);
 psx_cycle_count=10;input_instruction_histogram_boundary(1);
 psx_cycle_count=11;input_instruction_histogram_record(0x1004);
 psx_cycle_count=20;input_instruction_histogram_boundary(2);
 assert(!memcmp(tags,g_psx_icache_tv,sizeof(tags)) && psx_cycle_count==20);
 printf("PASS passive range %s\n",argv[1]);return 0;
}
