#ifndef PSX_SOURCE_MDEC_FIFO_H
#define PSX_SOURCE_MDEC_FIFO_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Optional source comparison controller; no hardware timing claim.
 * PS1B-425: the behaviour specification is in the sandbox spec folder. */
typedef unsigned (*SourceMDECBlock)(void*,uint32_t,unsigned,const uint16_t*,unsigned,uint32_t*);
typedef void (*SourceMDECTable)(void*,unsigned,unsigned,uint32_t);
enum { SMDEC_IDLE,SMDEC_HEADER_WAIT,SMDEC_INPUT,SMDEC_BLOCK_WAIT,SMDEC_OUTPUT };
typedef struct SourceMDEC {
    uint32_t in[32],out[32],pixels[48];
    unsigned in_at,in_count,out_at,out_count;
    uint32_t command,control;
    int32_t credit;
    uint16_t remaining,encoded[64];
    unsigned phase,busy,coefficient,encoded_count,block,pixel_count,pixel_at;
    unsigned quant_index,matrix_index;
    uint8_t row,word_in_row,row_words;
    SourceMDECBlock decode; SourceMDECTable table; void *context;
    int error;
    unsigned block_cycles; /* immutable model configuration */
} SourceMDEC;
static inline void source_mdec_power(SourceMDEC *s,SourceMDECBlock decode,SourceMDECTable table,void *context){
    memset(s,0,sizeof(*s));s->block_cycles=474;s->decode=decode;s->table=table;s->context=context;
}
static inline int source_mdec_can_write(const SourceMDEC *s){
    /* Spec S2: the input request needs control bit 30, a busy controller, an
     * empty input queue and a word count that has not run out (FFFFh). */
    return (s->control>>30&1u) && s->busy && s->in_count==0 && s->remaining!=0xFFFFu;
}
static inline int source_mdec_can_read(const SourceMDEC *s){
    /* Spec S2: the output request needs control bit 29 and a full output
     * queue of 32 words. */
    return (s->control>>29&1u) && s->out_count==32;
}
static inline uint32_t source_mdec_status(const SourceMDEC *s){
    /* Spec S1 (PSX-SPX "1F801824h - MDEC1 - MDEC Status Register"): output
     * queue empty, input queue full, busy, the two requests, command bits
     * 28-25, and the remaining word count. Bits 22-16 are zero in this
     * profile. */
    uint32_t status=s->remaining;
    if(s->out_count==0)status|=1u<<31;
    if(s->in_count==32)status|=1u<<30;
    if(s->busy)status|=1u<<29;
    if(source_mdec_can_write(s))status|=1u<<28;
    if(source_mdec_can_read(s))status|=1u<<27;
    status|=(s->command>>25&15u)<<23;
    return status;
}
static inline uint32_t source_mdec_pop_input(SourceMDEC *s){
    /* Spec section 2: the oldest queued input word. The caller makes sure
     * that the queue is not empty. Indices are modulo 32. */
    uint32_t value=s->in[s->in_at&31u];
    s->in_at=(s->in_at+1u)&31u;
    s->in_count--;
    return value;
}
static inline int source_mdec_half(SourceMDEC *s,uint16_t value){
    /* Spec B2 (PSX-SPX "MDEC Data Format", "Dummy halfwords"): FE00h before
     * a block has begun is padding. */
    if(value==0xFE00u && s->coefficient==0)return 0;
    /* Spec B3: a block holds at most 64 halfwords. */
    if(s->encoded_count>=64){s->error=1;return 0;}
    s->encoded[s->encoded_count++]=value;
    /* Spec B2: the first halfword begins the block. A later one that is not
     * FE00h moves the coefficient on by one plus its high six bits (PSX-SPX
     * "RLE (Run length data, for 2nd through 64th value)"). At the limit
     * of 64 the block is fully defined and ends without an end code
     * (PSX-SPX "EOB (End Of Block)": "EOB isn't required if the block was
     * already fully defined"; the b53_replay_driver hash holds this). */
    if(s->coefficient==0){s->coefficient=1;return 0;}
    if(value!=0xFE00u){
        s->coefficient+=1u+(value>>10);
        if(s->coefficient<64)return 0;
    }
    /* Spec B3: FE00h ends the block (PSX-SPX "EOB (End Of Block)"). The
     * owner decodes it; more than 48 output words is an error, and then the
     * block number and the encoded count stay. */
    s->coefficient=0;
    unsigned words=s->decode(s->context,s->command,s->block,s->encoded,s->encoded_count,s->pixels);
    if(words>48){s->error=1;return 0;}
    /* Spec B4: only blocks 2 and above give output words. A colour command
     * (bit 28) goes through blocks 0 to 5; otherwise block 2 repeats. */
    s->encoded_count=0;
    if(s->block>=2)s->pixel_count=words;
    s->block=(s->command>>28&1u)?(s->block+1u)%6u:2u;
    return (int)s->block_cycles;
}
static inline void source_mdec_run(SourceMDEC *s,unsigned clocks){
    /* Spec T1: an error stops all service; more than 1,000,000 clocks in one
     * call is an error. Elapsed clocks add to the credit, which is held at
     * 128 at most; a debt stays until later clocks repay it. */
    if(s->error)return;
    if(clocks>1000000u){s->error=1;return;}
    int64_t credit=(int64_t)s->credit+(int64_t)clocks;
    s->credit=credit>128?128:(int32_t)credit;
    for(;;){
        if(s->phase==SMDEC_IDLE){
            /* Spec T2, T6: an idle controller is not busy. The oldest input
             * word becomes the command, for one cycle. */
            s->busy=0;
            if(s->in_count==0)return;
            s->command=source_mdec_pop_input(s);
            s->busy=1;
            s->credit-=1;
            s->phase=SMDEC_HEADER_WAIT;
        }else if(s->phase==SMDEC_HEADER_WAIT){
            /* Spec T2: the header waits for credit above zero. */
            if(s->credit<=0)return;
            unsigned operation=s->command>>29;
            if(operation==1){
                /* Spec T3 (PSX-SPX "MDEC(1) - Decode Macroblock(s)"): the
                 * word count, an empty output queue, block 0 for a colour
                 * depth and block 2 otherwise, and the row length of the
                 * output-address adjustment (spec A1, A2). */
                unsigned depth=s->command>>27&3u;
                s->remaining=(uint16_t)s->command;
                s->out_at=s->out_count=0;
                s->pixel_count=s->coefficient=s->encoded_count=0;
                s->block=(s->command>>28&1u)?0u:2u;
                s->row_words=depth==2?6:depth==3?4:0;
                s->row=0;
                s->word_in_row=s->row_words;
            }else if(operation==2){
                /* Spec T4 (PSX-SPX "MDEC(2) - Set Quant Table(s)"). */
                s->quant_index=0;
                s->remaining=(s->command&1u)?32:16;
            }else if(operation==3){
                /* Spec T4 (PSX-SPX "MDEC(3) - Set Scale Table"). */
                s->matrix_index=0;
                s->remaining=32;
            }else{
                /* Spec T4: operations 0 and 4 to 7 have no data phase. */
                s->remaining=(uint16_t)s->command;
                s->phase=SMDEC_IDLE;
                continue;
            }
            /* Spec T5: the count goes down once when the header is done. */
            s->remaining--;
            s->phase=SMDEC_INPUT;
        }else if(s->phase==SMDEC_INPUT){
            /* Spec T5, T6: each data word takes the count down by one, and
             * costs no cycle of its own. */
            if(s->in_count==0)return;
            uint32_t word=source_mdec_pop_input(s);
            unsigned operation=s->command>>29;
            s->remaining--;
            if(operation==1){
                /* Spec B5: low halfword, then high halfword. The cost of
                 * the blocks they complete comes off the credit. An error
                 * stops before that. */
                s->pixel_count=0;
                int cost=source_mdec_half(s,(uint16_t)word);
                cost+=source_mdec_half(s,(uint16_t)(word>>16));
                if(s->error)return;
                s->credit-=cost;
                s->phase=SMDEC_BLOCK_WAIT;
            }else{
                /* Spec B1: the owner gets the table word as it is. A quant
                 * word moves the index by 4 modulo 128, a scale word by 2
                 * modulo 64. Spec T6: after the last word, idle. */
                if(operation==2){
                    s->table(s->context,2,s->quant_index,word);
                    s->quant_index=(s->quant_index+4u)&127u;
                }else{
                    s->table(s->context,3,s->matrix_index,word);
                    s->matrix_index=(s->matrix_index+2u)&63u;
                }
                if(s->remaining==0xFFFFu)s->phase=SMDEC_IDLE;
            }
        }else if(s->phase==SMDEC_BLOCK_WAIT){
            /* Spec B5: the output begins when the credit is above zero. */
            if(s->credit<=0)return;
            s->pixel_at=0;
            s->phase=SMDEC_OUTPUT;
        }else{
            /* Spec B6: output words go into the queue in order, as far as
             * it has room; a full queue keeps the rest. Spec T6: when all
             * are queued, the command goes on with its next word, or ends
             * after its last one. */
            while(s->pixel_at<s->pixel_count && s->out_count<32){
                s->out[(s->out_at+s->out_count)&31u]=s->pixels[s->pixel_at++];
                s->out_count++;
            }
            if(s->pixel_at<s->pixel_count)return;
            s->phase=s->remaining==0xFFFFu?SMDEC_IDLE:SMDEC_INPUT;
        }
    }
}
static inline void source_mdec_control(SourceMDEC *s,uint32_t value){
    /* Spec S5 (PSX-SPX "1F801824h - MDEC1 - MDEC Control/Reset Register"):
     * bits 30-0 are kept. Bit 31 resets the controller and both queues; the
     * arrays, the output-address state, the callbacks and block_cycles
     * stay. */
    s->control=value&0x7FFFFFFFu;
    if(value>>31){
        s->phase=SMDEC_IDLE;
        s->remaining=0;
        s->command=0;
        s->busy=s->pixel_count=0;
        s->credit=0;
        s->quant_index=s->matrix_index=0;
        s->coefficient=s->encoded_count=s->block=0;
        s->in_at=s->in_count=s->out_at=s->out_count=0;
        s->error=0;
    }
}
static inline void source_mdec_write(SourceMDEC *s,uint32_t value,int dma){
    /* Spec S3: a word for a full input queue is lost. Otherwise it is
     * queued; a CPU write to a controller that is not busy lifts the credit
     * to 1; then the controller is served with no elapsed clocks. */
    if(s->in_count>=32)return;
    s->in[(s->in_at+s->in_count)&31u]=value;
    s->in_count++;
    if(!dma && !s->busy && s->credit<1)s->credit=1;
    source_mdec_run(s,0);
}
static inline uint32_t source_mdec_read(SourceMDEC *s,int dma,uint32_t *offset){
    /* Spec S4: an empty queue gives zero. Otherwise the oldest word is
     * taken. Only a DMA read reports an address adjustment, moves the
     * output-address state and serves the controller. */
    uint32_t value=0,adjustment=0;
    if(s->out_count!=0){
        value=s->out[s->out_at&31u];
        s->out_at=(s->out_at+1u)&31u;
        s->out_count--;
        if(dma){
            /* Spec A1: rows 0 to 7 step by row_words; rows 8 to 15 are 7
             * times row_words lower. The result is modulo 2^32. */
            adjustment=(uint32_t)(s->row&7u)*s->row_words;
            if(s->row&8u)adjustment-=7u*s->row_words;
            /* Spec A2: one word of the row is used; after the last one the
             * next row begins. Both counters are 8 bits wide. */
            s->word_in_row--;
            if(s->word_in_row==0){s->word_in_row=s->row_words;s->row++;}
            source_mdec_run(s,0);
        }
    }
    if(offset)*offset=adjustment;
    return value;
}
#endif
