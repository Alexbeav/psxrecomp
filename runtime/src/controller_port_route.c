#include "controller_port_route.h"

static int swap_first_two(int slot, int swapped)
{
    if (!swapped)
        return slot;
    if (slot == 0)
        return 1;
    if (slot == 1)
        return 0;
    return slot;
}

int controller_port_route_host_for_sio(int sio_slot, int swapped)
{
    return swap_first_two(sio_slot, swapped);
}

int controller_port_route_sio_for_host(int host_player, int swapped)
{
    /* The two-port swap is its own inverse. */
    return swap_first_two(host_player, swapped);
}
