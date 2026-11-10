/* Authored controller schedules and callbacks; no decode algorithm or media. */
#include <stdio.h>
#include <stdlib.h>
#include "source_mdec_fifo.h"
static uint32_t random_state=0x53c0ffeeu, callback_hash, table_hash;
static unsigned oversized;
static uint32_t draw(void){random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static unsigned block(void *context,uint32_t command,unsigned index,const uint16_t *encoded,unsigned count,uint32_t *pixels){
    (void)context;
    uint32_t h=command^index^count;
    for(unsigned i=0;i<count;i++)h=(h^encoded[i])*16777619u;
    callback_hash=(callback_hash^h)*16777619u;
    unsigned depth=(command>>27)&3u;
    unsigned n=index<2?0:depth==2?48:depth==3?32:depth==1?16:8;
    for(unsigned i=0;i<n;i++)pixels[i]=h+0x1020304u*i;
    return oversized?49:n;
}
static void table(void *context,unsigned op,unsigned index,uint32_t value){
    (void)context;table_hash=((table_hash^op)*16777619u^index)*16777619u^value;
}
static void word(uint32_t v){if(fwrite(&v,4,1,stdout)!=1)exit(2);}
static void emit(SourceMDEC *s,uint32_t result,uint32_t offset){
    word(result);word(offset);word(callback_hash);word(table_hash);
    word(source_mdec_status(s));word(source_mdec_can_write(s));word(source_mdec_can_read(s));
    word(s->command);word(s->control);word((uint32_t)s->credit);word(s->remaining);
    word(s->phase);word(s->busy);word(s->coefficient);word(s->encoded_count);word(s->block);
    word(s->pixel_count);word(s->pixel_at);word(s->quant_index);word(s->matrix_index);
    word(s->row);word(s->word_in_row);word(s->row_words);word(s->error);word(s->block_cycles);
    word(s->in_at);word(s->in_count);word(s->out_at);word(s->out_count);
    for(unsigned i=0;i<32;i++)word(i<s->in_count?s->in[(s->in_at+i)&31u]:0);
    for(unsigned i=0;i<32;i++)word(i<s->out_count?s->out[(s->out_at+i)&31u]:0);
    for(unsigned i=0;i<48;i++)word(i<s->pixel_count?s->pixels[i]:0);
    for(unsigned i=0;i<64;i++)word(i<s->encoded_count?s->encoded[i]:0);
}
static void tick(SourceMDEC *s,unsigned n){source_mdec_run(s,n);emit(s,0,0);}
static void write_input(SourceMDEC *s,uint32_t v,int dma){source_mdec_write(s,v,dma);emit(s,0,0);}
static void read_output(SourceMDEC *s,int dma){uint32_t offset=0xfeedu;uint32_t v=source_mdec_read(s,dma,&offset);emit(s,v,offset);}
static void control(SourceMDEC *s,uint32_t v){source_mdec_control(s,v);emit(s,0,0);}
int main(void){
#ifdef _WIN32
    /* Binary stdout is set by the launcher on Windows; acceptance runs Linux. */
#endif
    SourceMDEC s;
    const unsigned clocks[]={0,1,2,127,128,129,473,474,475,511,512,513,1000000};
    for(unsigned cost=474;cost<=512;cost+=38){
      for(unsigned depth=2;depth<=3;depth++){
        source_mdec_power(&s,block,table,NULL);s.block_cycles=cost;emit(&s,0,0);
        control(&s,0x60000000u);
        for(unsigned op=0;op<8;op++)if(op!=1){
            uint32_t command=(op<<29)|3u;write_input(&s,command,0);tick(&s,1);
            if(op==2||op==3)for(unsigned i=0;i<32;i++){write_input(&s,0x12340000u+i,0);tick(&s,0);}
            control(&s,0xe0000000u);
        }
        for(unsigned repetition=0;repetition<3;repetition++){
            write_input(&s,0x20000060u|(depth<<27),repetition&1u);tick(&s,1);tick(&s,1);
            for(unsigned i=0;i<96;i++)write_input(&s,((i&1u)?0xfe00fe00u:0xfe000000u+(i&1023u)),1);
            for(unsigned i=0;i<1024;i++){
                tick(&s,clocks[i%13]);
                read_output(&s,i&1u);
                if((i%7)==0)write_input(&s,0xfe00003fu,0);
            }
            control(&s,0xe0000000u);
        }
      }
    }
    for(unsigned epoch=0;epoch<24;epoch++){
      source_mdec_power(&s,block,table,NULL);control(&s,0x60000000u);
      for(unsigned i=0;i<3000;i++){
        uint32_t op=draw()%8u;
        uint32_t value=draw();
        unsigned elapsed=clocks[draw()%13u];
        switch(op){
          case 0:tick(&s,elapsed);break;
          case 1:write_input(&s,value,0);break;
          case 2:write_input(&s,value,1);break;
          case 3:read_output(&s,0);break;
          case 4:read_output(&s,1);break;
          case 5:control(&s,value);break;
          case 6:write_input(&s,0x38000040u,0);break;
          case 7:tick(&s,1000001);break;
        }
      }
    }
    source_mdec_power(&s,block,table,NULL);s.encoded_count=64;s.coefficient=1;
    int charge=source_mdec_half(&s,1);emit(&s,(uint32_t)charge,0);
    source_mdec_power(&s,block,table,NULL);oversized=1;
    charge=source_mdec_half(&s,1);emit(&s,(uint32_t)charge,0);
    charge=source_mdec_half(&s,0xfe00);emit(&s,(uint32_t)charge,0);
    return ferror(stdout)?2:0;
}
