"""Exercise the actual host's F7 dispatch, without an SDL window or game."""
from pathlib import Path
import argparse,os,subprocess,sys,shutil
parser=argparse.ArgumentParser()
parser.add_argument('--compiler',required=True)
parser.add_argument('--sdl',type=Path,required=True)
parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'src/main.cpp')
parser.add_argument('--work',type=Path,required=True)
args=parser.parse_args()
runtime=Path(__file__).resolve().parents[1]
text=args.source.read_text(encoding='utf-8')
def function(name):
 start=text.find('static '+name+'(')
 if start<0:return ''
 first=text.index('{',start);end=first+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]+'\n'
code=r'''
#include "psx_sdl.h"
#include "host_keymap.h"
#include "replay_session.h"
#include <cstdio>
#include <cstring>
#define PAD_L1 (1<<10)
#define PAD_R1 (1<<11)
static int savestate_menu_replays,savestate_menu_slot,savestate_menu_delete_confirm=-1;
static int savestate_menu_renaming,savestate_menu_open=1,savestate_menu_key_direction;
static SDL_Keycode savestate_menu_open_key;
static char savestate_menu_name[INPUT_ROUTE_REPLAY_NAME_MAX+1];
static int plays,saves,exports,deletes,renames,syncs;
static void savestate_menu_sync_overlay(){++syncs;}
static void savestate_menu_text_input(int){}
static void savestate_menu_close(){savestate_menu_open=0;}
static int savestate_submit_slot(int,int){++saves;return 1;}
extern "C" void psx_savestate_menu_set_notice(const char*){}
extern "C" void psx_savestate_menu_note_slots_changed(){}
extern "C" int replay_session_play_slot(int){++plays;return 1;}
extern "C" int replay_session_slot_exists(int){return 1;}
extern "C" int replay_session_slot_info(int,char* name,size_t size,uint32_t*){std::snprintf(name,size,"fixture");return 1;}
extern "C" int replay_session_rename_slot(int,const char*){++renames;return 1;}
extern "C" int replay_session_export_slot(int,char*,size_t){++exports;return 1;}
extern "C" int replay_session_delete_slot(int){++deletes;return 1;}
extern "C" int host_keymap_match_event(HostKeymapAction,int,int,int){return 0;}
'''
for signature in ('void savestate_menu_change_page','void savestate_menu_delete_replay','void savestate_menu_export_replay',
                  'void savestate_menu_submit','int savestate_menu_slot_from_key','void savestate_menu_handle_key'):
 part=function(signature)
 if not part and signature in ('void savestate_menu_change_page','void savestate_menu_delete_replay','void savestate_menu_export_replay'):
  part='static '+signature+'() {}\n'
 assert part,signature
 code+=part
part=function('void savestate_menu_page_buttons')
code+=part if part else 'static void savestate_menu_page_buttons(uint16_t) {}\n'
code+=r'''
static int failures;
static void check(bool value,const char* name){if(!value){++failures;std::printf("FAIL: %s\n",name);}}
static void press(SDL_Keycode key){savestate_menu_handle_key(key,SDL_SCANCODE_UNKNOWN,0,0);}
int main(){
 press(SDLK_TAB);check(savestate_menu_replays==1,"Tab reaches Replays through actual host dispatch");
 press(SDLK_RETURN);check(plays==1 && saves==0 && !savestate_menu_open,"Replay page plays replay instead of loading state slot");
 savestate_menu_open=1;press(SDLK_e);check(exports==1,"Replay export through keyboard input");
 press(SDLK_DELETE);check(deletes==0,"Delete needs confirmation");press(SDLK_DELETE);check(deletes==1,"Second delete confirms");
 press(SDLK_F2);check(savestate_menu_renaming==1,"F2 begins rename");press(SDLK_RETURN);check(renames==1 && !savestate_menu_renaming,"Enter commits rename");
 press(SDLK_TAB);check(savestate_menu_replays==0,"Tab returns to States");press(SDLK_RETURN);check(saves==1,"States still use real state submission");
 savestate_menu_page_buttons(0xffffu & ~PAD_L1);check(savestate_menu_replays==1,"L1 changes page");
 savestate_menu_page_buttons(0xffffu & ~PAD_L1);check(savestate_menu_replays==1,"held L1 does not repeat");
 savestate_menu_page_buttons(0xffffu);savestate_menu_page_buttons(0xffffu & ~PAD_R1);check(!savestate_menu_replays,"R1 returns page");
 std::printf("host navigation failures=%d\n",failures);return failures?1:0;
}
'''
args.work.mkdir(parents=True,exist_ok=True)
cpp=args.work/'host_navigation.cpp';cpp.write_text(code,encoding='utf-8')
exe=args.work/'host_navigation.exe'
command=[args.compiler,'-std=c++17','-O0','-DPSX_SDL3=1','-DSDL_MAIN_HANDLED=1','-I'+str(runtime/'include'),
 '-I'+str(args.sdl/'include'),str(cpp),'-o',str(exe)]
result=subprocess.run(command,text=True,encoding='utf-8',errors='replace')
if result.returncode:raise SystemExit(result.returncode)
raise SystemExit(subprocess.run([str(exe)],text=True,encoding='utf-8',errors='replace').returncode)
