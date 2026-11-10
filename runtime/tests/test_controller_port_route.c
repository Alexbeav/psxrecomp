#include "controller_port_route.h"

#include <stdio.h>

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void)
{
    int slot;
    check(controller_port_route_host_for_sio(0, 0) == 0,
          "normal route keeps host P1 on SIO port 1");
    check(controller_port_route_host_for_sio(1, 0) == 1,
          "normal route keeps host P2 on SIO port 2");
    check(controller_port_route_host_for_sio(0, 1) == 1,
          "swapped route sends host P2 to SIO port 1");
    check(controller_port_route_host_for_sio(1, 1) == 0,
          "swapped route sends host P1 to SIO port 2");
    check(controller_port_route_host_for_sio(2, 1) == 2,
          "route leaves multitap seats unchanged");
    for (slot = 0; slot < 8; slot++) {
        int sio = controller_port_route_sio_for_host(slot, 1);
        check(controller_port_route_host_for_sio(sio, 1) == slot,
              "host/SIO mappings are inverse");
    }
    if (failures)
        return 1;
    printf("PASS: controller port routing permutation\n");
    return 0;
}
