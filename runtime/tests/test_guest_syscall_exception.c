/* Verify architectural exception entry without any guest-memory or scheduler calls.
 * The Bio Hazard cold comparison exercises syscall routing through this entry. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "../src/traps.c"
int source_gpu_runtime_active(void) { return 1; }
int psx_get_in_exception(void) { return 0; }
int main(void) {
 for(unsigned f=0; f<8; f++) {
  CPUState c={0},e;
  for(unsigned i=0;i<32;i++)c.gpr[i]=0x1000+i;
  c.gpr[4]=f;c.pc=0x650;c.cop0[12]=0x40000401;c.cop0[13]=0xf000057c;
  e=c;e.pc=0x80000080;e.cop0[14]=0x650;e.cop0[12]=0x40000404;e.cop0[13]=0x520;
  assert(enter_guest_syscall_exception(&c)==1);assert(!memcmp(&c,&e,sizeof(c)));
 }
 return 0;
}
