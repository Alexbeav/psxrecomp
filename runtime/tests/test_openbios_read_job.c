/* Differential caller-contract fixture. Link the actual generated original
 * read body (no instruction timing) beside the native implementation. Fake
 * only the command/event device boundary, not either request algorithm. */
#include "cpu_state.h"
#include "psx_cycles.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern void OpenBIOS_func_1FC03A78(CPUState *cpu);
extern void psx_lle_OpenBIOS_func_1FC03A78(CPUState *cpu);
uint64_t psx_cycle_count, psx_next_service_cycle;
int psx_in_device_service, g_event_step_conservative, g_ls_replay_active;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t g_psx_gcs_recip_q16 = 65536u, g_psx_gcs_frac;
volatile uint32_t g_psx_last_fn_entry;
uint32_t g_debug_last_store_pc;
static uint8_t ram[2u << 20];
static int scenario, stage, polls, seeks, reads, errors, callbacks, skips;
static uint32_t error_codes[16];
static uint64_t ready_at;
static uint32_t rw(uint32_t a) { uint32_t v; memcpy(&v,ram+(a&0x1FFFFFu),4); return v; }
uint32_t psx_read_word(uint32_t a) { return rw(a); }
static void ww(uint32_t a,uint32_t v) { memcpy(ram+(a&0x1FFFFFu),&v,4); }
static uint8_t rb(uint32_t a) { return ram[a&0x1FFFFFu]; }
static void wb(uint32_t a,uint8_t v) { ram[a&0x1FFFFFu]=v; }
static uint16_t rh(uint32_t a) { return (uint16_t)(rb(a)|(rb(a+1u)<<8)); }
static void wh(uint32_t a,uint16_t v) { wb(a,(uint8_t)v);wb(a+1u,(uint8_t)(v>>8)); }
void psx_devices_service_to_now(void) { psx_next_service_cycle=psx_cycle_count+1; }
void psx_advance_cycles_slow(uint32_t n) { psx_cycle_count+=n; }
uint32_t psx_idle_cycles_to_next_observable_event(void) { ++skips; return 16000; }
void psx_check_interrupts_at(CPUState *cpu,uint32_t pc) { (void)cpu;(void)pc; psx_cycle_count+=32; }
void psx_break(CPUState *cpu,uint32_t code,uint32_t pc) { (void)cpu;(void)code;(void)pc;assert(0); }
void psx_unaligned_access(CPUState *cpu,uint32_t addr,uint32_t pc) { (void)cpu;(void)addr;(void)pc;assert(0); }

typedef struct { uint32_t result,sp,callee[8],payload[4],codes[16]; int seek,read,error,callback; uint8_t msf[3]; } Outcome;
static Outcome run(int native,int which,int restore_each_boundary) {
    CPUState cpu={0};Outcome result={0};
    memset(ram,0,sizeof(ram));scenario=which;stage=polls=seeks=reads=errors=callbacks=skips=0;
    memset(error_codes,0,sizeof(error_codes));psx_cycle_count=0;psx_next_service_cycle=1;ready_at=0;
    cpu.read_word=rw;cpu.write_word=ww;cpu.read_byte=rb;cpu.write_byte=wb;cpu.read_half=rh;cpu.write_half=wh;
    cpu.gpr[29]=0x801F0000u;cpu.gpr[31]=0x80012340u;
    cpu.gpr[4]=0x80020000u;cpu.gpr[5]=12345;cpu.gpr[6]=3;
    for(int i=0;i<8;++i)cpu.gpr[16+i]=0x13570000u+i;
    cpu.pc=0xBFC03A78u;
    ww(0x4BE0,1);ww(0x4BD4,2);ww(0x4BD8,3);
    ww(0xB0,0x24082AA0);ww(0xB4,0x01000008);ww(0xB8,0);
    const uint32_t handler[]={0x3C080000,0x250849CC,0x00095080,0x01485020,0x8D4A0000,0x240800B0,0x01400008,0};
    for(unsigned i=0;i<8;++i)ww(0x2AA0+i*4,handler[i]);
    ww(0x49F8,which==7?0x80012300u:0xBFC0574Cu);
    for(unsigned step=0;cpu.pc!=0x80012340u;++step) {
        assert(step<6000000u);
        if(restore_each_boundary) {
            /* Model a serializer copying guest CPU/RAM then destroying the
             * live CPU. No native job state can hide in a host continuation. */
            CPUState saved=cpu;memset(&cpu,0,sizeof(cpu));cpu=saved;
        }
        if(cpu.pc==0xA0u) {
            unsigned selector=cpu.gpr[9];
            if(selector==0x78u) {
                ++seeks;assert(cpu.gpr[4]==cpu.gpr[29]+16u);
                for(unsigned i=0;i<3;++i)result.msf[i]=rb(cpu.gpr[4]+i);
                cpu.gpr[2]=which==1?0u:1u;stage=1;polls=0;ready_at=psx_cycle_count+48000;
            } else if(selector==0x7Eu) {
                ++reads;assert(cpu.gpr[4]==0x80020000u&&cpu.gpr[5]==3&&cpu.gpr[6]==0x80);
                cpu.gpr[2]=which==2?0u:1u;stage=2;polls=0;ready_at=psx_cycle_count+64000;
            } else {
                assert(selector==0xA1u&&cpu.gpr[4]==0x44u);assert(errors<16);
                error_codes[errors++]=cpu.gpr[5];cpu.gpr[2]=0;
            }
            cpu.pc=cpu.gpr[31];
        } else if(cpu.pc==0xB0u) {
            assert(cpu.gpr[9]==0x0Bu);++polls;psx_cycle_count+=128;
            unsigned event=(stage==1&&which==3)?2u:(stage==2&&which==5)?3u:
                (stage==2&&(which==6||(which==4&&reads==1)))?2u:1u;
            int ready=which==7?polls>=20:psx_cycle_count>=ready_at;
            cpu.gpr[2]=ready&&cpu.gpr[4]==event;
            if(cpu.gpr[2]) {
                ++callbacks;
                if(stage==2&&event==1u)for(unsigned i=0;i<4;++i)ww(0x20000u+i*4u,0xCAFEBABEu+i);
            }
            cpu.pc=cpu.gpr[31];
        } else if(native) OpenBIOS_func_1FC03A78(&cpu);
        else psx_lle_OpenBIOS_func_1FC03A78(&cpu);
    }
    result.result=cpu.gpr[2];result.sp=cpu.gpr[29];
    for(int i=0;i<8;++i)result.callee[i]=cpu.gpr[16+i];
    for(unsigned i=0;i<4;++i)result.payload[i]=rw(0x20000+i*4);
    memcpy(result.codes,error_codes,sizeof(error_codes));
    result.seek=seeks;result.read=reads;result.error=errors;result.callback=callbacks;
    if(which==7)assert(skips==0);
    return result;
}
int main(void) {
    for(int which=0;which<8;++which) {
        Outcome reference=run(0,which,0),native=run(1,which,0),restored=run(1,which,1);
        assert(memcmp(&reference,&native,sizeof(native))==0);
        assert(memcmp(&native,&restored,sizeof(native))==0);
        printf("read request scenario %d: caller outcomes and restored continuation match\n",which);
    }
    return 0;
}
