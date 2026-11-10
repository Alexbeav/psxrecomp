/* Authored public-port receipt: source FIFO spec S1/S6 and finite stock F1
 * reset observation. No media, reference implementation or timing inference. */
#include "mdec.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint64_t s_frame_count, psx_cycle_count;
int debug_server_fmv_quiet(void) { return 0; }
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--default")){
        mdec_init();return mdec_read(0x1f801824u)!=0x8604ffffu;
    }
    mdec_init();
    uint32_t status = mdec_read(0x1f801824u);
    printf("source MDEC power status=%08x expected=80000000\n", status);
    if (status != 0x80000000u) return 1;
    mdec_write(0x1f801824u, 0xe0000000u);
    status = mdec_read(0x1f801824u);
    printf("source MDEC reset status=%08x expected=80000000\n", status);
    return status != 0x80000000u;
}
