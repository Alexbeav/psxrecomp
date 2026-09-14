#include "iso_reader.h"
#include <cstdio>
#include <fstream>
int main(int argc,char **argv) {
 if(argc!=4)return 1;
 PS1::ISOReader disc;if(!disc.Open(argv[1]))return 2;
 std::ifstream inputs(argv[3]);FILE *out=fopen(argv[2],"wb");if(!out)return 3;
 fprintf(out,"lba\tq\n");unsigned lba;
 while(inputs>>lba) {
  unsigned char q[12];bool valid=false;
  if(!disc.ReadSubChannelQ(lba,q,&valid) || !valid)return 4;
  fprintf(out,"%u\t",lba);for(auto b:q)fprintf(out,"%02X",b);fprintf(out,"\n");
 }
 return fclose(out)!=0;
}
