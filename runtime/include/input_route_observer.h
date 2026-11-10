#ifndef PSX_INPUT_ROUTE_OBSERVER_H
#define PSX_INPUT_ROUTE_OBSERVER_H
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Nonzero only after a declared endpoint requests its final CPU-return observation. */
uint32_t input_route_observer_terminal_frame(void);
FILE *input_route_observer_output(const char *name);
int input_route_observer_init(uint32_t total);
int input_route_observer_dualshock_init(uint32_t total);
void input_route_observer_set_end(uint32_t total);
void input_route_observer_resume(uint32_t consumed);
/* Full-audit diagnostic continuation. The sidecar contains the actual journal
 * prefix and portable hash state; a legacy state cannot stand in for it. */
int input_route_observer_checkpoint_active(void);
int input_route_observer_checkpoint_save(const char *path,uint32_t consumed,uint32_t cpu_return,char sha256[65]);
int input_route_observer_checkpoint_prepare(const char *path,const char *sha256,uint32_t consumed,uint32_t cpu_return);
void input_route_observer_source_frame(uint32_t frame);
void input_route_observer_boundary(uint32_t completed, uint64_t runtime_frame);
void input_route_observer_input(uint16_t buttons);
void input_route_observer_applied(uint16_t buttons, int connected, int analog);
/* PSXRTI3 two-digital-pad route: P1 goes through the digital calls above and
 * P2 through these, input before applied, P2 applied before P1 applied. */
int input_route_observer_two_digital_init(uint32_t total);
void input_route_observer_port2_input(uint16_t buttons);
void input_route_observer_port2_applied(uint16_t buttons, int connected, int analog);
void input_route_observer_dualshock_input(uint16_t buttons, const uint8_t source_axes[4], uint8_t mode_button);
void input_route_observer_dualshock_applied(uint16_t buttons, const uint8_t sticks_lx_ly_rx_ry[4],
                                          int connected, int config_capable, int analog);
#ifdef __cplusplus
}
#endif
#endif
