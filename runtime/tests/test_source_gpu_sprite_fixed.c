/* Fixed-size sprite family (GP0 0x68-0x7F) in the source command projection.
 *
 * The source core builds every 0x60-0x7F entry from one macro, SPR_HELPER(cv):
 *
 *     len         = 2 + ((cv & 0x4) >> 2) + ((cv & 0x18) ? 0 : 1)
 *     fifo_fb_len = 2 | ((cv & 0x4) >> 2) | ((cv & 0x18) ? 0 : 1)   (| , not +)
 *
 * so bit 2 adds a texture-coordinate word and size class 0 adds a width/height
 * word, while classes 1/2/3 are the fixed 1x1, 8x8 and 16x16 forms that carry
 * no size word. This fixture pins both formulas across the whole range, checks
 * that the previously admitted variable-size opcodes are unchanged, and checks
 * that the fixed classes take their extent from the opcode rather than reading
 * a word that is not in the packet. Source compatibility checks, not a PS1
 * hardware timing claim. */
#include "source_gpu_command_projection.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
static void check(int okay,const char *message) {
    checks++;
    if(!okay){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
}

/* The source macro, written out independently of the implementation. */
static unsigned macro_len(unsigned cv){return 2u+((cv&0x4u)>>2)+((cv&0x18u)?0u:1u);}
static unsigned macro_fifo(unsigned cv){return 2u|((cv&0x4u)>>2)|((cv&0x18u)?0u:1u);}

/* Cost of one sprite drawn at (4,4) into a wide clip, with no blend or mask
 * work: the flat 16 setup clocks plus one clock per pixel of the extent. */
static int expect_cost(unsigned w,unsigned h){return 16+(int)(w*h);}

int main(void) {
    /* 1. Both formulas hold across the entire rectangle range. */
    for(unsigned cv=0x60;cv<=0x7f;cv++) {
        uint32_t word=cv<<24;
        check(source_gpu_block_supported(cv),"every rectangle opcode is admitted");
        check(source_gpu_command_length(word)==macro_len(cv),"packet length matches SPR_HELPER len");
        check(source_gpu_command_feedback_length(word)==macro_fifo(cv),"feedback matches SPR_HELPER fifo_fb_len");
    }

    /* 2. Regression: the variable-size opcodes keep the lengths and the
     *    three-word feedback they were qualified with. */
    for(unsigned cv=0x60;cv<=0x67;cv++) {
        uint32_t word=cv<<24;
        check(source_gpu_command_length(word)==((cv&4u)?4u:3u),"variable rectangle length unchanged");
        check(source_gpu_command_feedback_length(word)==3u,"variable rectangle feedback unchanged");
    }

    /* 3. Fixed classes: 2 words flat, 3 textured, and feedback 2 vs 3. */
    const unsigned fixed[3]={0x68,0x70,0x78};
    const unsigned extent[3]={1,8,16};
    for(unsigned k=0;k<3;k++) {
        for(unsigned low=0;low<4;low++) {
            unsigned flat=fixed[k]+low,textured=fixed[k]+4+low;
            check(source_gpu_command_length(flat<<24)==2u,"fixed flat sprite is a two-word packet");
            check(source_gpu_command_length(textured<<24)==3u,"fixed textured sprite is a three-word packet");
            check(source_gpu_command_feedback_length(flat<<24)==2u,"fixed flat sprite feeds back two words");
            check(source_gpu_command_feedback_length(textured<<24)==3u,"fixed textured sprite feeds back three words");
            check(source_gpu_sprite_class(flat)==k+1 && source_gpu_sprite_class(textured)==k+1,"size class from bits 3-4");
        }
        /* The extent comes from the opcode; no size word exists to read. */
        uint32_t flat_words[2]={(uint32_t)(fixed[k])<<24,0x00040004u};
        SourceGPUCommandProjection s;source_gpu_command_cold(&s);
        s.clip_x1=1023;s.clip_y1=511;
        check(source_gpu_command_block_cost(&s,flat_words)==expect_cost(extent[k],extent[k]),
              "fixed sprite cost uses the implied extent");
    }

    /* 4. The variable-size extent still comes from its width/height word, at
     *    word 2 when flat and word 3 when textured. */
    {
        SourceGPUCommandProjection s;source_gpu_command_cold(&s);
        s.clip_x1=1023;s.clip_y1=511;
        uint32_t flat[3]={0x60u<<24,0x00040004u,(3u<<16)|5u};
        uint32_t textured[4]={0x64u<<24,0x00040004u,300u<<22,(3u<<16)|5u};
        check(source_gpu_command_block_cost(&s,flat)==expect_cost(5,3),"flat variable extent from word 2");
        check(source_gpu_command_block_cost(&s,textured)==expect_cost(5,3),"textured variable extent from word 3");
    }

    /* 5. End to end on 0x7D, the 16x16 raw textured sprite that stopped the
     *    Mega Man X5 route at return 1589: three words, dispatched once, and
     *    charged two setup clocks plus the 16x16 raster walk. */
    {
        const uint32_t words[3]={(0x7Du<<24)|0x808080u,0x00040004u,300u<<22};
        SourceGPUCommandProjection s;source_gpu_command_cold(&s);
        s.clip_x1=1023;s.clip_y1=511;s.budget=1000;
        check(source_gpu_command_write(&s,words[0]),"0x7D first word admitted");
        check(s.dispatch.kind==SOURCE_GPU_DISPATCH_NONE && s.budget==1000,"no partial 0x7D dispatch or charge");
        check(source_gpu_command_write(&s,words[1]),"0x7D second word admitted");
        /* Three-word feedback against a three-word packet: the count never
         * reaches the threshold inside one packet, so DMA stays ready
         * throughout and the feedback only bites once packets queue up. */
        check(source_gpu_command_ready(&s)==1,"0x7D stays DMA-ready inside its own packet");
        check(source_gpu_command_write(&s,words[2]),"complete 0x7D packet dispatches");
        check(s.dispatch.kind==SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.count==3 && !s.count,
              "0x7D dispatches once as three words");
        check(s.budget==1000-2-expect_cost(16,16),"0x7D charges two setup clocks plus the 16x16 walk");
    }

    printf("source GPU fixed-size sprite family: %u checks passed\n",checks);
    return 0;
}
