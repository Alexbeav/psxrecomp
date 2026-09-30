/* netplay_bios_settle.h — which BIOS every peer boots for a netplay match.
 *
 * Each seat offers what it can boot: the bundled OpenBIOS and/or one retail
 * image, named by the CRC-32 of the file it would actually load. The host
 * settles one BIOS for the room from those offers and sends the result to
 * every peer, which then boots only that image.
 *
 * The wire token for retail is still "scph1001" so older peers read it, but it
 * means "the retail image named by retail_crc", not SCPH-1001. Before PS1B-289
 * the token was the whole identity: an SCPH-5552 peer and an SCPH-1001 peer
 * both offered "scph1001", each booted its own dump, and the rollback boot
 * digest never matched.
 *
 * Rule (netplay_bios_settle):
 *   1. Retail is possible only when every seat offered a retail image and all
 *      the CRCs are equal. A seat with no offer, no dump, or no CRC (an older
 *      client) makes retail impossible.
 *   2. Retail wins when it is possible and the host prefers it, or when nobody
 *      prefers OpenBIOS.
 *   3. Otherwise OpenBIOS, when every seat that sent an offer links it. A seat
 *      with no offer is an older client and is assumed to link it, as before.
 *   4. Otherwise retail, if it is possible.
 *   5. Otherwise there is no BIOS every seat can boot: the host must not start.
 */
#ifndef NETPLAY_BIOS_SETTLE_H
#define NETPLAY_BIOS_SETTLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NetplayBiosSeat {
    int      offered;          /* 0 = no offer received (older client / not ready) */
    int      can_openbios;     /* links the bundled OpenBIOS backend */
    int      can_retail;       /* has a retail dump a linked backend accepts */
    int      prefer_openbios;  /* the player picked OpenBIOS */
    uint32_t retail_crc;       /* CRC-32 of the retail file it would boot; 0 = not sent */
    int      is_host;
} NetplayBiosSeat;

enum {
    NETPLAY_BIOS_OPENBIOS = 0,
    NETPLAY_BIOS_RETAIL   = 1,
    NETPLAY_BIOS_NONE     = 2   /* no BIOS every seat can boot */
};

typedef struct NetplayBiosSettle {
    int      kind;         /* NETPLAY_BIOS_* */
    uint32_t retail_crc;   /* RETAIL: the image every seat boots */
    /* NONE: two seats that cannot share a retail image (-1 when unused).
     * seat_b is -1 when seat_a simply has no usable retail offer. */
    int      seat_a;
    int      seat_b;
} NetplayBiosSettle;

NetplayBiosSettle netplay_bios_settle(const NetplayBiosSeat *seats, int count);

/* Wire token for a settled kind: "openbios", "scph1001" (retail), or "". */
const char *netplay_bios_token(int kind);

/* retail_crc on the wire: eight lowercase hex digits, "" for 0. */
void     netplay_bios_format_crc(uint32_t crc, char *out, size_t cap);
/* Parses eight hex digits (surrounding spaces allowed). Returns 0 for
 * anything else, which callers treat as "not sent". */
uint32_t netplay_bios_parse_crc(const char *text);

/* Short player-facing name for a retail image CRC ("SCPH-5552"), or the CRC
 * in hex ("BIOS d786f0b9") when the image is not in psx_bios_known_images.h. */
void     netplay_bios_describe_crc(uint32_t crc, char *out, size_t cap);

/* One line explaining a NONE result, for the launcher's status line. */
void     netplay_bios_describe_refusal(const NetplayBiosSettle *s,
                                       const NetplayBiosSeat *seats, int count,
                                       char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_BIOS_SETTLE_H */
