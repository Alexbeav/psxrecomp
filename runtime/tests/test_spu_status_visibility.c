/* Authored register/sample boundary test. It does not assert a hardware reset
 * latch or sample phase; each case first settles one explicit sample. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/spu.c"
uint64_t s_frame_count;
static uint64_t test_clock;
uint64_t psx_get_cycle_count(void) { return test_clock; }
void audio_trace_pcm(int tap,const int16_t *stereo,int frames){(void)tap;(void)stereo;(void)frames;}
void audio_trace_event(uint16_t kind,uint32_t a,uint32_t b){(void)kind;(void)a;(void)b;}
void psx_irq_raise(uint32_t bit,uint32_t detail){(void)bit;(void)detail;}
uint32_t crc32_update(uint32_t crc,const uint8_t *data,size_t len){(void)data;(void)len;return crc;}
bool spu_shadow_enabled(void){return false;}
void spu_shadow_reset(void){}
void spu_shadow_process(int16_t *canon,int frames){(void)canon;(void)frames;}
static unsigned checks,failures;
static void expect(unsigned value,const char *why,unsigned mode){
 unsigned actual=spu_read(0x1F801DAEu)&63u;checks++;
 if(actual!=value){if(failures<8)fprintf(stderr,"mode%u %s: %u expected%u\n",mode,why,actual,value);failures++;}
}
static void sample(void){int16_t stereo[2];test_clock+=768u;spu_render(stereo,1);}
int main(void){
 for(unsigned mode=0;mode<64;mode++){
  test_clock=0;spu_init();sample();expect(0,"settled reset",mode);
  spu_write(0x1F801DAAu,0xC000u|mode);expect(0,"before next sample",mode);
  sample();expect(mode,"after sample",mode);
  spu_write(0x1F801DAAu,0xC000u);expect(mode,"clear before sample",mode);
  spu_write(0x1F801DAAu,0xC000u|(mode^63u));
  spu_write(0x1F801DAAu,0xC000u);expect(mode,"latest write still pending",mode);
  sample();expect(0,"latest value applied",mode);
 }
 /* No sample means no application. The normal disabled-SPU path must still
  * apply control; IRQ/capture/transfer-status bits have separate ownership. */
 test_clock=0;spu_init();sample();spu_write(0x1F801DAAu,0x10u);
 spu_render(NULL,1);expect(0,"null output does not advance",0x10);
 {int16_t stereo[2];spu_render(stereo,0);}
 expect(0,"zero samples do not advance",0x10);
 sample();expect(0x10,"disabled SPU applies control",0x10);
 /* The existing register-image wire must retain pending versus applied mode. */
 spu_write(0x1F801DAAu,0);
 uint32_t bytes=spu_snapshot_bytes();uint8_t *wire=(uint8_t*)malloc(bytes);
 if(!wire)return 2;
 spu_snapshot_write(wire);sample();expect(0,"post-save advance",0x10);
 checks++;if(!spu_snapshot_read(wire,bytes))failures++;
 expect(0x10,"restored applied mode",0x10);sample();expect(0,"restored pending mode applies",0x10);
 free(wire);
 printf("%u register/sample checks, %u failures\n",checks,failures);
 return failures?1:0;
}
