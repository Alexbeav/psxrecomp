/* Authored guest runner linked to complete production runtime objects. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "cpu_state.h"
#include "psx_bios_backend.h"
#include "dirty_ram_interp.h"
#include "psx_icache.h"
#include "psx_cycles.h"
#include "interrupts.h"
#include "timers.h"
#include "source_gpu_runtime.h"
#include "overlay_loader.h"
extern uint8_t *memory_get_ram_ptr(void);
extern uint32_t psx_read_word(uint32_t);
extern uint16_t psx_read_half(uint32_t);
extern uint8_t psx_read_byte(uint32_t);
extern void psx_write_word(uint32_t,uint32_t);
extern void psx_write_half(uint32_t,uint16_t);
extern void psx_write_byte(uint32_t,uint8_t);
extern void memory_set_sr_ptr(const uint32_t*);
extern void psx_irq_set_cause_ptr(uint32_t*);
extern void psx_rfe_escape_check(CPUState*);
extern CPUState *debug_cpu_ptr;
static jmp_buf done;
static unsigned steps;static int precise;
#ifdef L1_OVERLAY
static unsigned overlay_entries;
#endif
extern int g_psx_precise_slice;
extern int psx_get_in_exception(void);
static void boundary(uint32_t pc){
 (void)pc;
 if(psx_read_word(0x100)==0xfeed1234u)longjmp(done,1);
 if(++steps>100000){fprintf(stderr,"instruction limit pc=%08x\n",pc);exit(8);}
}
#ifdef L1_NATIVE
extern int psx_dispatch_game_compiled(CPUState*,uint32_t);
static unsigned native_entries;
#endif
static int entry(uint32_t p){return (p&0x1fffffffu)<0x200000u && !(p&3);}
static void dispatch_call(CPUState*c,uint32_t p,uint32_t stop){
 c->pc=p;
 while(c->pc && c->pc!=stop){
#ifdef L1_NATIVE
  native_entries++;if(psx_dispatch_game_compiled(c,c->pc)){psx_rfe_escape_check(c);continue;}native_entries--;
#endif
#ifdef L1_OVERLAY
  overlay_entries++;if(overlay_loader_dispatch(c,c->pc)){psx_rfe_escape_check(c);continue;}overlay_entries--;
#endif
  int handled=precise && !psx_get_in_exception() ? psx_slice_block_impl(c,c->pc,1,1) : 0;
  if(!handled)handled=dirty_ram_dispatch(c,c->pc,stop);
  if(!handled){fprintf(stderr,"dispatch miss %08x\n",c->pc);exit(9);}
  psx_rfe_escape_check(c);
 }
}
static void dispatch(CPUState*c,uint32_t p){dispatch_call(c,p,0);}
static const PsxBiosImageInfo info={.image_id="L1-AUTHORED",.image_sha256="",.image_size=0};
static const PsxBiosBackend backend={.image=&info,.dispatch=dispatch,.dispatch_call=dispatch_call,.is_entry=entry};
int main(int argc,char**argv){
 if(argc<2)return 2;precise=argc>2&&!strcmp(argv[2],"precise");g_psx_precise_slice=precise;
 if(argc>2&&!strcmp(argv[2],"handoff")){extern int g_psx_slice_irq_handoff,g_psx_slice_slot_take;g_psx_slice_irq_handoff=g_psx_slice_slot_take=1;}
 FILE*f=fopen(argv[1],"rb");if(!f)return 3;
 if(fread(memory_get_ram_ptr(),1,0x200000,f)!=0x200000)return 4;fclose(f);
#ifdef L1_NATIVE
 uint8_t *original=malloc(0x40040);memcpy(original,memory_get_ram_ptr(),0x40040);dirty_ram_register_text_image(0,original,0x40040);
#endif
 static CPUState c;c.pc=0x80001000;c.read_word=psx_read_word;c.read_half=psx_read_half;c.read_byte=psx_read_byte;c.write_word=psx_write_word;c.write_half=psx_write_half;c.write_byte=psx_write_byte;c.read_fudge=32;c.ld_which_t=32;
 psx_bios_activate(&backend);debug_cpu_ptr=&c;memory_set_sr_ptr(&c.cop0[12]);psx_irq_set_cause_ptr(&c.cop0[13]);
 dirty_ram_mark_executable_range(0x80,0x1ff0);psx_icache_reset();g_psx_icache_active=1;timers_init();source_gpu_runtime_init();
#ifdef L1_OVERLAY
 if(argc<4)return 11;overlay_loader_init(argv[3],"PAIR-TEST",0);
 fprintf(stderr,"overlay registered=%d\n",overlay_loader_registered_count());
 if(overlay_loader_registered_count()==0)return 12;
#endif
 g_input_instruction_histogram_active=1;g_input_instruction_histogram_callback=boundary;
 if(!setjmp(done))dispatch(&c,c.pc);
#ifdef L1_NATIVE
 fprintf(stderr,"native entries=%u\n",native_entries);
 if(!native_entries)return 10;
#endif
#ifdef L1_OVERLAY
 fprintf(stderr,"overlay entries=%u\n",overlay_entries);
 if(!overlay_entries)return 13;
#endif
 for(unsigned j=0;j<20;j++)printf("%08x%c",psx_read_word(j<16?0x100000+4*j:0x100800+4*(j-16)),j==19?'\n':' ');
 return 0;
}
