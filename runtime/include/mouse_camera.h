#ifndef PSX_MOUSE_CAMERA_H
#define PSX_MOUSE_CAMERA_H

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxMouseCameraConfig {
    int enabled;
    uint32_t facing_site;
    uint32_t facing_expected;
    uint32_t application_state_addr;
    uint32_t player_pointer_addr;
    uint32_t player_state_offset;
    uint32_t wrapper_offset;
    uint32_t base_offset;
    uint32_t owner_offset;
    uint32_t desired_pitch_offset;
    uint32_t rendered_pitch_offset;
    uint32_t vector_x_offset;
    uint32_t vector_y_offset;
    uint32_t vector_z_offset;
    int controller_reg;
    double chase_yaw_sensitivity;
    double chase_pitch_sensitivity;
    double aim_yaw_sensitivity;
    double aim_pitch_sensitivity;
    int invert_y;
} PsxMouseCameraConfig;

void psx_mouse_camera_configure(const PsxMouseCameraConfig* config);
int  psx_mouse_camera_enabled(void);
void psx_mouse_camera_set_focus(int focused);
void psx_mouse_camera_set_aim(int aiming);
void psx_mouse_camera_add_motion(int dx, int dy);
void psx_mouse_camera_commit_frame(void);
void psx_mouse_camera_reset(void);

/* Emitted only at a configured, full-word-verified resident block leader. */
void psx_mouse_camera_hook(CPUState* cpu, uint32_t site);

typedef struct PsxMouseCameraStats {
    uint64_t hook_calls;
    uint64_t applied_chase;
    uint64_t applied_aim;
    uint64_t rejected_word;
    uint64_t rejected_state;
    uint64_t rejected_owner;
    uint32_t last_controller;
    uint32_t last_player;
    uint32_t last_wrapper;
    uint32_t last_base;
    int32_t last_yaw;
    int32_t last_pitch;
    int32_t last_chase_pitch;
} PsxMouseCameraStats;

void psx_mouse_camera_get_stats(PsxMouseCameraStats* out);

#ifdef __cplusplus
}
#endif
#endif
