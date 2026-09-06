"""Compile the production ReadN/ReadS cases against a synthetic drive.

No retail media required. Pass C compiler and a private output directory.
"""
from pathlib import Path
import subprocess, sys

source = (Path(__file__).parents[1] / 'src/cdrom.c').read_text()
start = source.index('static int reject_audio_disc_data_read(void) {')
end = source.index('\nstatic void exec_command(uint8_t cmd) {', start)
helper = source[start:end]
cases = []
for marker in ['    case 0x06: /* ReadN */', '    case 0x1B: /* ReadS */']:
    start = source.index(marker)
    cases.append(source[start:source.index('\n    case ', start + len(marker))])

harness = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define CDSTAT_ERROR 1
#define CDIRQ_ERROR 5
#define CDIRQ_ACK 3
static void *iso_handle = (void*)1;
static uint8_t mode_reg, stat_reg = 2;
static int track_count, data_track, present, bytes[8], count, irq, reads;
static int iso_track_count(void *p) { (void)p; return track_count; }
static int iso_track_is_audio(void *p, int t) { (void)p; return t != data_track; }
static int has_disc(void) { return present; }
static void response_push(int v) { bytes[count++] = v; }
static void set_irq(int v) { irq = v; }
static int read_continues_current_stream(void) { return 0; }
static void start_read_stream(int cmd) { (void)cmd; reads++; }
'''+helper+'\nstatic void execute(uint8_t cmd) { switch(cmd) {\n'+''.join(cases)+r'''
} }
int main(void) {
  const int commands[] = {6, 27};
  for (unsigned i=0; i<2; ++i) {
    /* 1-track and album audio-only media, with and without CDDA mode. */
    for (int n=1; n<=12; n+=11) for (int mode=0; mode<256; ++mode) {
      track_count=n; data_track=0; present=1; mode_reg=mode;
      count=irq=reads=0; execute(commands[i]);
      if (mode & 1) { assert(reads==1 && irq==3 && count==1 && bytes[0]==2); }
      else { assert(reads==0 && irq==5 && count==2 && bytes[0]==3 && bytes[1]==0x40); }
    }
    /* Every possible data-track position preserves the mixed-mode path. */
    for (int t=1; t<=8; ++t) {
      track_count=8; data_track=t; present=1; mode_reg=0;
      count=irq=reads=0; execute(commands[i]);
      assert(reads==1 && irq==3 && count==1);
    }
    present=0; count=irq=reads=0; execute(commands[i]);
    assert(reads==0 && irq==5 && count==1);
  }
  puts("PASS: production ReadN/ReadS audio-disc errors, all mode bytes, mixed-mode and no-disc controls");
}
'''
out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
fixture = out/'audio_disc_read_fixture.c'
fixture.write_text(harness)
exe = out/'audio_disc_read_fixture.exe'
subprocess.run([sys.argv[1], '-std=c11', '-Wall', '-Wextra', '-Werror', str(fixture), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
