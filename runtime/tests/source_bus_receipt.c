/* Authored public-register witness: selected source GPU block waits for DREQ.
 * Spec186 2.4/7.1, accepted test_dma_source_spec2 upload readiness.
 * No display/BIOS/disc; existing default DMA fixture supplies memory/IRQ seams. */
#include <stdlib.h>
#define main default_dma_hold_main
#include "test_dma_gpu_list_cpu_hold.c"
#undef main
int gpu_dma_source_ll_ready(void){return 0;}
int gpu_dma_walk_ready(void){return 0;}
uint32_t gpu_dma_vram_upload_words(void){return 65536;}
int main(void){
#ifdef _WIN32
 _putenv_s("PSX_INPUT_ROUTE_FILE","authored-fixture");
 _putenv_s("PSX_GPU_DMA_MODEL","octoshock-2.2.2-bounded-quad");
#else
 setenv("PSX_INPUT_ROUTE_FILE","authored-fixture",1);
 setenv("PSX_GPU_DMA_MODEL","octoshock-2.2.2-bounded-quad",1);
#endif
 psx_cycle_count=0;dma_init();s_gp0_n=0;
 dma_write(0x1f8010f0,0x800);
 dma_write(0x1f8010a0,0x1000);dma_write(0x1f8010a4,0x10020);
 dma_write(0x1f8010a8,0x11000201);
 uint32_t chcr=dma_read(0x1f8010a8),bcr=dma_read(0x1f8010a4);
 printf("SOURCE_BUS_GPU_HELD chcr=%08x bcr=%08x gp0_words=%u expected=11000201/00010020/0\n",chcr,bcr,s_gp0_n);
 return chcr!=0x11000201 || bcr!=0x10020 || s_gp0_n!=0;
}

int source_gpu_runtime_active(void){return 0;}
void source_gpu_runtime_dma_write(void){}
