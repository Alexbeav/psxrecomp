#ifndef PSX_SOURCE_MDEC_FIFO_H
#define PSX_SOURCE_MDEC_FIFO_H
#include <stdint.h>
#include <string.h>

/* Optional Octoshock source-compatibility model, independently expressed.
 * This is a FIFO/clock controller, not an IDCT implementation or a hardware
 * timing claim. The owner supplies table writes and one completed RLE block.
 * Feed elapsed clocks only at qualified source DMA/MDEC service boundaries. */
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
} SourceMDEC;
static inline void source_mdec_power(SourceMDEC *s,SourceMDECBlock decode,SourceMDECTable table,void *context){
    memset(s,0,sizeof(*s));s->decode=decode;s->table=table;s->context=context;
}
static inline int source_mdec_can_write(const SourceMDEC *s){
    return s->in_count==0 && (s->control&(1u<<30)) && s->busy && s->remaining!=65535;
}
static inline int source_mdec_can_read(const SourceMDEC *s){
    return s->out_count==32 && (s->control&(1u<<29));
}
static inline uint32_t source_mdec_status(const SourceMDEC *s){
    return (!s->out_count?1u<<31:0)|(s->in_count==32?1u<<30:0)|
      (s->busy?1u<<29:0)|(source_mdec_can_write(s)?1u<<28:0)|
      (source_mdec_can_read(s)?1u<<27:0)|(((s->command>>25)&15u)<<23)|s->remaining;
}
static inline uint32_t source_mdec_pop_input(SourceMDEC *s){
    uint32_t v=s->in[s->in_at];s->in_at=(s->in_at+1)&31u;s->in_count--;return v;
}
static inline int source_mdec_half(SourceMDEC *s,uint16_t value){
    if(!s->coefficient && value==0xfe00)return 0;
    if(s->encoded_count>=64){s->error=1;return 0;}
    s->encoded[s->encoded_count++]=value;
    if(!s->coefficient)s->coefficient=1;
    else if(value==0xfe00)s->coefficient=64;
    else {s->coefficient+=1u+(value>>10);if(s->coefficient>64)s->coefficient=64;}
    if(s->coefficient<64)return 0;
    s->coefficient=0;
    unsigned words=s->decode(s->context,s->command,s->block,s->encoded,s->encoded_count,s->pixels);
    if(words>48){s->error=1;return 0;}
    if(s->block>=2)s->pixel_count=words;
    s->encoded_count=0;s->block++;
    if(s->block==((s->command&(1u<<28))?6u:3u))s->block=(s->command&(1u<<28))?0u:2u;
    return 474;
}
static inline void source_mdec_run(SourceMDEC *s,unsigned clocks){
    if(s->error)return;
    if(clocks>1000000){s->error=1;return;}
    s->credit+=(int32_t)clocks;if(s->credit>128)s->credit=128;
    for(;;){
        if(s->phase==SMDEC_IDLE){
            s->busy=0;
            if(!s->in_count)return;
            s->command=source_mdec_pop_input(s);s->busy=1;s->credit--;
            s->phase=SMDEC_HEADER_WAIT;
        }
        if(s->phase==SMDEC_HEADER_WAIT){
            if(s->credit<=0)return;
            unsigned op=s->command>>29;
            if(op==1){
                s->remaining=(uint16_t)s->command;s->out_at=s->out_count=0;
                s->pixel_count=s->coefficient=s->encoded_count=0;
                s->block=(s->command&(1u<<28))?0u:2u;
                unsigned depth=(s->command>>27)&3u;
                s->row_words=(uint8_t)(depth==2?6:depth==3?4:0);
                s->row=0;s->word_in_row=s->row_words;
            } else if(op==2){s->quant_index=0;s->remaining=(s->command&1u)?32:16;}
            else if(op==3){s->matrix_index=0;s->remaining=32;}
            else {s->remaining=(uint16_t)s->command;s->phase=SMDEC_IDLE;continue;}
            s->remaining--;s->phase=SMDEC_INPUT;
        }
        if(s->phase==SMDEC_INPUT){
            if(!s->in_count)return;
            uint32_t word=source_mdec_pop_input(s);s->remaining--;
            unsigned op=s->command>>29;
            if(op==1){
                s->pixel_count=0;
                int cost=source_mdec_half(s,(uint16_t)word);
                cost+=source_mdec_half(s,(uint16_t)(word>>16));
                if(s->error)return;
                s->credit-=cost;s->phase=SMDEC_BLOCK_WAIT;
            } else {
                unsigned *index=op==2?&s->quant_index:&s->matrix_index;
                s->table(s->context,op,*index,word);
                *index=(*index+(op==2?4:2))&(op==2?127u:63u);
                if(s->remaining==65535)s->phase=SMDEC_IDLE;
                continue;
            }
        }
        if(s->phase==SMDEC_BLOCK_WAIT){
            if(s->credit<=0)return;
            s->pixel_at=0;s->phase=SMDEC_OUTPUT;
        }
        if(s->phase==SMDEC_OUTPUT){
            while(s->pixel_at<s->pixel_count){
                if(s->out_count==32)return;
                s->out[(s->out_at+s->out_count)&31u]=s->pixels[s->pixel_at++];s->out_count++;
            }
            s->phase=s->remaining==65535?SMDEC_IDLE:SMDEC_INPUT;
        }
    }
}
static inline void source_mdec_control(SourceMDEC *s,uint32_t value){
    if(value&(1u<<31)){
        s->phase=SMDEC_IDLE;s->remaining=0;s->command=s->busy=0;
        s->pixel_count=0;s->credit=0;s->quant_index=s->matrix_index=0;
        s->coefficient=s->encoded_count=s->block=0;
        s->in_at=s->out_at=s->in_count=s->out_count=0;s->error=0;
    }
    s->control=value&0x7fffffffu;
}
static inline void source_mdec_write(SourceMDEC *s,uint32_t value,int dma){
    if(s->in_count==32){s->error=1;return;}
    s->in[(s->in_at+s->in_count)&31u]=value;s->in_count++;
    if(!dma && !s->busy && s->credit<1)s->credit=1;
    source_mdec_run(s,0);
}
static inline uint32_t source_mdec_read(SourceMDEC *s,int dma,uint32_t *offset){
    if(offset)*offset=0;
    if(!s->out_count)return 0;
    uint32_t value=s->out[s->out_at];s->out_at=(s->out_at+1)&31u;s->out_count--;
    if(dma){
        uint32_t delta=(s->row&7u)*s->row_words;
        if(s->row&8u)delta-=7u*s->row_words;
        if(offset)*offset=delta;
        s->word_in_row--;
        if(!s->word_in_row){s->word_in_row=s->row_words;s->row++;}
        source_mdec_run(s,0);
    }
    return value;
}
#endif
