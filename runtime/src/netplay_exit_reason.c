/* Why a netplay match ended, in words (PS1B-290). See netplay_exit_reason.h. */
#include "netplay_exit_reason.h"
#include "sio.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *origin;
    const char *text;
} NetplayExitReason;

static const NetplayExitReason k_reasons[] = {
    { "netplay_boot_mismatch",
      "The match ended: the players' consoles started differently "
      "(different BIOS image or boot settings)." },
    { "netplay_peer_disconnect",
      "The match ended: the other player left or stopped responding." },
    { "netplay_load_failed",
      "The match ended: a player could not load the shared save state." },
    { "netplay_load_stall",
      "The match ended: loading the shared save state timed out." },
    { "netplay_link_stall",
      "The match ended: could not connect to the other player in time." },
    { "netplay_admit_stall",
      "The match ended: the players' games stopped running in step." },
};

const char *netplay_exit_reason_text(const char *origin)
{
    size_t i;
    if (!origin) return NULL;
    for (i = 0; i < sizeof(k_reasons) / sizeof(k_reasons[0]); ++i) {
        if (strcmp(k_reasons[i].origin, origin) == 0)
            return k_reasons[i].text;
    }
    return NULL;
}

void netplay_start_failure_text(int start_rc, int has_netplay,
                                const char *bind_hostport,
                                const char *peer_hostport, int bind_probe,
                                int sys_error, char *out, size_t cap)
{
    char address[64] = "";
    const char *port = "";
    const char *colon;
    if (!out || !cap) return;
    out[0] = '\0';
    if (!bind_hostport) bind_hostport = "";
    if (!peer_hostport) peer_hostport = "";
    /* "address:port": the port is what follows the last colon. */
    colon = strrchr(bind_hostport, ':');
    if (colon && (size_t)(colon - bind_hostport) < sizeof(address)) {
        memcpy(address, bind_hostport, (size_t)(colon - bind_hostport));
        address[colon - bind_hostport] = '\0';
        port = colon + 1;
    }
    if (!has_netplay) {
        snprintf(out, cap, "This build has no netplay: it was built without "
                           "the netplay library.");
    } else if (start_rc == -3 && bind_probe == NETPLAY_BIND_FAILED) {
        snprintf(out, cap,
                 "Netplay could not open UDP port %.5s on %.15s (system error "
                 "%d). Another program or the operating system holds that "
                 "port. Choose another port and start again.",
                 port, address, sys_error);
    } else if (start_rc == -3 && bind_probe == NETPLAY_BIND_BAD_ADDRESS) {
        snprintf(out, cap,
                 "Netplay could not start: \"%.60s\" is not an address and a "
                 "port to listen on.", bind_hostport);
    } else if (start_rc == -3 && peer_hostport[0]) {
        snprintf(out, cap,
                 "Netplay could not start: the other player's address "
                 "\"%.60s\" could not be used.", peer_hostport);
    } else if (start_rc == -3) {
        snprintf(out, cap,
                 "Netplay could not start its connection on %.60s.",
                 bind_hostport);
    } else if (start_rc == -4) {
        snprintf(out, cap,
                 "Netplay could not start the online connection. The log "
                 "names the setting that is missing or wrong.");
    } else {
        snprintf(out, cap, "Netplay could not start (code %d).", start_rc);
    }
}

int netplay_seat_refusal(int sio_device, int port, char *out, size_t cap)
{
    const char *device;
    if (out && cap) out[0] = '\0';
    switch (sio_device) {
    case SIO_DEVICE_PAD:    return 0;
    case SIO_DEVICE_MOUSE:  device = "PS1 Mouse"; break;
    case SIO_DEVICE_NEGCON: device = "neGcon";    break;
    case SIO_DEVICE_GUNCON: device = "GunCon";    break;
    default:                device = "device that is not a pad"; break;
    }
    if (out && cap)
        snprintf(out, cap,
                 "Netplay needs a pad or the keyboard: port %d is set to a %s. "
                 "Change the controller for that port and start again.",
                 port, device);
    return 1;
}

int netplay_boot_mismatch_final(uint32_t mismatch_since_ms, uint32_t now_ms,
                                uint32_t grace_ms)
{
    if (!mismatch_since_ms) return 0;
    return (uint32_t)(now_ms - mismatch_since_ms) >= grace_ms;
}
