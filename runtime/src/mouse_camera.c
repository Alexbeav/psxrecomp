#include "mouse_camera.h"

#include <math.h>
#include <string.h>

static PsxMouseCameraConfig s_config;
static PsxMouseCameraStats s_stats;
static int s_focused;
static int s_aiming;
static int s_pending_x;
static int s_pending_y;
static int s_pending_age;
static int s_pitch_valid;
static int32_t s_pitch_target;
static uint32_t s_pitch_base;

static int clamp_int(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}

static int32_t normalize_angle(uint32_t value) {
    int32_t angle = (int32_t)(value & 0xFFFu);
    return angle > 0x7FF ? angle - 0x1000 : angle;
}

static int is_ram_pointer(uint32_t value) {
    return value != 0u && (value & 0x1FFFFFFFu) < 0x00200000u;
}

static int scaled(int value, double sensitivity, int limit) {
    const double result = (double)value * sensitivity;
    const long rounded = lround(result);
    return clamp_int((int)rounded, -limit, limit);
}

void psx_mouse_camera_configure(const PsxMouseCameraConfig* config) {
    memset(&s_config, 0, sizeof(s_config));
    if (config) s_config = *config;
    psx_mouse_camera_reset();
    memset(&s_stats, 0, sizeof(s_stats));
}

int psx_mouse_camera_enabled(void) { return s_config.enabled != 0; }

void psx_mouse_camera_set_focus(int focused) {
    s_focused = focused ? 1 : 0;
    if (!s_focused) psx_mouse_camera_reset();
}

void psx_mouse_camera_set_aim(int aiming) {
    s_aiming = aiming ? 1 : 0;
}

void psx_mouse_camera_add_motion(int dx, int dy) {
    if (!s_config.enabled || !s_focused) return;
    s_pending_x = clamp_int(s_pending_x + dx, -2048, 2048);
    s_pending_y = clamp_int(s_pending_y + dy, -2048, 2048);
    s_pending_age = 0;
}

void psx_mouse_camera_commit_frame(void) {
    /* SDL motion is sampled at the vblank callback; the semantic guest block
     * normally runs before the next one. Allow a bounded four-callback window
     * for nested/accelerated vblanks, then expire so motion cannot cross a
     * scripted-camera gap. */
    if ((s_pending_x || s_pending_y) && ++s_pending_age > 4) {
        s_pending_x = 0;
        s_pending_y = 0;
        s_pending_age = 0;
    }
}

void psx_mouse_camera_reset(void) {
    s_pending_x = 0;
    s_pending_y = 0;
    s_pending_age = 0;
    s_aiming = 0;
    s_pitch_valid = 0;
    s_pitch_target = 0;
    s_pitch_base = 0;
}

void psx_mouse_camera_hook(CPUState* cpu, uint32_t site) {
    uint32_t player, player_state, wrapper, owner, base, controller;
    int yaw, pitch;
    if (!cpu || !s_config.enabled || site != s_config.facing_site) return;
    ++s_stats.hook_calls;

    if (cpu->read_word(site) != s_config.facing_expected) {
        ++s_stats.rejected_word;
        psx_mouse_camera_reset();
        return;
    }
    if (cpu->read_word(s_config.application_state_addr) != 0u) {
        ++s_stats.rejected_state;
        psx_mouse_camera_reset();
        return;
    }
    player = cpu->read_word(s_config.player_pointer_addr);
    player_state = player ? cpu->read_word(player + s_config.player_state_offset) : 0u;
    wrapper = player_state ? cpu->read_word(player_state + s_config.wrapper_offset) : 0u;
    owner = wrapper ? cpu->read_word(wrapper + s_config.owner_offset) : 0u;
    base = wrapper ? cpu->read_word(wrapper + s_config.base_offset) : 0u;
    controller = cpu->gpr[s_config.controller_reg];
    s_stats.last_controller = controller;
    s_stats.last_player = player;
    s_stats.last_wrapper = wrapper;
    s_stats.last_base = base;
    if (!is_ram_pointer(player) || !is_ram_pointer(player_state) ||
        !is_ram_pointer(wrapper) || !is_ram_pointer(base) || owner != player ||
        !is_ram_pointer(controller)) {
        ++s_stats.rejected_owner;
        psx_mouse_camera_reset();
        return;
    }

    if (!s_pending_x && !s_pending_y) return;
    if (s_config.invert_y) s_pending_y = -s_pending_y;
    if (s_aiming) {
        yaw = scaled(s_pending_x, s_config.aim_yaw_sensitivity, 256);
        pitch = scaled(s_pending_y, s_config.aim_pitch_sensitivity, 96);
        s_stats.last_yaw = yaw;
        s_stats.last_pitch = pitch;
        cpu->write_word(controller + s_config.vector_x_offset,
                        (uint32_t)(yaw * 4096));
        cpu->write_word(controller + s_config.vector_y_offset, 0u);
        cpu->write_word(controller + s_config.vector_z_offset,
                        (uint32_t)(pitch * 4096));
        ++s_stats.applied_aim;
    } else {
        yaw = scaled(s_pending_x, s_config.chase_yaw_sensitivity, 256);
        pitch = scaled(s_pending_y, s_config.chase_pitch_sensitivity, 96);
        s_stats.last_yaw = yaw;
        s_stats.last_pitch = pitch;
        if (yaw) {
            cpu->write_word(controller + s_config.vector_x_offset,
                            (uint32_t)(yaw * 4096));
            cpu->write_word(controller + s_config.vector_y_offset, 0u);
            cpu->write_word(controller + s_config.vector_z_offset, 0u);
        }
        if (!s_pitch_valid || s_pitch_base != base) {
            s_pitch_target = clamp_int(
                normalize_angle(cpu->read_word(
                    base + s_config.desired_pitch_offset)), -512, 512);
            s_pitch_base = base;
            s_pitch_valid = 1;
        }
        s_pitch_target = clamp_int(s_pitch_target + pitch, -512, 512);
        s_stats.last_chase_pitch = s_pitch_target;
        cpu->write_word(base + s_config.desired_pitch_offset,
                        (uint32_t)s_pitch_target);
        cpu->write_word(base + s_config.rendered_pitch_offset,
                        (uint32_t)s_pitch_target);
        ++s_stats.applied_chase;
    }
    s_pending_x = 0;
    s_pending_y = 0;
    s_pending_age = 0;
}

void psx_mouse_camera_get_stats(PsxMouseCameraStats* out) {
    if (out) *out = s_stats;
}
