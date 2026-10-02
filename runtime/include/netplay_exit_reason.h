/* netplay_exit_reason.h — why a netplay match ended, in words (PS1B-290).
 *
 * A match that ends early returns every player to the lobby. The runtime knew
 * why (netplay_soft_exit's origin) but only printed it to stdout, so players
 * saw the lobby again with no reason. These map an origin to one sentence for
 * the launcher's status line, and decide when a boot-digest mismatch is final.
 */
#ifndef NETPLAY_EXIT_REASON_H
#define NETPLAY_EXIT_REASON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The player-facing sentence for a netplay_soft_exit origin, or NULL when the
 * player ended the match themselves (window close, Escape) or the origin is
 * unknown. */
const char *netplay_exit_reason_text(const char *origin);

/* The rollback boot digests of the two peers are both known and different.
 * The peers booted differently, and waiting does not fix that; before PS1B-290
 * only the generic 20 s admit-stall watchdog ended it. The mismatch is final
 * once the same pair of digests has held for grace_ms (the short wait lets a
 * stale peer digest be replaced, and the peer reach the same verdict).
 * mismatch_since_ms is 0 when there is no mismatch. */
int netplay_boot_mismatch_final(uint32_t mismatch_since_ms, uint32_t now_ms,
                                uint32_t grace_ms);

#define NETPLAY_BOOT_MISMATCH_GRACE_MS 3000u

/* Why a netplay start failed, in one sentence (PS1B-386). The start reported
 * every failure as "built without recomp-net, or bind/peer invalid": a player
 * whose system held the port read first that the build had no netplay.
 * `start_rc` is psx_netplay_start's result, `has_netplay` whether the build
 * has the netplay library. For a LAN start (-3) the caller tries the bind
 * itself and passes what the system answered: `bind_probe` and, for a refused
 * bind, the system's error number. The sentence names the port and the
 * address, says that another program or the system holds the port (or, for
 * access denied, Windows 10013 and POSIX 13, that the system does not allow
 * the port: a range kept for Hyper-V or WSL has no program behind it) and
 * that another port can be chosen, and names the build only when the build
 * has no netplay. It fits the lobby client's last_error. */
enum {
    NETPLAY_BIND_NOT_TRIED   = -1, /* not a LAN start, or a host name to resolve */
    NETPLAY_BIND_OK          = 0,  /* the port could be bound when tried again */
    NETPLAY_BIND_FAILED      = 1,  /* the system refused the bind */
    NETPLAY_BIND_BAD_ADDRESS = 2   /* the listen address is not "address:port" */
};
#define NETPLAY_START_FAILURE_CAP 192
void netplay_start_failure_text(int start_rc, int has_netplay,
                                const char *bind_hostport,
                                const char *peer_hostport, int bind_probe,
                                int sys_error, char *out, size_t cap);

/* A netplay match carries pad input only (PS1B-313). A seat whose device is a
 * PS1 Mouse or a GunCon would sit in the match with no input, and a neGcon
 * would lose its twist; before this the match started and nothing told the
 * player. `sio_device` is the device the seat presents (sio.h SIO_DEVICE_*),
 * `port` the seat's number as the player sees it (1 = Player 1).
 * Returns 0 for a pad and leaves `out` empty. Returns 1 for anything else and
 * writes one sentence that names the device and the port; it fits the lobby
 * client's last_error. */
#define NETPLAY_SEAT_REFUSAL_CAP 160
int netplay_seat_refusal(int sio_device, int port, char *out, size_t cap);

/* The peers of a match must boot the BIOS the same way (PS1B-382). The boot
 * has three inputs, each resolved per player: the kit's game.toml, then the
 * player's settings.toml, then the launcher. Two peers of one build that
 * differed started a match and forked for good at sim 22, with nothing said.
 * A match therefore boots with the kit's values, which both peers share.
 * Sets `*mode` to `*kit`. Returns 1 when that changed a value (the player had
 * chosen otherwise, and is shown NETPLAY_BOOT_MODE_NOTICE), 0 when it did not. */
typedef struct NetplayBootMode {
    int bios_hle;    /* [runtime] bios_hle */
    int keep_intro;  /* [runtime] bios_hle_keep_intro */
    int fast_boot;   /* [runtime] fast_boot (alias of the boot skip) */
} NetplayBootMode;
#define NETPLAY_BOOT_MODE_NOTICE \
    "Netplay: this match uses the game's own BIOS setting."
int netplay_boot_mode_settle(const NetplayBootMode *kit, NetplayBootMode *mode);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_EXIT_REASON_H */
