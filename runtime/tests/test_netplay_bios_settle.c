/* Netplay BIOS settle rule and its wire helpers.
 * Plain checks, not assert(): Release test builds define NDEBUG. */
#include "netplay_bios_settle.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    ++failures; } } while (0)

enum { SCPH1001 = 0x37157331u, SCPH5552 = 0xD786F0B9u };

/* A seat with a retail dump; OpenBIOS as given. */
static NetplayBiosSeat retail(uint32_t crc, int can_open, int host)
{
    NetplayBiosSeat s;
    memset(&s, 0, sizeof(s));
    s.offered = 1;
    s.can_openbios = can_open;
    s.can_retail = 1;
    s.retail_crc = crc;
    s.is_host = host;
    return s;
}

static NetplayBiosSeat openbios_only(int prefer, int host)
{
    NetplayBiosSeat s;
    memset(&s, 0, sizeof(s));
    s.offered = 1;
    s.can_openbios = 1;
    s.prefer_openbios = prefer;
    s.is_host = host;
    return s;
}

int main(void)
{
    NetplayBiosSeat seats[3];
    NetplayBiosSettle r;
    char text[256];

    /* The 09-16 defect: SCPH-5552 host, SCPH-1001 guest, both link OpenBIOS.
     * Before the fix both booted their own dump. Now: OpenBIOS. */
    seats[0] = retail(SCPH5552, 1, 1);
    seats[1] = retail(SCPH1001, 1, 0);
    r = netplay_bios_settle(seats, 2);
    CHECK(r.kind == NETPLAY_BIOS_OPENBIOS);
    CHECK(strcmp(netplay_bios_token(r.kind), "openbios") == 0);

    /* Same pair, but builds that link one retail backend and no OpenBIOS:
     * no BIOS both can boot, so the host must refuse, naming both images. */
    seats[0] = retail(SCPH5552, 0, 1);
    seats[1] = retail(SCPH1001, 0, 0);
    r = netplay_bios_settle(seats, 2);
    CHECK(r.kind == NETPLAY_BIOS_NONE);
    CHECK(r.seat_a == 0 && r.seat_b == 1);
    netplay_bios_describe_refusal(&r, seats, 2, text, sizeof(text));
    CHECK(strstr(text, "SCPH-5552") != NULL);
    CHECK(strstr(text, "SCPH-1001") != NULL);

    /* EU vs EU without OpenBIOS works today and must keep working. */
    seats[0] = retail(SCPH5552, 0, 1);
    seats[1] = retail(SCPH5552, 0, 0);
    r = netplay_bios_settle(seats, 2);
    CHECK(r.kind == NETPLAY_BIOS_RETAIL);
    CHECK(r.retail_crc == SCPH5552);
    CHECK(strcmp(netplay_bios_token(r.kind), "scph1001") == 0);

    /* US vs US with OpenBIOS linked: retail, as before. */
    seats[0] = retail(SCPH1001, 1, 1);
    seats[1] = retail(SCPH1001, 1, 0);
    r = netplay_bios_settle(seats, 2);
    CHECK(r.kind == NETPLAY_BIOS_RETAIL && r.retail_crc == SCPH1001);

    /* A guest prefers OpenBIOS; the host's retail pick still wins. */
    seats[1].prefer_openbios = 1;
    r = netplay_bios_settle(seats, 2);
    CHECK(r.kind == NETPLAY_BIOS_RETAIL && r.retail_crc == SCPH1001);

    /* The host prefers OpenBIOS: OpenBIOS. */
    seats[0].prefer_openbios = 1;
    seats[1].prefer_openbios = 0;
    r = netplay_bios_settle(seats, 2);
    CHECK(r.kind == NETPLAY_BIOS_OPENBIOS);

    /* Three seats, nobody prefers OpenBIOS: retail when every image matches. */
    seats[0] = retail(SCPH1001, 1, 1);
    seats[1] = retail(SCPH1001, 1, 0);
    seats[2] = retail(SCPH1001, 1, 0);
    r = netplay_bios_settle(seats, 3);
    CHECK(r.kind == NETPLAY_BIOS_RETAIL);

    /* One seat has only OpenBIOS: OpenBIOS for everyone who links it. */
    seats[2] = openbios_only(1, 0);
    r = netplay_bios_settle(seats, 3);
    CHECK(r.kind == NETPLAY_BIOS_OPENBIOS);

    /* An older peer sends no CRC: retail is unsafe, OpenBIOS if all link it. */
    seats[0] = retail(SCPH1001, 1, 1);
    seats[1] = retail(0, 1, 0);
    r = netplay_bios_settle(seats, 2);
    CHECK(r.kind == NETPLAY_BIOS_OPENBIOS);
    /* ...and a refusal when the host cannot run OpenBIOS. */
    seats[0].can_openbios = 0;
    r = netplay_bios_settle(seats, 2);
    CHECK(r.kind == NETPLAY_BIOS_NONE);
    CHECK(r.seat_a == 1 && r.seat_b == -1);
    netplay_bios_describe_refusal(&r, seats, 2, text, sizeof(text));
    CHECK(strstr(text, "no usable BIOS") != NULL);

    /* A seat that sent no offer at all is an older client: OpenBIOS. */
    memset(&seats[1], 0, sizeof(seats[1]));
    seats[0] = retail(SCPH1001, 1, 1);
    r = netplay_bios_settle(seats, 2);
    CHECK(r.kind == NETPLAY_BIOS_OPENBIOS);

    /* Every seat prefers OpenBIOS but one cannot run it: retail if possible. */
    seats[0] = retail(SCPH5552, 0, 1);
    seats[1] = retail(SCPH5552, 1, 0);
    seats[1].prefer_openbios = 1;
    r = netplay_bios_settle(seats, 2);
    CHECK(r.kind == NETPLAY_BIOS_RETAIL && r.retail_crc == SCPH5552);

    /* No seats: OpenBIOS, the old default. */
    r = netplay_bios_settle(seats, 0);
    CHECK(r.kind == NETPLAY_BIOS_OPENBIOS);
    CHECK(strcmp(netplay_bios_token(NETPLAY_BIOS_NONE), "") == 0);

    /* Wire helpers. */
    netplay_bios_format_crc(SCPH5552, text, sizeof(text));
    CHECK(strcmp(text, "d786f0b9") == 0);
    CHECK(netplay_bios_parse_crc(text) == SCPH5552);
    CHECK(netplay_bios_parse_crc("D786F0B9") == SCPH5552);
    CHECK(netplay_bios_parse_crc(" 37157331\n") == SCPH1001);
    netplay_bios_format_crc(0, text, sizeof(text));
    CHECK(text[0] == '\0');
    CHECK(netplay_bios_parse_crc("") == 0);
    CHECK(netplay_bios_parse_crc(NULL) == 0);
    CHECK(netplay_bios_parse_crc("1234567") == 0);    /* too short */
    CHECK(netplay_bios_parse_crc("123456789") == 0);  /* too long */
    CHECK(netplay_bios_parse_crc("scph1001") == 0);   /* a token, not a CRC */
    netplay_bios_describe_crc(SCPH1001, text, sizeof(text));
    CHECK(strcmp(text, "SCPH-1001") == 0);
    netplay_bios_describe_crc(0x12345678u, text, sizeof(text));
    CHECK(strcmp(text, "BIOS 12345678") == 0);

    if (failures) {
        fprintf(stderr, "netplay_bios_settle: %d check(s) FAILED\n", failures);
        return 1;
    }
    puts("netplay_bios_settle: PASS");
    return 0;
}
