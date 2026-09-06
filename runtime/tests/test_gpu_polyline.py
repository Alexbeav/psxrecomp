"""Compile the actual GP0 reader with a recording renderer and synthetic packets.

The VRAM upload branch and drawing backend are outside this parser test.
No retail bytes are included. Requires a GCC/Clang-compatible C compiler.
"""
import argparse
import pathlib
import subprocess
import tempfile

PREAMBLE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
enum { GP0_IDLE, GP0_COLLECTING, GP0_POLYLINE_MONO, GP0_POLYLINE_SHADED };
static int gp0_state, gp0_words_collected, gp0_words_needed;
static uint32_t gp0_cmd_buf[16], gp0_cmd_source_addr, gp0_next_source_addr;
static uint32_t gp0_opcode_count[256], gp0_draw_count;
static int polyline_has_prev, polyline_prev_x, polyline_prev_y;
static int draw_offset_x, draw_offset_y, semi_transparency, polyline_semi_trans;
static uint16_t polyline_prev_c, polyline_color;
static int lines, commands;
static uint32_t last_command, header_source;
static uint16_t rgb888_to_rgb555(uint32_t v) {
    return ((v>>3)&31) | ((v>>6)&992) | ((v>>9)&31744);
}
static void parse_vertex(uint32_t v,int32_t*x,int32_t*y) {
    *x=(int16_t)(v<<5)>>5; *y=(int16_t)(v>>11)>>5;
}
static int psx_gpu_line_oversize(int x,int y,int a,int b) {
    return abs(a-x)>1023 || abs(b-y)>511;
}
static void gr_draw_line(int x,int y,int a,int b,int c) { lines++; }
static void gr_draw_shaded_line(int x,int y,int c,int a,int b,int d) { lines++; }
static void gr_set_semi_transparency(int a,int b) {}
static void gp0_ring_record(const uint32_t*w,int n) {header_source=gp0_cmd_source_addr;}
static void gp0_execute_command(void) {commands++;last_command=gp0_cmd_buf[0];}
static void psx_fatal_halt(const char*s) {fprintf(stderr,"%s\n",s);exit(3);}
'''
CASES = r'''
static void run(const uint32_t *words,int count,int expected) {
    gp0_state=GP0_IDLE; lines=commands=0;
    /* Deliberately stale packet length: a new polyline must reset it. */
    gp0_words_collected=12;
    for(int i=0;i<count;i++) {gp0_next_source_addr=0x1000+i*4;feed(words[i]);}
    assert(gp0_state==GP0_IDLE);
    assert(lines==expected);
    assert(header_source==0x1000);
    assert(commands==1 && last_command==0xE1000123);
}
#define RUN(expected,...) do {uint32_t w[]={__VA_ARGS__};run(w,sizeof(w)/4,expected);}while(0)
int main(void) {
    /* Second color matches the sentinel mask but is mandatory initial data. */
    RUN(2,0x5A000011,0x00080008,0x52505050,0x00080018,
        0xFC000022,0x00180018,0x55555555,0xE1000123);
    /* Both mandatory mono vertices can match the sentinel mask. */
    RUN(1,0x48000011,0x50005000,0x50005008,0x55555555,0xE1000123);
    /* Mandatory shaded coordinates also cannot terminate the command. */
    RUN(1,0x58000011,0x50005000,0x55555555,0x50005008,
        0x55555555,0xE1000123);
    /* Later shaded coordinate is data; only a color slot can terminate. */
    RUN(2,0x58000011,0x00080008,0x000000FF,0x00080018,
        0x0000FF00,0x50005008,0x55555555,0xE1000123);
    /* Negative coordinates preserve alignment, including third vertices. */
    RUN(2,0x48000011,0xFFF8FFF8,0x00080008,0xFFF00010,
        0x55555555,0xE1000123);
    /* Noncanonical sentinel bits are valid at an eligible position. */
    RUN(1,0x58000011,0x00080008,0x000000FF,0x00080018,
        0x5ABC5DEF,0xE1000123);
    puts("PASS: six GP0 polyline streams and following command alignment");
    return 0;
}
'''

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--cc', default='cc')
    ap.add_argument('--gpu-source', type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parents[1]/'src/gpu.c')
    args=ap.parse_args()
    source=args.gpu_source.read_text(encoding='utf-8')
    a=source.index('static int gp0_command_word_count(')
    b=source.index('\nstatic void ws_ui_prepass_add',a)
    count=source[a:b]
    a=source.index('    /* State: mono polyline')
    b=source.index('\nvoid gpu_write_gp0(',a)
    # Include the complete remainder of gpu_write_gp0_body, not a parser copy.
    body=source[a:b]
    with tempfile.TemporaryDirectory(prefix='gp0-polyline-') as tmp:
        c=pathlib.Path(tmp)/'reader.c'
        c.write_text(PREAMBLE+count+'\nstatic void feed(uint32_t val) {\n'+body+CASES,
                     encoding='utf-8')
        for opt in ['-O0','-O2']:
            exe=pathlib.Path(tmp)/'reader.exe'
            subprocess.run([args.cc,'-std=c11',opt,str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__': main()
