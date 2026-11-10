"""Execute the actual desktop refresh body with authored host/device seams."""
from pathlib import Path
import subprocess
import sys

compiler, source, scratch = sys.argv[1:]
source=Path(source); scratch=Path(scratch); scratch.mkdir(parents=True,exist_ok=True)
text=(source/'runtime/src/main.cpp').read_text(encoding='utf-8')
start=text.index('static void refresh_player_devices(void) {')
brace=text.index('{',start); depth=1; end=brace+1
while depth:
    depth += (text[end]=='{')-(text[end]=='}'); end+=1
body=text[start:end]
prefix=r'''
#include <cstdio>
#include <cstring>
#define PSX_MAX_PLAYERS 2
using SDL_JoystickID=int;
using SDL_Joystick=int;
struct PlayerInput { int kind=2,mode=0; bool steering_wheel=false; void* handle=nullptr; char guid[40]={}; int instance=0; };
PlayerInput g_players[2];
namespace PSXRecompV4 {enum {PAD_MODE_NEGCON=2};}
enum {REPLAY_IDLE=0,SIO_PAD_JOGCON=3,SIO_PAD_NEGCON=2,SIO_PAD_DUALSHOCK=1};
int route=0,netplay=0,replay=0,writes=0,opens=0;
void psx_local_mouse_reset() {}
int psx_netplay_active(){return netplay;}
int replay_session_state(){return replay;}
int input_route_session_preserves_ports_on_refresh(){return route;}
int SDL_GameControllerGetAttached(void*){return 1;}
SDL_Joystick* SDL_GameControllerGetJoystick(void*){return nullptr;}
int SDL_JoystickGetGUID(SDL_Joystick*){return 0;}
void SDL_JoystickGetGUIDString(int,char* p,int){*p=0;}
int psx_controller_handle_needs_close(bool,bool,bool,const char*,const char*){return 0;}
void close_player(PlayerInput&){}
void open_player(PlayerInput&,int){++opens;}
bool live_pad_mode(int){return false;}
int assert_sio_pad_profile(int,bool){++writes;return 0;}
void sio_reset_dualshock(int){++writes;}
int pad_type_boot_for_slot(int,const PlayerInput&,int){return SIO_PAD_DUALSHOCK;}
void sio_set_pad_config_capable(int,int){++writes;}
void sio_set_pad_type(int,int,int,int,int,int){++writes;}
'''
suffix=r'''
int main(){
    route=1;refresh_player_devices();
    if(writes || opens!=2){std::printf("FAIL admitted route: sio_writes=%d host_opens=%d\n",writes,opens);return 1;}
    route=0;writes=opens=0;refresh_player_devices();
    if(!writes || opens!=2){std::puts("FAIL unowned refresh");return 1;}
    netplay=1;writes=opens=0;refresh_player_devices();
    if(writes || opens!=2){std::puts("FAIL netplay refresh");return 1;}
    netplay=0;replay=1;writes=opens=0;refresh_player_devices();
    if(writes || opens!=2){std::puts("FAIL replay refresh");return 1;}
    std::puts("PASS production desktop refresh: admitted route preserves SIO, host handles refresh, unowned/netplay/replay controls pass");
}
'''
probe=scratch/'desktop_refresh.cpp';probe.write_text(prefix+body+suffix,encoding='utf-8')
exe=scratch/'desktop_refresh.exe'
subprocess.run([compiler,'-std=c++17','-O2',str(probe),'-o',str(exe)],check=True)
result=subprocess.run([str(exe)],capture_output=True,text=True,encoding='utf-8',errors='replace')
print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
raise SystemExit(result.returncode)
