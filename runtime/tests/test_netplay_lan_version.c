/* ctest -R netplay_lan_version_test
 * The LAN / Direct IP build check (PS1B-295).
 * Plain checks, not assert(): Release test builds define NDEBUG. */
#include "netplay_lan_version.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    ++failures; } } while (0)

static const char *version_of(const char *tail)
{
    static char out[32];
    memset(out, 'x', sizeof(out));
    netplay_lan_join_version(tail, out, sizeof(out));
    return out;
}

int main(void)
{
    char small[6];

    /* A JOIN tail as the guest sends it: prefer, can_openbios, can_retail,
     * has_card, share, BIOS CRC, version. */
    CHECK(strcmp(version_of("scph1001\n0\n1\n1\n0\n37157331\n0.1.2-6ef86cae\n"), "0.1.2-6ef86cae") == 0);
    CHECK(strcmp(version_of("openbios\n1\n0\n0\n0\n\ndev\n"), "dev") == 0);
    /* No trailing newline, and a CR from a hand-typed peer. */
    CHECK(strcmp(version_of("scph1001\n0\n1\n1\n0\n37157331\n0.1.2-6ef86cae"), "0.1.2-6ef86cae") == 0);
    CHECK(strcmp(version_of("scph1001\n0\n1\n1\n0\n37157331\n0.1.2-6ef86cae\r\n"), "0.1.2-6ef86cae") == 0);
    /* Guests from before the check: tail ends at the CRC, the memory-card
     * lines or the BIOS lines, or there is no tail. */
    CHECK(strcmp(version_of("scph1001\n0\n1\n1\n0\n37157331\n"), "") == 0);
    CHECK(strcmp(version_of("scph1001\n0\n1\n1\n0\n"), "") == 0);
    CHECK(strcmp(version_of("scph1001\n0\n1\n"), "") == 0);
    CHECK(strcmp(version_of(""), "") == 0);
    CHECK(strcmp(version_of(NULL), "") == 0);
    /* A long line is cut to the buffer and stays terminated. */
    netplay_lan_join_version("a\nb\nc\nd\ne\nf\n0123456789\n", small, sizeof(small));
    CHECK(strcmp(small, "01234") == 0);
    netplay_lan_join_version("a\nb\nc\nd\ne\nf\n0123456789\n", small, 0);
    netplay_lan_join_version("a\nb\nc\nd\ne\nf\n0123456789\n", NULL, 8);

    /* Builds of one release meet. */
    CHECK(netplay_lan_version_ok("0.1.2-6ef86cae", "0.1.2-6ef86cae"));
    /* Another framework commit, another kit version, a release against dev. */
    CHECK(!netplay_lan_version_ok("0.1.2-6ef86cae", "0.1.2-a3e5fd89"));
    CHECK(!netplay_lan_version_ok("0.1.2-6ef86cae", "0.1.3-6ef86cae"));
    CHECK(!netplay_lan_version_ok("0.1.2-6ef86cae", "0.1.2"));
    CHECK(!netplay_lan_version_ok("0.1.2-6ef86cae", "dev"));
    CHECK(!netplay_lan_version_ok("dev", "0.1.2-6ef86cae"));
    /* A guest from before the check counts as dev: a release host refuses it,
     * a dev host takes it as before. */
    CHECK(!netplay_lan_version_ok("0.1.2-6ef86cae", ""));
    CHECK(!netplay_lan_version_ok("0.1.2-6ef86cae", NULL));
    CHECK(netplay_lan_version_ok("dev", ""));
    CHECK(netplay_lan_version_ok("dev", NULL));
    CHECK(netplay_lan_version_ok("dev", "dev"));
    CHECK(netplay_lan_version_ok("", ""));
    CHECK(netplay_lan_version_ok(NULL, "dev"));
    /* Surrounding blanks do not make two versions differ; a prefix does. */
    CHECK(netplay_lan_version_ok("0.1.2-6ef86cae", " 0.1.2-6ef86cae \r"));
    CHECK(!netplay_lan_version_ok("0.1.2-6ef86cae", "0.1.2-6ef86ca"));
    CHECK(!netplay_lan_version_ok("0.1.2-6ef86ca", "0.1.2-6ef86cae"));

    /* Both sentences fit the lobby client's last_error (192 bytes). */
    CHECK(strlen(NETPLAY_LAN_VERSION_GUEST_TEXT) < 192);
    CHECK(strlen(NETPLAY_LAN_VERSION_HOST_TEXT) < 192);

    if (failures) {
        fprintf(stderr, "netplay_lan_version_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("netplay_lan_version_test: passed\n");
    return 0;
}
