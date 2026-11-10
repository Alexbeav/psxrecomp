"""Exercise the actual debug route handlers/getters without SDL or a game.

Extract the unchanged function bodies from debug_server.c; stub only JSON and
responses. Live game qualification must additionally check native pad input.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    found = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^\n]*\)\n\{.*?^\}",
                      source, re.M | re.S)
    if not found:
        raise ValueError("Missing source function: " + name)
    return found[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[1] / 'src/debug_server.c').read_text()
    runtime = Path(__file__).resolve().parents[1]
    state = source[source.index('static PSX_BSS InputRouteStep s_input_route['):
                   source.index('static uint64_t s_input_route_start_frame = 0;')]
    declarations = '\n'.join(line for line in state.splitlines()
        if re.match(r'(?:#define INPUT_ROUTE_MAX_STEPS|static .*\bs_input_route_(?:count|index|remaining|active|file_mode|consumed|delivery_pending|port|wait_load)\b)', line))
    declarations = '#undef INPUT_ROUTE_MAX_STEPS\n' + declarations
    declarations += 'static int s_two_port_mode; static uint16_t s_route_port2=0xffff;\n'
    declarations += '\nstatic InputRouteStep s_input_route[INPUT_ROUTE_STORAGE_STEPS];\n'
    declarations += 'static void input_route_loaded(void);\n'
    preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "input_route_file.h"
#include "input_dualshock_route_file.h"
#define PSX_BSS
static uint64_t s_frame_count;
static InputDualShockRouteStep s_dualshock_route[INPUT_ROUTE_STORAGE_STEPS];
static uint16_t s_input_route_p2[INPUT_ROUTE_STORAGE_STEPS];
static uint8_t s_dualshock_axes[4],s_dualshock_mode_button;
static int s_dualshock_route_mode,s_update_enabled;
static int pending,p2_pending,expect_dual,applied,p2_applied,terminal;
static uint16_t pending_word,p2_word,held[2];
uint16_t sio_get_pad_buttons_slot(int slot){return held[slot];}
int sio_get_pad_connected(int slot){(void)slot;return 1;}
int sio_get_pad_analog(int slot){(void)slot;return 0;}
int sio_get_pad_config_capable(int slot){(void)slot;return 1;}
void sio_get_pad_sticks(int slot,uint8_t *sticks){(void)slot;memset(sticks,128,4);}
int input_route_dualshock_ports_match(void){return 1;}
void update_boundary(void){}
void input_instruction_histogram_boundary(uint64_t frame){(void)frame;}
void input_route_observer_boundary(uint32_t consumed,uint64_t frame){(void)consumed;(void)frame;}
int input_route_observer_terminal_frame(void){return terminal;}
void input_route_observer_source_frame(uint32_t frame){assert(frame>0);}
void input_route_observer_resume(uint32_t consumed){(void)consumed;pending=p2_pending=0;}
void input_route_observer_input(uint16_t word){assert(!pending);pending=1;pending_word=word;}
void input_route_observer_dualshock_input(uint16_t word,const uint8_t *sticks,int mode){
    (void)sticks;(void)mode;assert(expect_dual);input_route_observer_input(word);
}
void input_route_observer_applied(uint16_t word,int connected,int analog){
    if(!pending)fprintf(stderr,"digital delivery before any input was sampled\n");
    assert(!expect_dual&&pending&&word==pending_word&&connected&&!analog);pending=0;++applied;
}
void input_route_observer_dualshock_applied(uint16_t word,const uint8_t *sticks,int connected,int capable,int analog){
    (void)sticks;(void)analog;
    assert(expect_dual&&pending&&word==pending_word&&connected&&capable);pending=0;++applied;
}
void input_route_observer_port2_input(uint16_t word){assert(!p2_pending);p2_pending=1;p2_word=word;}
void input_route_observer_port2_applied(uint16_t word,int connected,int analog){
    assert(p2_pending&&word==p2_word&&connected&&!analog);p2_pending=0;++p2_applied;
}
typedef struct { unsigned unused; } PsxFrameFingerprint;
#define PSX_FRAME_FINGERPRINT_INIT {0}
static int s_fp_reset_on_load;
static PsxFrameFingerprint s_fp;
static unsigned s_fp_head,s_fp_total;
static int s_input_override=-1,s_input_override_p2=-1;
static int s_input_frames,s_input_frames_p2,s_axis_override,s_pad_type_override=-1;
static int s_pad_connected_override[2] = {-1, -1};
static unsigned char s_axis_st[4],s_host_axes[4],s_host_lt,s_host_rt;
static int s_host_layer;
static uint16_t s_host_buttons;
static char response[512];
static void send_ok(int id){(void)id;strcpy(response,"ok");}
static void send_err(int id,const char *text){(void)id;snprintf(response,sizeof response,"ERROR:%s",text);}
static void send_fmt(const char *format,...){va_list v;va_start(v,format);vsnprintf(response,sizeof response,format,v);va_end(v);}
static int json_get_int(const char *json,const char *key,int fallback){
    char name[80];snprintf(name,sizeof name,"\"%s\"",key);
    const char *p=strstr(json,name);if(!p)return fallback;
    p=strchr(p,':');assert(p);return atoi(p+1);
}
'''
    body = '\n'.join(function(source, name) for name in (
        'handle_clear_input', 'handle_input_route_clear', 'handle_input_route_append',
        'handle_input_route_start', 'handle_input_route_stop', 'handle_input_route_status',
        'input_route_loaded', 'debug_server_note_savestate_loaded',
        'input_route_next', 'debug_server_seek_input_route', 'input_file_next', 'debug_server_get_input_override',
        'debug_server_get_input_override_port2'))
    checks = r'''
static void queue(void){
    handle_input_route_clear(1,"{}");
    handle_input_route_append(1,"{\"frames\":2,\"buttons\":32767}");
    handle_input_route_append(1,"{\"frames\":1,\"buttons\":65535}");
}
int main(void){
    queue();s_input_override=123;s_input_override_p2=456;
    handle_input_route_start(1,"{\"port\":2,\"at_frame\":10}");
    assert(strstr(response,"\"port\":2"));
    assert(s_input_override==123&&s_input_override_p2==-1);
    s_frame_count=9;
    assert(debug_server_get_input_override_port2()==-1);
    assert(s_input_route_remaining==2);
    s_frame_count=10;
    assert(debug_server_get_input_override()==123);
    assert(s_input_route_remaining==2);
    assert(debug_server_get_input_override_port2()==32767);
    assert(s_input_route_remaining==1);
    ++s_frame_count;
    assert(debug_server_get_input_override()==123);
    assert(debug_server_get_input_override_port2()==32767);
    assert(s_input_route_index==1&&s_input_route_remaining==1);
    ++s_frame_count;
    assert(debug_server_get_input_override_port2()==65535);
    assert(!s_input_route_active);
    assert(debug_server_get_input_override_port2()==-1);
    queue();handle_input_route_start(1,"{\"port\":2}");
    handle_clear_input(1,"{\"port\":1}");assert(s_input_route_active);
    handle_clear_input(1,"{\"port\":2}");assert(!s_input_route_active);
    queue();handle_input_route_start(1,"{}");
    assert(s_input_route_port==1);s_input_override_p2=456;
    assert(debug_server_get_input_override_port2()==456&&s_input_route_remaining==2);
    assert(debug_server_get_input_override()==32767);
    handle_clear_input(1,"{\"port\":2}");assert(s_input_route_active);
    handle_input_route_status(1,"{}");assert(strstr(response,"\"port\":1"));
    handle_input_route_append(1,"{\"frames\":1,\"buttons\":0}");
    assert(strstr(response,"ERROR"));
    handle_input_route_stop(1,"{}");assert(!s_input_route_active);
    handle_input_route_start(1,"{\"port\":3}");
    assert(strstr(response,"ERROR")&&!s_input_route_active);
    handle_input_route_start(1,"{\"port\":2}");
    handle_clear_input(1,"{}");assert(!s_input_route_active);
    handle_input_route_clear(1,"{}");handle_input_route_start(1,"{}");
    assert(strstr(response,"empty"));
    queue();handle_input_route_start(1,"{\"port\":2,\"after_load\":1}");
    assert(s_input_route_active&&s_input_route_wait_load);
    assert(strstr(response,"\"waiting_for_load\":true"));
    for(unsigned n=0;n<20;++n){++s_frame_count;assert(debug_server_get_input_override_port2()==-1);}
    assert(s_input_route_remaining==2);
    handle_input_route_status(1,"{}");assert(strstr(response,"\"waiting_for_load\":true"));
    /* Route admission is independent of optional fingerprint reset. */
    debug_server_note_savestate_loaded();assert(!s_input_route_wait_load);
    assert(debug_server_get_input_override_port2()==32767&&s_input_route_remaining==1);
    debug_server_note_savestate_loaded(); /* An ordinary later load does not restart it. */
    assert(s_input_route_remaining==1);
    assert(debug_server_get_input_override_port2()==32767);
    assert(debug_server_get_input_override_port2()==65535&&!s_input_route_active);
    for(unsigned port=1;port<=2;++port){
        queue();handle_input_route_start(1,port==1?"{\"after_load\":1}":"{\"port\":2,\"after_load\":1}");
        handle_clear_input(1,port==1?"{\"port\":2}":"{\"port\":1}");assert(s_input_route_wait_load);
        handle_clear_input(1,port==1?"{\"port\":1}":"{\"port\":2}");assert(!s_input_route_wait_load&&!s_input_route_active);
    }
    queue();handle_input_route_start(1,"{\"after_load\":1}");
    handle_input_route_stop(1,"{}");debug_server_note_savestate_loaded();assert(!s_input_route_active);
    queue();handle_input_route_start(1,"{\"after_load\":1}");
    handle_input_route_clear(1,"{}");debug_server_note_savestate_loaded();assert(!s_input_route_active);
    queue();handle_input_route_start(1,"{\"after_load\":2}");assert(strstr(response,"ERROR")&&!s_input_route_active);
    handle_input_route_start(1,"{\"after_load\":1,\"at_frame\":10}");assert(strstr(response,"ERROR")&&!s_input_route_active);
    handle_input_route_start(1,"{\"after_load\":1}");s_fp_reset_on_load=1;s_fp_head=4;s_fp_total=5;
    debug_server_note_savestate_loaded();assert(!s_input_route_wait_load&&!s_fp_reset_on_load&&!s_fp_head&&!s_fp_total);
    /* File captures validate the previous sample, never a nonexistent first
     * sample. Keep the debug observer/checkpoint path enabled in this test. */
    for(unsigned mode=0;mode<3;++mode){
        pending=p2_pending=applied=p2_applied=terminal=0;
        expect_dual=s_dualshock_route_mode=mode==2;s_two_port_mode=mode==1;
        s_input_route_active=s_input_route_file_mode=1;s_input_route_count=2;
        s_input_route_index=s_input_route_consumed=0;s_input_route_remaining=1;
        s_input_route[0]=(InputRouteStep){1,0xffef};
        s_input_route[1]=(InputRouteStep){1,0xffff};
        s_input_route_p2[0]=0xfffd;s_input_route_p2[1]=0xffff;
        for(unsigned n=0;n<2;++n){
            s_dualshock_route[n].frames=1;s_dualshock_route[n].buttons=n?0xffff:0xffef;
            memset(s_dualshock_route[n].axes_ly_lx_ry_rx,128,4);s_dualshock_route[n].analog_button=0;
        }
        held[0]=held[1]=0xffff;
        assert(debug_server_get_input_override()==0xffef);
        assert(!applied&&!p2_applied&&s_input_route_consumed==1);
        held[0]=0xffef;if(s_two_port_mode)held[1]=(uint16_t)debug_server_get_input_override_port2();
        assert(debug_server_get_input_override()==0xffff);
        assert(applied==1&&p2_applied==(mode==1)&&s_input_route_consumed==2);
        held[0]=0xffff;if(s_two_port_mode)held[1]=(uint16_t)debug_server_get_input_override_port2();
        terminal=1;assert(debug_server_get_input_override()==0xffff);
        assert(applied==2&&p2_applied==(mode==1?2:0)&&!pending&&!p2_pending&&s_input_route_consumed==2);
        if(mode!=1){
            /* Seeking restores a completed boundary, not a half-delivered
             * sample, even if this process previously issued an input. */
            terminal=0;s_input_route_active=1;s_input_route_index=s_input_route_consumed=0;
            s_input_route_remaining=1;
            assert(debug_server_get_input_override()==0xffef&&pending);
            assert(debug_server_seek_input_route(1)&&!pending);
            int before=applied;
            assert(debug_server_get_input_override()==0xffff&&pending&&applied==before);
            held[0]=0xffff;terminal=1;
            assert(debug_server_get_input_override()==0xffff&&!pending&&applied==before+1);
        }
    }
    puts("debug input route: TCP controls plus first/delivered/terminal digital,P2,DualShock captures pass");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='psx-input-route-') as directory:
        path = Path(directory) / 'route.c'
        path.write_text(preamble + '\n' + declarations +
                        'static uint64_t s_input_route_start_frame = 0;\n' + body + checks)
        for opt in ('-O0', '-O2'):
            executable = Path(directory) / ('route' + opt + '.exe')
            subprocess.run([args.cc, '-std=c11', opt, '-Wall', '-Wextra', '-Werror', '-I'+str(runtime/'include'), str(path),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
