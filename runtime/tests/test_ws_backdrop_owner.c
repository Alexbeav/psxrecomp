#include "ws_backdrop_owner.h"

#include <stdio.h>

int main(void) {
    WsBackdropOwner owner = {0xFFFFFFFFu, 0xFFFFu, 0};

    if (ws_backdrop_owner_match(&owner, 10, 0, 0)) return 1;
    if (ws_backdrop_owner_match(&owner, 10, 0xFFFFu, 1)) return 2;
    if (!ws_backdrop_owner_match(&owner, 10, 4, 1)) return 3;
    if (!ws_backdrop_owner_match(&owner, 10, 4, 1)) return 4;
    if (ws_backdrop_owner_match(&owner, 10, 5, 1)) return 5;
    if (ws_backdrop_owner_match(&owner, 10, 4, 0)) return 6;

    /* A new guest frame must discover its owner again. */
    if (!ws_backdrop_owner_match(&owner, 11, 9, 1)) return 7;
    if (ws_backdrop_owner_match(&owner, 11, 4, 1)) return 8;

    puts("PASS: first textured OT rank owns only its frame's backdrop pass");
    return 0;
}
