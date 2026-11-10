/* Which BIOS every peer boots for a netplay match.
 * See netplay_bios_settle.h for the rule. */
#include "netplay_bios_settle.h"
#include "psx_bios_known_images.h"

#include <stdio.h>
#include <string.h>

NetplayBiosSettle netplay_bios_settle(const NetplayBiosSeat *seats, int count)
{
    NetplayBiosSettle s;
    int retail_ok = count > 0;
    int open_ok = count > 0;
    int host_prefers_retail = 0;
    int any_prefer_open = 0;
    int first = -1;
    int i;

    s.kind = NETPLAY_BIOS_OPENBIOS;
    s.retail_crc = 0;
    s.seat_a = -1;
    s.seat_b = -1;
    if (!seats || count <= 0)
        return s;

    for (i = 0; i < count; ++i) {
        const NetplayBiosSeat *seat = &seats[i];
        if (!seat->offered) {
            /* Older client: no retail identity; assumed to link OpenBIOS. */
            if (retail_ok) s.seat_a = i;
            retail_ok = 0;
            continue;
        }
        if (!seat->can_openbios) open_ok = 0;
        if (seat->prefer_openbios) any_prefer_open = 1;
        if (seat->is_host && !seat->prefer_openbios && seat->can_retail)
            host_prefers_retail = 1;
        if (!seat->can_retail || !seat->retail_crc) {
            if (retail_ok) s.seat_a = i;
            retail_ok = 0;
            continue;
        }
        if (first < 0) {
            first = i;
        } else if (seat->retail_crc != seats[first].retail_crc && retail_ok) {
            retail_ok = 0;
            s.seat_a = first;
            s.seat_b = i;
        }
    }

    if (retail_ok && (host_prefers_retail || !any_prefer_open)) {
        s.kind = NETPLAY_BIOS_RETAIL;
    } else if (open_ok) {
        s.kind = NETPLAY_BIOS_OPENBIOS;
    } else if (retail_ok) {
        s.kind = NETPLAY_BIOS_RETAIL;
    } else {
        s.kind = NETPLAY_BIOS_NONE;
        return s;
    }
    if (s.kind == NETPLAY_BIOS_RETAIL)
        s.retail_crc = seats[first].retail_crc;
    s.seat_a = -1;
    s.seat_b = -1;
    return s;
}

const char *netplay_bios_token(int kind)
{
    switch (kind) {
    case NETPLAY_BIOS_OPENBIOS: return "openbios";
    case NETPLAY_BIOS_RETAIL:   return "scph1001";
    default:                    return "";
    }
}

void netplay_bios_format_crc(uint32_t crc, char *out, size_t cap)
{
    if (!out || !cap) return;
    if (!crc) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%08x", (unsigned)crc);
}

uint32_t netplay_bios_parse_crc(const char *text)
{
    uint32_t v = 0;
    int digits = 0;
    if (!text) return 0;
    while (*text == ' ' || *text == '\t') ++text;
    for (; *text; ++text) {
        const char c = *text;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        if (++digits > 8) return 0;
        v = (v << 4) | d;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    if (digits != 8 || *text) return 0;
    return v;
}

void netplay_bios_describe_crc(uint32_t crc, char *out, size_t cap)
{
    size_t i;
    if (!out || !cap) return;
    for (i = 0; i < sizeof(psx_known_bios_images) / sizeof(psx_known_bios_images[0]); ++i) {
        if (psx_known_bios_images[i].crc32 == crc) {
            snprintf(out, cap, "%s", psx_known_bios_images[i].id);
            return;
        }
    }
    snprintf(out, cap, "BIOS %08x", (unsigned)crc);
}

void netplay_bios_describe_refusal(const NetplayBiosSettle *s,
                                   const NetplayBiosSeat *seats, int count,
                                   char *out, size_t cap)
{
    char a[32], b[32];
    if (!out || !cap) return;
    out[0] = '\0';
    if (!s || s->kind != NETPLAY_BIOS_NONE) return;
    if (seats && s->seat_a >= 0 && s->seat_a < count &&
        s->seat_b >= 0 && s->seat_b < count) {
        netplay_bios_describe_crc(seats[s->seat_a].retail_crc, a, sizeof(a));
        netplay_bios_describe_crc(seats[s->seat_b].retail_crc, b, sizeof(b));
        snprintf(out, cap,
                 "Players use different BIOS images (%s and %s), and not "
                 "everyone has OpenBIOS. Use the same BIOS dump.", a, b);
        return;
    }
    snprintf(out, cap,
             "A player has no usable BIOS dump for this game (or an older "
             "build), and not everyone has OpenBIOS.");
}
