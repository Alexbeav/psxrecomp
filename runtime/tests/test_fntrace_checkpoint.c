#include "fntrace.h"
#include <assert.h>
static unsigned clears, scratch, speed, captures;
void dirty_ram_clear_image_baseline(void) { clears++; }
void memory_clear_low_boot_scratch(void) { scratch++; }
void cdrom_notify_game_started(void) { speed++; }
void boot_state_trigger_capture(const CPUState *cpu) { (void)cpu; captures++; }
int main(void) {
    CPUState cpu={0};
    fntrace_set_game_range(0x80010000,0x80020000);
    fntrace_restore_game_started(1);
    fntrace_mark_game_started(&cpu);
    assert(fntrace_is_game_started() && !clears && !scratch && !speed && !captures);
    fntrace_restore_game_started(0);
    fntrace_mark_game_started(&cpu);fntrace_mark_game_started(&cpu);
    assert(clears==1 && scratch==1 && speed==1 && captures==1);
    return 0;
}
