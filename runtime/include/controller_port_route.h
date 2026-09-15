#ifndef PSX_CONTROLLER_PORT_ROUTE_H
#define PSX_CONTROLLER_PORT_ROUTE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Host controller assignments and emulated SIO ports are separate domains.
 * A live port swap changes only this permutation; device ownership remains
 * stable, so hotplug and rumble can use the inverse mapping consistently. */
int controller_port_route_host_for_sio(int sio_slot, int swapped);
int controller_port_route_sio_for_host(int host_player, int swapped);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CONTROLLER_PORT_ROUTE_H */
