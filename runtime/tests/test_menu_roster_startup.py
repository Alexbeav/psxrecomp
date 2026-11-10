"""The real startup roster block must publish a single entry to the menu."""
from pathlib import Path
import argparse,subprocess
parser=argparse.ArgumentParser()
parser.add_argument('--compiler',required=True)
parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'src/main.cpp')
parser.add_argument('--work',type=Path,required=True)
args=parser.parse_args()
text=args.source.read_text(encoding='utf-8')
start=text.index('    if (',text.index('/* Register the roster so the mounted image'))
opening=text.index('{',start);end=opening+1;depth=1
while depth:
 depth+=(text[end]=='{')-(text[end]=='}');end+=1
block=text[start:end]
code=r'''
#include <filesystem>
#include <vector>
#include <string>
#include <cstdio>
static int published;
static std::filesystem::path normalize_disc_path_for_launch(const std::filesystem::path& path){return path;}
static int roster_index_for_disc(const std::vector<std::filesystem::path>&,const std::string&){return 0;}
static void cdrom_disc_roster_set(const char* const* paths,int count,int mounted){
 published=count;
 for(int i=0;i<count;++i)if(std::string(paths[i])!="disc"+std::to_string(i+1)+".chd")published=-1;
 if(count && mounted!=1)published=-1;
}
static int check(int count){
 std::vector<std::filesystem::path> game_discs;
 for(int i=0;i<count;++i)game_discs.push_back("disc"+std::to_string(i+1)+".chd");
 std::string disc_path_str="disc1.chd";int selected_disc_index=1;published=0;
''' + block + r'''
 if(published!=count){std::printf("FAIL: startup roster=%d published=%d\n",count,published);return 1;}return 0;
}
int main(){int failed=check(0)+check(1)+check(2)+check(4);std::printf("menu roster startup failures=%d\n",failed);return failed?1:0;}
'''
args.work.mkdir(parents=True,exist_ok=True)
cpp=args.work/'roster.cpp';cpp.write_text(code,encoding='utf-8')
exe=args.work/'roster.exe'
result=subprocess.run([args.compiler,'-std=c++17','-O2',str(cpp),'-o',str(exe)],text=True,encoding='utf-8',errors='replace')
if result.returncode:raise SystemExit(result.returncode)
raise SystemExit(subprocess.run([str(exe)],text=True,encoding='utf-8',errors='replace').returncode)
