/* The source-only overlay flavor publishes DLL-local cycles before the host
 * instruction boundary. The ordinary v37 flavor preserves its independent boundary behavior. */
#define PSX_OVERLAY_DLL_BUILD 1
#define PSX_NO_DEBUG_TOOLS 1
#include "overlay_dispatch_preamble.c.inc"
#include "source_fixture_assert.h"
#include "psx_bios_module.h"
static uint32_t charged,last_pc,calls;
static void charge(uint32_t n){charged+=n;}
#ifdef PSX_SOURCE_BUS_BOUNDARY
static void boundary(CPUState *cpu,uint32_t pc){assert(charged==7);assert(cpu->gpr[1]==123);last_pc=pc;++calls;}
#endif
int main(void){
 OverlayCallbacks cb={0};CPUState cpu={0};cpu.gpr[1]=123;cb.cpu_charge=charge;
#ifdef PSX_SOURCE_BUS_BOUNDARY
 cb.source_bus_boundary=boundary;
#endif
 overlay_init(&cb);psx_cpu_charge(7);psx_source_bus_boundary(&cpu,0x80010004);
#ifdef PSX_SOURCE_BUS_BOUNDARY
 assert(calls==1 && last_pc==0x80010004 && charged==7);
 assert(overlay_abi()==(37|(4<<16)));
 assert(PSX_BIOS_MODULE_ABI_TAG==(2|(4<<16)));
#else
 assert(!calls && !last_pc && !charged);overlay_flush_cycles();assert(charged==7);
 assert(overlay_abi()==37);
 assert(PSX_BIOS_MODULE_ABI_TAG==2);
#endif
 printf("source overlay flavor=%u calls=%u charged=%u pc=%08x PASS\n",(unsigned)PSX_OVERLAY_EFFECTIVE_FLAVOR,calls,charged,last_pc);return 0;
}
