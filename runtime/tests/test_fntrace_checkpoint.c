#include "fntrace.h"
#include <assert.h>
#include <string.h>
static unsigned clears, scratch, speed, captures;
/* Synthetic guest state, including a nonzero low halfword like the BIOS. */
static const uint32_t boot_words[4] = {3, 0x11223344, 0x55667788, 0x99aabbcc};
static uint32_t low_ram[4];
void dirty_ram_clear_image_baseline(void) { clears++; }
/* Negative control for the old handoff: it destroys these guest words. */
void memory_clear_low_boot_scratch(void) { scratch++; memset(low_ram,0,sizeof(low_ram)); }
void cdrom_notify_game_started(void) { speed++; }
void boot_state_trigger_capture(const CPUState *cpu) { (void)cpu; captures++; }
int main(void) {
    CPUState cpu={0};
    memcpy(low_ram,boot_words,sizeof(low_ram));
    fntrace_set_game_range(0x80010000,0x80020000);
    fntrace_restore_game_started(1);
    fntrace_mark_game_started(&cpu);
    assert(fntrace_is_game_started() && !clears && !scratch && !speed && !captures);
    assert(memcmp(low_ram,boot_words,sizeof(low_ram))==0);
    fntrace_restore_game_started(0);
    fntrace_mark_game_started(&cpu);
    assert(memcmp(low_ram,boot_words,sizeof(low_ram))==0);
    fntrace_mark_game_started(&cpu);
    assert(clears==1 && scratch==0 && speed==1 && captures==1);
    assert(memcmp(low_ram,boot_words,sizeof(low_ram))==0);
    return 0;
}
