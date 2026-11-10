#include "tas_disc_relocation.h"
#include <cstdio>
#include <chrono>
using namespace psx_tas_disc_relocation;
int main() {
 int failures=0,checks=0;
 const auto root=std::filesystem::temp_directory_path()/("psx roster "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directories(root);
 const Path file=root/"mapping.tsv", old1=root/"old one.cue", old2=root/"old two.cue", one=root/"new one.cue", two=root/"new two.cue";
 const std::string game(64,'a'), route(64,'b'), h1(64,'c'), h2(64,'d');
 auto row=[](int n,const std::string& h,const Path& before,const Path& after){return std::to_string(n)+"\t"+h+"\t"+before.u8string()+"\t"+after.u8string()+"\n";};
 const std::string header="PSX-ROSTER-RELOCATION-1\n"+game+"\n"+route+"\n2\n";
 const std::string good=header+row(1,h1,old1,one)+row(2,h2,old2,two);
 const auto digest=[&](const Path& p){return p==one?h1:p==two?h2:std::string();};
 auto check=[&](const std::string& name,const std::string& data,bool expected,const Path& boot){
  std::ofstream(file,std::ios::binary)<<data;
  std::vector<Path> output={"unchanged"};
  const bool ok=load(file,game,route,{old1,old2},boot,digest,output);
  ++checks;if(ok!=expected || (ok?output!=std::vector<Path>{one,two}:output!=std::vector<Path>{"unchanged"})){std::fprintf(stderr,"FAIL %s\n",name.c_str());++failures;}
 };
 check("valid spaces",good,true,one);
 check("wrong boot",good,false,two);
 check("wrong header","x"+good,false,one);
 auto changed=good;changed[24]='e';check("wrong game identity",changed,false,one);
 check("wrong ordinal",header+row(2,h1,old1,one)+row(1,h2,old2,two),false,one);
 check("changed content",header+row(1,h2,old1,one)+row(2,h2,old2,two),false,one);
 check("wrong original",header+row(1,h1,old2,one)+row(2,h2,old2,two),false,one);
 check("duplicate mapped",header+row(1,h1,old1,one)+row(2,h1,old2,one),false,one);
 check("relative mapped",header+row(1,h1,old1,"relative.cue")+row(2,h2,old2,two),false,one);
 check("reordered",header+row(1,h2,old1,two)+row(2,h1,old2,one),false,one);
 check("extra row",good+"extra\n",false,one);
 check("missing row",header+row(1,h1,old1,one),false,one);
 changed=good;changed.insert(changed.find('\n',header.size()),"\t");check("trailing column",changed,false,one);
 changed=good;changed.insert(header.size()+4,1,'\0');check("nul path",changed,false,one);
 check("oversize",std::string(70000,'x'),false,one);
 std::filesystem::remove_all(root);
 std::printf("roster relocation checks=%d failures=%d\n",checks,failures);return failures?1:0;
}