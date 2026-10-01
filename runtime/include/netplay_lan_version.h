/* netplay_lan_version.h — the build check for LAN / Direct IP rooms (PS1B-295).
 *
 * The online lobby server refuses a join whose game_version differs from the
 * room's. A LAN / Direct IP room has no server, and its JOIN carried no
 * version, so two builds of different framework commits could meet there and
 * desync. The JOIN tail now ends with the guest's lobby version (line 7, after
 * the three BIOS lines, the two memory-card lines and the BIOS CRC), and the
 * host applies the server's rule.
 */
#ifndef NETPLAY_LAN_VERSION_H
#define NETPLAY_LAN_VERSION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The JOIN tail line that carries the version, counted from 0. */
#define NETPLAY_LAN_VERSION_LINE 6

/* Copy the version line of a JOIN tail into out. out is "" when the tail is
 * NULL or too short (a guest from before this check). Does not modify tail. */
void netplay_lan_join_version(const char *tail, char *out, size_t out_cap);

/* 1 when a guest that sent guest_version may join a host on host_version.
 * The online server's rule: a missing or blank version counts as "dev", and
 * the two must then be equal. So release builds of one commit meet, dev builds
 * meet each other and older guests, and nothing else does. */
int netplay_lan_version_ok(const char *host_version, const char *guest_version);

/* What the guest is told when the host refuses it for this reason. */
#define NETPLAY_LAN_VERSION_GUEST_TEXT \
    "That room runs another build of the game. Both players need the same release."
/* What the host is told. */
#define NETPLAY_LAN_VERSION_HOST_TEXT \
    "A player with another build of the game tried to join. Both players need the same release."

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_LAN_VERSION_H */
