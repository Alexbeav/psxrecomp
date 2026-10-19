/* Exercise the actual lobby parser/serializer and seat reconciliation.
 * No server or sockets: link the same real dependencies as chat-ring tests. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <string.h>
#include "../src/psx_lobby_client.c"

uint64_t psx_host_mono_ms(void) { return 1; }
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void wire_roundtrip(void) {
    PsxLobbyMatchCaps before, after;
    char wire[2048], object[1024];
    memset(&before, 0, sizeof(before));
    before.valid = 1;
    before.aspect_num = 16;
    before.aspect_den = 9;
    before.rollback = 1;
    strcpy(before.language, "it");
    strcpy(before.session_bios, "scph1001");
    before.session_bios_crc = 0xD786F0B9u;
    CHECK(append_match_caps_json(wire, sizeof(wire), &before) > 0);
    CHECK(json_extract_object(wire, "match_caps", object, sizeof(object)));
    parse_match_caps_object(object, &after);
    CHECK(after.valid && after.session_bios_crc == before.session_bios_crc);
    CHECK(after.rollback == 1 && after.aspect_num == 16 && after.aspect_den == 9);
    CHECK(strcmp(after.language, "it") == 0);
    parse_match_caps_object("{\"session_bios\":\"scph1001\"}", &after);
    CHECK(after.valid && after.session_bios_crc == 0); /* legacy host */
    parse_match_caps_object("{\"session_bios\":\"openbios\",\"session_bios_crc\":\"d786f0b9\"}", &after);
    CHECK(after.session_bios_crc == 0); /* irrelevant retail CRC ignored */
    parse_match_caps_object("{\"session_bios\":\"scph1001\",\"session_bios_crc\":\"invalid\"}", &after);
    CHECK(after.session_bios_crc == 0);
}

static void local_offer_wins(void) {
    char token[16], why[192];
    uint32_t crc;
    memset(&g_lc, 0, sizeof(g_lc));
    strcpy(g_lc.player_id, "me");
    g_lc.is_host = 1;
    g_lc.member_count = 2;
    strcpy(g_lc.members[0].player_id, "me");
    strcpy(g_lc.members[1].player_id, "peer");
    for (int i = 0; i < 2; ++i) {
        g_lc.members[i].bios_offer_valid = 1;
        g_lc.members[i].bios_can_scph1001 = 1;
        g_lc.members[i].bios_retail_crc = 0x37157331u;
    }
    /* The local file changed after the stale server echo, now matching peer. */
    g_lc.members[0].bios_retail_crc = 0xD786F0B9u;
    g_lc.bios_offer.valid = 1;
    g_lc.bios_offer.can_scph1001 = 1;
    g_lc.bios_offer.retail_crc = 0x37157331u;
    CHECK(psx_lobby_settle_session_bios(token, sizeof(token), &crc, why, sizeof(why)) == 0);
    CHECK(strcmp(token, "scph1001") == 0 && crc == 0x37157331u && !why[0]);
    g_lc.members[1].bios_retail_crc = 0xD786F0B9u;
    CHECK(psx_lobby_settle_session_bios(token, sizeof(token), &crc, why, sizeof(why)) == 1);
    CHECK(!token[0] && crc == 0 && strstr(why, "SCPH-1001") && strstr(why, "SCPH-5552"));
    g_lc.bios_offer.can_openbios = 1;
    g_lc.members[1].bios_can_openbios = 1;
    CHECK(psx_lobby_settle_session_bios(token, sizeof(token), &crc, why, sizeof(why)) == 0);
    CHECK(strcmp(token, "openbios") == 0 && crc == 0);
}

static void exit_message_bounds(void) {
    char long_message[512];
    memset(long_message, 'x', sizeof(long_message));
    long_message[sizeof(long_message)-1] = 0;
    psx_lobby_set_last_error(long_message);
    CHECK(strlen(g_lc.join.last_error) == sizeof(g_lc.join.last_error)-1);
    psx_lobby_set_last_error(NULL);
    CHECK(!g_lc.join.last_error[0]);
}

int main(void) {
    wire_roundtrip();
    local_offer_wins();
    exit_message_bounds();
    if (failures) return 1;
    puts("netplay BIOS wire/local-offer regression: PASS");
    return 0;
}
