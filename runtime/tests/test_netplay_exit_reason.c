/* Netplay exit reasons and the boot-mismatch deadline (PS1B-290), and the
 * refusal of a seat that is not a pad (PS1B-313).
 * Plain checks, not assert(): Release test builds define NDEBUG. */
#include "netplay_exit_reason.h"
#include "sio.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    ++failures; } } while (0)

int main(void)
{
    static const char *const ended[] = {
        "netplay_boot_mismatch", "netplay_peer_disconnect", "netplay_load_failed",
        "netplay_load_stall", "netplay_link_stall", "netplay_admit_stall",
    };
    size_t i;

    /* Every origin netplay_soft_exit uses for a failure has a sentence, and it
     * fits the lobby client's last_error (192 bytes). */
    for (i = 0; i < sizeof(ended) / sizeof(ended[0]); ++i) {
        const char *t = netplay_exit_reason_text(ended[i]);
        CHECK(t != NULL);
        CHECK(t && strlen(t) > 20 && strlen(t) < 192);
    }
    CHECK(strstr(netplay_exit_reason_text("netplay_boot_mismatch"), "BIOS") != NULL);

    /* A player who closed the window or pressed Escape needs no reason. */
    CHECK(netplay_exit_reason_text("sdl_window_close") == NULL);
    CHECK(netplay_exit_reason_text("netplay_escape") == NULL);
    CHECK(netplay_exit_reason_text("netplay_barrier_escape") == NULL);
    CHECK(netplay_exit_reason_text("") == NULL);
    CHECK(netplay_exit_reason_text(NULL) == NULL);

    /* A start that failed says what failed (PS1B-386). */
    {
        char why[NETPLAY_START_FAILURE_CAP];
        static const char long_text[] =
            "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234";
        /* The port is in use: the port, the address and the system's error
         * are named, the player is told what to do, and the build is not
         * blamed. */
        netplay_start_failure_text(-3, 1, "127.0.0.1:47810", "127.0.0.1:47811",
                                   NETPLAY_BIND_FAILED, 10048, "in use", why, sizeof(why));
        CHECK(strstr(why, "UDP port 47810 on 127.0.0.1") != NULL);
        CHECK(strstr(why, "system error 10048") != NULL);
        CHECK(strstr(why, "Another program or the operating system holds that port") != NULL);
        CHECK(strstr(why, "Choose another port") != NULL);
        CHECK(strstr(why, "built without") == NULL && strstr(why, "recomp-net") == NULL);
        CHECK(strlen(why) < 192);
        /* The same cause has another number on Linux (98) and on macOS (48).
         * The longest port and address still fit last_error. */
        {
            static const int in_use[] = { 10048, 98, 48 };
            size_t k;
            for (k = 0; k < sizeof(in_use) / sizeof(in_use[0]); ++k) {
                netplay_start_failure_text(-3, 1, "255.255.255.255:65535", "", NETPLAY_BIND_FAILED,
                                           in_use[k], "", why, sizeof(why));
                CHECK(strstr(why, "UDP port 65535 on 255.255.255.255 (system error ") != NULL);
                CHECK(strstr(why, "holds that port. Choose another port and start again.") != NULL);
                CHECK(strlen(why) < 192);
            }
        }
        /* A LAN host's own port, any address; no address means any address. */
        netplay_start_failure_text(-3, 1, "0.0.0.0:7777", "", NETPLAY_BIND_FAILED, 98, NULL, why, sizeof(why));
        CHECK(strstr(why, "UDP port 7777 on 0.0.0.0 (system error 98)") != NULL);
        CHECK(strstr(why, "holds that port") != NULL);
        netplay_start_failure_text(-3, 1, ":7777", "", NETPLAY_BIND_FAILED, 98, NULL, why, sizeof(why));
        CHECK(strstr(why, "UDP port 7777 on 0.0.0.0 (system error 98)") != NULL);
        /* A port the system does not hand out (a range Windows keeps for
         * Hyper-V or WSL: 10013; a port below 1024 elsewhere: 13). The player
         * is not sent looking for another program. */
        {
            static const int denied[] = { 10013, 13 };
            size_t k;
            for (k = 0; k < sizeof(denied) / sizeof(denied[0]); ++k) {
                netplay_start_failure_text(-3, 1, "127.0.0.1:47810", "127.0.0.1:47811",
                                           NETPLAY_BIND_FAILED, denied[k], "denied", why, sizeof(why));
                CHECK(strstr(why, "UDP port 47810 on 127.0.0.1 (system error ") != NULL);
                CHECK(strstr(why, "The operating system does not allow this port.") != NULL);
                CHECK(strstr(why, "Choose another port and start again.") != NULL);
                CHECK(strstr(why, "Another program") == NULL && strstr(why, "holds") == NULL);
                CHECK(strstr(why, "built without") == NULL);
                CHECK(strlen(why) < 192);
            }
            CHECK(strstr(why, "(system error 13)") != NULL);
            /* The number alone does not decide: a port that could be bound is not "not allowed". */
            netplay_start_failure_text(-3, 1, "127.0.0.1:47810", "x", NETPLAY_BIND_OK, 10013, "", why, sizeof(why));
            CHECK(strstr(why, "does not allow") == NULL);
        }
        /* The listen address is not one of this computer's (Windows 10049,
         * Linux 99, macOS 49). No port is held and another port does not
         * help, so neither is said. */
        {
            static const int no_address[] = { 10049, 99, 49 };
            size_t k;
            for (k = 0; k < sizeof(no_address) / sizeof(no_address[0]); ++k) {
                netplay_start_failure_text(-3, 1, "192.168.1.50:7777", "", NETPLAY_BIND_FAILED,
                                           no_address[k], "not available", why, sizeof(why));
                CHECK(strstr(why, "Netplay could not listen on 192.168.1.50 (system error ") != NULL);
                CHECK(strstr(why, "That address is not one of this computer's addresses.") != NULL);
                CHECK(strstr(why, "Check the listen address and start again.") != NULL);
                CHECK(strstr(why, "holds") == NULL && strstr(why, "another port") == NULL);
                CHECK(strstr(why, "does not allow") == NULL);
            }
            CHECK(strstr(why, "(system error 49)") != NULL);
            netplay_start_failure_text(-3, 1, "255.255.255.255:65535", "", NETPLAY_BIND_FAILED,
                                       10049, "", why, sizeof(why));
            CHECK(strlen(why) < 192 && strstr(why, "start again.") != NULL);
        }
        /* Any other answer of the system: the number and the system's text,
         * and no cause and no advice the code cannot know. */
        netplay_start_failure_text(-3, 1, "10.0.0.1:7777", "", NETPLAY_BIND_FAILED, 101,
                                   "Network is unreachable", why, sizeof(why));
        CHECK(strcmp(why, "Netplay could not open UDP port 7777 on 10.0.0.1 "
                          "(system error 101: Network is unreachable).") == 0);
        CHECK(strstr(why, "holds") == NULL && strstr(why, "Choose") == NULL && strstr(why, "does not allow") == NULL);
        /* Without a text, a text in another alphabet or a long text: the number alone. */
        netplay_start_failure_text(-3, 1, "10.0.0.1:7777", "", NETPLAY_BIND_FAILED, 101, NULL, why, sizeof(why));
        CHECK(strcmp(why, "Netplay could not open UDP port 7777 on 10.0.0.1 (system error 101).") == 0);
        netplay_start_failure_text(-3, 1, "10.0.0.1:7777", "", NETPLAY_BIND_FAILED, 10055,
                                   "\xcc\xe7 \xe4\xe9\xe1\xe8\xdd\xf3\xe9\xec\xef", why, sizeof(why));
        CHECK(strcmp(why, "Netplay could not open UDP port 7777 on 10.0.0.1 (system error 10055).") == 0);
        netplay_start_failure_text(-3, 1, "10.0.0.1:7777", "", NETPLAY_BIND_FAILED, 10055, long_text, why, sizeof(why));
        CHECK(strcmp(why, "Netplay could not open UDP port 7777 on 10.0.0.1 (system error 10055).") == 0);
        /* The longest values with the longest text that is shown (80 letters) fit. */
        netplay_start_failure_text(-3, 1, "255.255.255.255:65535", "", NETPLAY_BIND_FAILED, -2147483647,
                                   long_text + 5, why, sizeof(why));
        CHECK(strlen(why) < 192 && strstr(why, "01234).") != NULL);
        /* The system made no socket: no port and no address is blamed. */
        netplay_start_failure_text(-3, 1, "0.0.0.0:7777", "", NETPLAY_BIND_NO_SOCKET, 24,
                                   "Too many open files", why, sizeof(why));
        CHECK(strcmp(why, "Netplay could not open a network socket (system error 24: Too many open files).") == 0);
        /* The build is named only when the build has no netplay. */
        netplay_start_failure_text(-1, 0, "127.0.0.1:47810", "", NETPLAY_BIND_NOT_TRIED, 0, "", why, sizeof(why));
        CHECK(strstr(why, "This build has no netplay") != NULL);
        CHECK(strstr(why, "port") == NULL);
        /* The listen address opened and the peer address cannot be read: the
         * peer address is the cause. */
        netplay_start_failure_text(-3, 1, "127.0.0.1:47810", "not an address", NETPLAY_BIND_PEER_BAD, 0, "",
                                   why, sizeof(why));
        CHECK(strstr(why, "the other player's address \"not an address\"") != NULL);
        CHECK(strstr(why, "holds that port") == NULL);
        /* The peer address is not blamed when nothing was tried, or when the
         * listen address opened and the peer address reads well: no cause is
         * known then. */
        netplay_start_failure_text(-3, 1, "localhost:40010", "127.0.0.1:40011", NETPLAY_BIND_NOT_TRIED, 0, "",
                                   why, sizeof(why));
        CHECK(strcmp(why, "Netplay could not start its connection on localhost:40010.") == 0);
        netplay_start_failure_text(-3, 1, "127.0.0.1:40010", "127.0.0.1:40011", NETPLAY_BIND_OK, 0, "",
                                   why, sizeof(why));
        CHECK(strcmp(why, "Netplay could not start its connection on 127.0.0.1:40010.") == 0);
        CHECK(strstr(why, "other player") == NULL);
        /* A listen address that is not address:port. */
        netplay_start_failure_text(-3, 1, "47810", "", NETPLAY_BIND_BAD_ADDRESS, 0, "", why, sizeof(why));
        CHECK(strstr(why, "\"47810\" is not an address and a port") != NULL);
        /* A host waiting for the first peer (no peer address), port free. */
        netplay_start_failure_text(-3, 1, "0.0.0.0:7777", "", NETPLAY_BIND_OK, 0, "", why, sizeof(why));
        CHECK(strstr(why, "its connection on 0.0.0.0:7777") != NULL);
        /* The run report holds no address (PS1G-63): players send that file
         * to other people, and the other player's address is another
         * person's. The report's sentence is the same words with each address
         * as one fixed word. Checked for every cause and every way the system
         * answers, with a numeric address, a host name and a typed text that
         * is no address. */
        {
            static const char *const listen[] = {
                "203.0.113.77:47810", "listen-host.example:47810", "my lan pc", "203.0.113.77:port" };
            static const char *const peer[] = { "198.51.100.23:47811", "peer-host.example:47811", "friend's pc", "" };
            static const int probes[] = { NETPLAY_BIND_NOT_TRIED, NETPLAY_BIND_OK, NETPLAY_BIND_FAILED,
                                          NETPLAY_BIND_BAD_ADDRESS, NETPLAY_BIND_NO_SOCKET, NETPLAY_BIND_PEER_BAD };
            static const int errors[] = { 0, 10048, 98, 10013, 13, 10049, 99, 101 };
            static const int codes[] = { -3, -4, -1 };
            static const char *const never[] = {
                "203.0.113", "198.51.100", "listen-host", "peer-host", "example", "my lan pc", "friend", ":port" };
            char report[NETPLAY_START_FAILURE_CAP];
            size_t a, b, c, d, e, n;
            for (a = 0; a < 4; ++a) for (b = 0; b < 4; ++b) for (c = 0; c < 6; ++c)
            for (d = 0; d < 8; ++d) for (e = 0; e < 3; ++e) {
                netplay_start_failure_report_text(codes[e], 1, listen[a], peer[b], probes[c], errors[d],
                                                  "in use", report, sizeof(report));
                for (n = 0; n < sizeof(never) / sizeof(never[0]); ++n)
                    if (strstr(report, never[n])) {
                        fprintf(stderr, "address in the report: [%s] holds [%s]\n", report, never[n]);
                        ++failures;
                    }
                CHECK(report[0] != '\0' && strlen(report) < NETPLAY_START_FAILURE_CAP);
            }
            /* The words stay, and the port stays when it is a plain number. */
            netplay_start_failure_report_text(-3, 1, "203.0.113.77:47810", "198.51.100.23:47811",
                                              NETPLAY_BIND_FAILED, 10048, "in use", report, sizeof(report));
            CHECK(strcmp(report, "Netplay could not open UDP port 47810 on (address) (system error 10048). "
                                 "Another program or the operating system holds that port. "
                                 "Choose another port and start again.") == 0);
            netplay_start_failure_report_text(-3, 1, "203.0.113.77:47810", "", NETPLAY_BIND_FAILED, 10049,
                                              "", report, sizeof(report));
            CHECK(strstr(report, "Netplay could not listen on (address) (system error 10049).") != NULL);
            netplay_start_failure_report_text(-3, 1, "listen-host.example:40010", "198.51.100.23:40011",
                                              NETPLAY_BIND_OK, 0, "", report, sizeof(report));
            CHECK(strcmp(report, "Netplay could not start its connection on (address):40010.") == 0);
            netplay_start_failure_report_text(-3, 1, "127.0.0.1:47810", "friend's pc", NETPLAY_BIND_PEER_BAD,
                                              0, "", report, sizeof(report));
            CHECK(strstr(report, "the other player's address \"(address)\" could not be used.") != NULL);
            netplay_start_failure_report_text(-3, 1, "my lan pc", "", NETPLAY_BIND_BAD_ADDRESS, 0, "",
                                              report, sizeof(report));
            CHECK(strstr(report, "\"(address)\" is not an address and a port to listen on.") != NULL);
            /* The sentence for the box and the log is not changed by this. */
            netplay_start_failure_text(-3, 1, "203.0.113.77:47810", "", NETPLAY_BIND_FAILED, 10048, "in use",
                                       why, sizeof(why));
            CHECK(strstr(why, "UDP port 47810 on 203.0.113.77") != NULL);
        }
        /* Online (relay): five causes share the code, so none is named and
         * the player is not sent to a log a released game does not show. */
        netplay_start_failure_text(-4, 1, "0.0.0.0:0", "", NETPLAY_BIND_NOT_TRIED, 0, "", why, sizeof(why));
        CHECK(strcmp(why, "Netplay could not start the online connection.") == 0);
        /* Any other code. */
        netplay_start_failure_text(-2, 1, "", "", NETPLAY_BIND_NOT_TRIED, 0, "", why, sizeof(why));
        CHECK(strstr(why, "(code -2)") != NULL);
        /* Missing strings and a short buffer are safe. */
        netplay_start_failure_text(-3, 1, NULL, NULL, NETPLAY_BIND_FAILED, 5, NULL, why, sizeof(why));
        CHECK(strlen(why) > 20 && strlen(why) < 192);
        memset(why, 'x', sizeof(why));
        netplay_start_failure_text(-3, 1, "127.0.0.1:47810", "", NETPLAY_BIND_FAILED, 10048, "", why, 8);
        CHECK(strlen(why) == 7 && why[8] == 'x');
        netplay_start_failure_text(-3, 1, "127.0.0.1:47810", "", NETPLAY_BIND_FAILED, 10048, "", NULL, 0);
    }

    /* No mismatch seen: never final. */
    CHECK(!netplay_boot_mismatch_final(0u, 100000u, NETPLAY_BOOT_MISMATCH_GRACE_MS));
    /* Seen, grace not yet over, then over. */
    CHECK(!netplay_boot_mismatch_final(1000u, 3999u, NETPLAY_BOOT_MISMATCH_GRACE_MS));
    CHECK(netplay_boot_mismatch_final(1000u, 4000u, NETPLAY_BOOT_MISMATCH_GRACE_MS));
    /* The 32-bit millisecond clock wraps. */
    CHECK(!netplay_boot_mismatch_final(0xFFFFFF00u, 0x00000100u, NETPLAY_BOOT_MISMATCH_GRACE_MS));
    CHECK(netplay_boot_mismatch_final(0xFFFFFF00u, 0x00000C00u, NETPLAY_BOOT_MISMATCH_GRACE_MS));
    /* The old behaviour was a 20 s admit-stall watchdog; the grace is far
     * shorter than that. */
    CHECK(NETPLAY_BOOT_MISMATCH_GRACE_MS < 20000u);

    /* A pad seat is unchanged: no refusal and no text, on any port. */
    {
        char why[NETPLAY_SEAT_REFUSAL_CAP];
        static const struct { int device; const char *name; } other[] = {
            { SIO_DEVICE_MOUSE, "PS1 Mouse" }, { SIO_DEVICE_NEGCON, "neGcon" },
            { SIO_DEVICE_GUNCON, "GunCon" },
        };
        memset(why, 'x', sizeof(why));
        CHECK(netplay_seat_refusal(SIO_DEVICE_PAD, 1, why, sizeof(why)) == 0);
        CHECK(why[0] == '\0');
        CHECK(netplay_seat_refusal(SIO_DEVICE_PAD, 2, why, sizeof(why)) == 0);
        /* A Mouse, a neGcon and a GunCon are refused with one sentence that
         * names the device and the port and fits last_error (192 bytes). */
        for (i = 0; i < sizeof(other) / sizeof(other[0]); ++i) {
            CHECK(netplay_seat_refusal(other[i].device, 2, why, sizeof(why)) == 1);
            CHECK(strstr(why, other[i].name) != NULL);
            CHECK(strstr(why, "port 2") != NULL);
            CHECK(strstr(why, "pad or the keyboard") != NULL);
            CHECK(strlen(why) > 20 && strlen(why) < 192);
        }
        CHECK(netplay_seat_refusal(SIO_DEVICE_MOUSE, 1, why, sizeof(why)) == 1);
        CHECK(strstr(why, "port 1 is set to a PS1 Mouse") != NULL);
        /* A device kind added later is refused until netplay can carry it. */
        CHECK(netplay_seat_refusal(99, 1, why, sizeof(why)) == 1);
        CHECK(strstr(why, "port 1") != NULL);
        /* The verdict does not need a buffer, and a short one is not overrun. */
        CHECK(netplay_seat_refusal(SIO_DEVICE_GUNCON, 1, NULL, 0) == 1);
        memset(why, 'x', sizeof(why));
        CHECK(netplay_seat_refusal(SIO_DEVICE_GUNCON, 1, why, 8) == 1);
        CHECK(strlen(why) == 7 && why[8] == 'x');
    }

    /* A match boots with the kit's BIOS setting (PS1B-382). */
    {
        static const NetplayBootMode real_bios = { 0, 0, 0 };
        static const NetplayBootMode shortcut = { 1, 0, 0 };
        NetplayBootMode mode;
        /* A player on the kit's value: nothing changes and nothing is said. */
        mode = real_bios;
        CHECK(netplay_boot_mode_settle(&real_bios, &mode) == 0);
        CHECK(mode.bios_hle == 0 && mode.keep_intro == 0 && mode.fast_boot == 0);
        mode = shortcut;
        CHECK(netplay_boot_mode_settle(&shortcut, &mode) == 0);
        CHECK(mode.bios_hle == 1);
        /* The stale settings file: the kit says the real BIOS, the player's
         * file still says the shortcut. The match takes the kit's value. */
        mode = shortcut;
        CHECK(netplay_boot_mode_settle(&real_bios, &mode) == 1);
        CHECK(mode.bios_hle == 0 && mode.keep_intro == 0 && mode.fast_boot == 0);
        /* The other way round, and each of the other two inputs alone. */
        mode = real_bios;
        CHECK(netplay_boot_mode_settle(&shortcut, &mode) == 1);
        CHECK(mode.bios_hle == 1);
        mode = real_bios; mode.fast_boot = 1;
        CHECK(netplay_boot_mode_settle(&real_bios, &mode) == 1);
        CHECK(mode.fast_boot == 0);
        mode = shortcut; mode.keep_intro = 1;
        CHECK(netplay_boot_mode_settle(&shortcut, &mode) == 1);
        CHECK(mode.bios_hle == 1 && mode.keep_intro == 0);
        /* On is on: 2 and 1 are the same setting. */
        mode.bios_hle = 2; mode.keep_intro = 0; mode.fast_boot = 0;
        CHECK(netplay_boot_mode_settle(&shortcut, &mode) == 0);
        CHECK(mode.bios_hle == 1);
        /* Two peers with different files end on the same three values. */
        {
            NetplayBootMode peer_a = { 1, 0, 1 }, peer_b = { 0, 1, 0 };
            (void)netplay_boot_mode_settle(&real_bios, &peer_a);
            (void)netplay_boot_mode_settle(&real_bios, &peer_b);
            CHECK(memcmp(&peer_a, &peer_b, sizeof(peer_a)) == 0);
        }
        CHECK(netplay_boot_mode_settle(NULL, &mode) == 0);
        CHECK(netplay_boot_mode_settle(&real_bios, NULL) == 0);
        /* The sentence fits a toast and names what the match uses. */
        CHECK(strlen(NETPLAY_BOOT_MODE_NOTICE) < 64);
        CHECK(strstr(NETPLAY_BOOT_MODE_NOTICE, "BIOS setting") != NULL);
    }

    if (failures) {
        fprintf(stderr, "netplay_exit_reason: %d check(s) FAILED\n", failures);
        return 1;
    }
    puts("netplay_exit_reason: PASS");
    return 0;
}
