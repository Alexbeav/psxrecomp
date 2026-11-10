"""Exercise the production mode handoff across replay arming using real SIO polls."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

runtime = Path(__file__).resolve().parent.parent

def function(text, name):
    start = text.index('static ', text.index(name) - 20)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def run(argv, **kw):
    return subprocess.run(argv, text=True, encoding='utf-8', errors='replace', check=True, **kw)

source = Path(sys.argv[3]) if len(sys.argv) > 3 else runtime / 'src/main.cpp'
text = source.read_text(encoding='utf-8-sig')
functions = '\n'.join(function(text, name) for name in (
    'live_pad_mode(int s)', 'sampled_pad_mode(int s)',
    'apply_pad_slot_to_sio(int s,', 'apply_replay_p1_to_sio(void)'))
with tempfile.TemporaryDirectory(prefix='controller-recording-', dir=os.environ.get('TMP')) as tmp:
    work = Path(tmp)
    c = work / 'bus.c'
    c.write_text('#define main existing_pad_checks\n#include "' +
                 (runtime/'tests/test_psx_pad_apply.c').as_posix() + '"\n#undef main\n'
                 'void probe_reset(int analog) { fresh(); sio_reset_dualshock(0); '
                 'if(analog) sio_set_pad_type(0,1,128,128,128,128); }\n'
                 'int probe_id(void) { uint8_t b[9]; poll_bytes(b); return b[1]; }\n'
                 'int probe_word(void) { uint8_t b[9]; poll_bytes(b); return b[3]|(b[4]<<8); }\n', encoding='utf-8')
    cpp = work / 'probe.cpp'
    cpp.write_text(r'''
#include "sio.h"
#include "psx_netplay.h"
#include "replay_session.h"
#include <cstdio>
namespace PSXRecompV4 { enum {PAD_MODE_ANALOG=1,PAD_MODE_DIGITAL=2,PAD_MODE_NEGCON=3}; }
struct PlayerInput { bool steering_wheel=false; int mode=1,kind=2; };
PlayerInput g_players[PSX_MAX_PLAYERS];
struct Policy { void* callback=nullptr; } g_mod_controller_policy[PSX_MAX_PLAYERS];
int g_mod_controller_mode_override[PSX_MAX_PLAYERS]={-1,-1};
bool g_controller_mode_locked=false,g_controller_route_input=false;
extern "C" {
void probe_reset(int);
int probe_id(void);
int probe_word(void);
static ReplayState state=REPLAY_IDLE;
ReplayState replay_session_state(void) {return state;}
int mod_controller_source_present(unsigned) {return 0;}
int mod_pad_transform_initial_type(unsigned) {return -1;}
int psx_netplay_active(void) {return 0;}
int psx_selfcheck_input_locked(void) {return 0;}
int input_route_session_recording(void) {return 0;}
void psx_selfcheck_note_pad(int,unsigned short,unsigned char,unsigned char,unsigned char,unsigned char,unsigned char) {}
}
static unsigned short g_replay_p1_buttons=0xfff7;
static unsigned char g_replay_p1_sticks[4]={128,128,128,128};
''' + functions + r'''
int main() {
    unsigned failures=0;
    for (int analog=0;analog<2;++analog) {
        probe_reset(analog);
        const int expected=analog ? 0x73 : 0x41;
        for (ReplayState phase : {REPLAY_IDLE,REPLAY_ARMING,REPLAY_RECORDING,REPLAY_IDLE}) {
            state=phase;
            PsxNetPad pad={0xfff7,128,128,128,128,1,1};
            if (phase==REPLAY_RECORDING) apply_replay_p1_to_sio();
            else {
                const int mode=live_pad_mode(0) ? sampled_pad_mode(0) : g_players[0].mode;
                pad.analog=mode==PSXRecompV4::PAD_MODE_DIGITAL ? 0 : 1;
                apply_pad_slot_to_sio(0,pad);
            }
            const int got=probe_id();
            if(probe_word()!=0xfff7) ++failures;
            if(got!=expected) {
                std::fprintf(stderr,"initial=%02x replay_phase=%d expected=%02x got=%02x\n",expected,phase,expected,got);
                ++failures;
            }
        }
    }
    for(ReplayState phase : {REPLAY_LOADING,REPLAY_PLAYING}) {
        state=phase;
        if(live_pad_mode(0)) ++failures;
    }
    state=REPLAY_IDLE;
    g_controller_mode_locked=true;
    if(live_pad_mode(0)) ++failures;
    g_controller_mode_locked=false;
    g_controller_route_input=true;
    if(live_pad_mode(0)) ++failures;
    std::printf("recording mode: %u failures\n",failures);
    return failures ? 1 : 0;
}
''', encoding='utf-8')
    for opt in ('-O0','-O2'):
        objs=[]
        for name in (str(c), str(runtime/'src/sio.c'),str(runtime/'src/psx_pad_apply.c'),str(runtime/'src/psx_net_pad.c')):
            obj=work/(Path(name).stem+'.o')
            run([sys.argv[1],opt,'-DPSX_NO_DEBUG_TOOLS=1','-DPSX_MAX_PLAYERS=2',
                 '-I'+str(runtime/'include'),'-I'+str(runtime/'src'),'-c',name,'-o',str(obj)])
            objs.append(str(obj))
        exe=work/'controller-recording.exe'
        run([sys.argv[2],opt,'-std=c++17','-include','initializer_list','-DPSX_NO_DEBUG_TOOLS=1',
             '-DPSX_MAX_PLAYERS=2','-I'+str(runtime/'include'),str(cpp),*objs,'-o',str(exe)])
        run([str(exe)])
        print('controller recording mode '+opt+': PASS',flush=True)
