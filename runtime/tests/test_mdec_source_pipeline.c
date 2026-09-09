/* Production MDEC API with read-only inspection of its private FIFO state. */
#include <stdio.h>
#include <stdlib.h>
#include "mdec.c"
uint64_t s_frame_count,psx_cycle_count;
int debug_server_fmv_quiet(void){return 0;}
int main(int argc,char **argv){
 if(argc!=3)return 2;FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wbx");if(!in || !out)return 2;
#ifdef _WIN32
 if(_putenv_s("PSX_MDEC_SOURCE_MODEL","octoshock-2.3"))abort();
#else
 if(setenv("PSX_MDEC_SOURCE_MODEL","octoshock-2.3",1))abort();
#endif
 uint32_t op,value;
 while(fread(&op,4,1,in)==1){
  if(fread(&value,4,1,in)!=1)abort();uint32_t result=0,aux=0;
  switch(op){
   case 0:mdec_init();break;
   case 1:mdec_source_advance(value);break;
   case 2:mdec_write(0x1f801820,value);break;
   case 3:mdec_write(0x1f801824,value);break;
   case 4:result=mdec_source_dma_read(&aux);break;
   case 5:mdec_dma_write_word(value);break;
   case 6:result=mdec_read(0x1f801820);break;
   default:abort();
  }
  SourceMDEC *s=&source_mdec;
  uint32_t row[]={result,aux,mdec_read(0x1f801824),(uint32_t)s->credit,s->command,s->control,s->remaining,s->in_count,s->out_count,
                 s->coefficient,s->block,s->pixel_count,s->pixel_at,s->row,s->word_in_row,s->row_words,s->busy,s->quant_index,s->matrix_index};
  if(fwrite(row,sizeof(row),1,out)!=1)abort();
 }
 if(ferror(in) || fclose(in) || fclose(out))return 4;return 0;
}
