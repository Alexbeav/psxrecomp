#include "mouse_camera.h"

#include <assert.h>
#include <string.h>

static uint32_t ram[0x20000 / 4];

static uint32_t read_word(uint32_t address) {
    return ram[(address & 0x1FFFFu) >> 2];
}

static void write_word(uint32_t address, uint32_t value) {
    ram[(address & 0x1FFFFu) >> 2] = value;
}

int main(void) {
    CPUState cpu;
    PsxMouseCameraStats stats;
    const PsxMouseCameraConfig config = {
        .enabled = 1,
        .facing_site = 0x80001000u,
        .facing_expected = 0x8EA30034u,
        .application_state_addr = 0x80001100u,
        .player_pointer_addr = 0x80001104u,
        .player_state_offset = 0x20u,
        .wrapper_offset = 0xE0u,
        .base_offset = 0xA4u,
        .owner_offset = 0xDCu,
        .desired_pitch_offset = 0x8E8u,
        .rendered_pitch_offset = 0x918u,
        .vector_x_offset = 0xCCu,
        .vector_y_offset = 0xD0u,
        .vector_z_offset = 0xD4u,
        .controller_reg = 18,
        .chase_yaw_sensitivity = 0.75,
        .chase_pitch_sensitivity = 1.0,
        .aim_yaw_sensitivity = 2.0,
        .aim_pitch_sensitivity = 0.5,
        .invert_y = 0,
    };
    memset(&cpu, 0, sizeof(cpu));
    memset(ram, 0, sizeof(ram));
    cpu.read_word = read_word;
    cpu.write_word = write_word;
    write_word(config.facing_site, config.facing_expected);
    write_word(config.application_state_addr, 0u);
    write_word(config.player_pointer_addr, 0x80002000u);
    write_word(0x80002020u, 0x80003000u);
    write_word(0x800030E0u, 0x80004000u);
    write_word(0x800040A4u, 0x80005000u);
    write_word(0x800040DCu, 0x80002000u);
    write_word(0x800058E8u, 100u);
    cpu.gpr[18] = 0x80006000u;

    psx_mouse_camera_configure(&config);
    psx_mouse_camera_set_focus(1);
    psx_mouse_camera_add_motion(8, 5);
    psx_mouse_camera_hook(&cpu, config.facing_site);
    assert((int32_t)read_word(0x800060CCu) == 6 * 4096);
    assert((int32_t)read_word(0x800060D4u) == 0);
    assert((int32_t)read_word(0x800058E8u) == 105);
    assert((int32_t)read_word(0x80005918u) == 105);

    /* Vblank/debug polling may run more than once before the semantic block;
     * retain a bounded four-callback window, then expire without application. */
    psx_mouse_camera_add_motion(4, 0);
    psx_mouse_camera_commit_frame();
    psx_mouse_camera_commit_frame();
    psx_mouse_camera_commit_frame();
    psx_mouse_camera_commit_frame();
    psx_mouse_camera_hook(&cpu, config.facing_site);
    assert((int32_t)read_word(0x800060CCu) == 3 * 4096);

    psx_mouse_camera_set_aim(1);
    psx_mouse_camera_add_motion(-3, 10);
    psx_mouse_camera_hook(&cpu, config.facing_site);
    assert((int32_t)read_word(0x800060CCu) == -6 * 4096);
    assert((int32_t)read_word(0x800060D4u) == 5 * 4096);

    /* Scripted ownership rejects and discards motion. */
    write_word(0x800040DCu, 0x80007000u);
    psx_mouse_camera_add_motion(20, 20);
    psx_mouse_camera_hook(&cpu, config.facing_site);
    assert((int32_t)read_word(0x800060CCu) == -6 * 4096);
    write_word(0x800040DCu, 0x80002000u);
    psx_mouse_camera_hook(&cpu, config.facing_site);
    assert((int32_t)read_word(0x800060CCu) == -6 * 4096);

    /* A changed executable word fails closed. */
    write_word(config.facing_site, 0u);
    psx_mouse_camera_add_motion(7, 0);
    psx_mouse_camera_hook(&cpu, config.facing_site);
    psx_mouse_camera_get_stats(&stats);
    assert(stats.applied_chase == 2);
    assert(stats.applied_aim == 1);
    assert(stats.rejected_owner == 1);
    assert(stats.rejected_word == 1);
    assert(stats.last_yaw == -6);
    assert(stats.last_pitch == 5);
    return 0;
}
