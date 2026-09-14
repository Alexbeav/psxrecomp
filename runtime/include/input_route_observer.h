#ifndef PSX_INPUT_ROUTE_OBSERVER_H
#define PSX_INPUT_ROUTE_OBSERVER_H
#include <stdint.h>
#include <stdio.h>
FILE *input_route_observer_output(const char *name);
int input_route_observer_init(uint32_t total);
int input_route_observer_dualshock_init(uint32_t total);
void input_route_observer_set_end(uint32_t total);
void input_route_observer_boundary(uint32_t completed, uint64_t runtime_frame);
void input_route_observer_input(uint16_t buttons);
void input_route_observer_applied(uint16_t buttons, int connected, int analog);
void input_route_observer_dualshock_input(uint16_t buttons, const uint8_t source_axes[4]);
void input_route_observer_dualshock_applied(uint16_t buttons, const uint8_t sticks_lx_ly_rx_ry[4],
                                          int connected, int config_capable, int analog);
#endif
