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
    /* PS1B-425-STUB: spec rows for can_write. */
    fprintf(stderr,"PS1B-425 stub reached: can_write\n");abort();
    return 0;
}
static inline int source_mdec_can_read(const SourceMDEC *s){
    /* PS1B-425-STUB: spec rows for can_read. */
    fprintf(stderr,"PS1B-425 stub reached: can_read\n");abort();
    return 0;
}
static inline uint32_t source_mdec_status(const SourceMDEC *s){
    /* PS1B-425-STUB: spec rows for status. */
    fprintf(stderr,"PS1B-425 stub reached: status\n");abort();
    return 0;
}
static inline uint32_t source_mdec_pop_input(SourceMDEC *s){
    /* PS1B-425-STUB: spec rows for pop_input. */
    fprintf(stderr,"PS1B-425 stub reached: pop_input\n");abort();
    return 0;
}
static inline int source_mdec_half(SourceMDEC *s,uint16_t value){
    /* PS1B-425-STUB: spec rows for half. */
    fprintf(stderr,"PS1B-425 stub reached: half\n");abort();
    return (int)s->block_cycles;
}
static inline void source_mdec_run(SourceMDEC *s,unsigned clocks){
    /* PS1B-425-STUB: spec rows for run. */
    fprintf(stderr,"PS1B-425 stub reached: run\n");abort();
}
static inline void source_mdec_control(SourceMDEC *s,uint32_t value){
    /* PS1B-425-STUB: spec rows for control. */
    fprintf(stderr,"PS1B-425 stub reached: control\n");abort();
}
static inline void source_mdec_write(SourceMDEC *s,uint32_t value,int dma){
    /* PS1B-425-STUB: spec rows for write. */
    fprintf(stderr,"PS1B-425 stub reached: write\n");abort();
}
static inline uint32_t source_mdec_read(SourceMDEC *s,int dma,uint32_t *offset){
    /* PS1B-425-STUB: spec rows for read. */
    fprintf(stderr,"PS1B-425 stub reached: read\n");abort();
    return 0;
}
#endif
