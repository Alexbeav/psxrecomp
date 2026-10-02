/* Why a start ended before the first frame, as the run report prints it
 * (PS1G-63). Plain checks, not assert(): Release test builds define NDEBUG. */
#include "start_refusal.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    ++failures; } } while (0)

int main(void)
{
    char out[4096];
    int n;

    /* A start that ran: nothing recorded, and the report gets `null`. */
    psx_start_refusal_reset();
    CHECK(psx_start_refusal_kind() == NULL);
    n = psx_start_refusal_json(out, sizeof(out));
    CHECK(n == 4 && strcmp(out, "null") == 0);
    n = psx_start_launcher_status_json(out, sizeof(out));
    CHECK(n == 4 && strcmp(out, "null") == 0);

    /* A refusal: the kind and the sentence the player was shown. A line break
     * in the sentence is \n in the report, a quote and a backslash are
     * escaped, and any other control character is a space, so the report
     * stays one valid JSON document whatever a path contains. */
    psx_start_refusal_set("no_bios", "BIOS Mismatch",
        "That BIOS (524288 bytes, CRC32 318178BF) is not an image this build "
        "was compiled from.\r\n\nC:\\Games\\\"PS1\"\\bios.bin\ttab");
    CHECK(psx_start_refusal_kind() != NULL &&
          strcmp(psx_start_refusal_kind(), "no_bios") == 0);
    n = psx_start_refusal_json(out, sizeof(out));
    CHECK(n == (int)strlen(out));
    CHECK(strcmp(out,
        "{\"kind\": \"no_bios\", \"title\": \"BIOS Mismatch\", \"message\": "
        "\"That BIOS (524288 bytes, CRC32 318178BF) is not an image this build "
        "was compiled from.\\n\\nC:\\\\Games\\\\\\\"PS1\\\"\\\\bios.bin tab\"}") == 0);

    /* The first refusal of a start is kept: a later exit path must not
     * replace the reason. */
    psx_start_refusal_set("video_init", "Game window could not be opened", "later");
    CHECK(strcmp(psx_start_refusal_kind(), "no_bios") == 0);

    /* No kind, no refusal. */
    psx_start_refusal_reset();
    psx_start_refusal_set(NULL, "t", "m");
    psx_start_refusal_set("", "t", "m");
    CHECK(psx_start_refusal_kind() == NULL);

    /* A missing title or sentence is an empty string, not a crash. */
    psx_start_refusal_set("already_running", NULL, NULL);
    psx_start_refusal_json(out, sizeof(out));
    CHECK(strcmp(out, "{\"kind\": \"already_running\", \"title\": \"\", \"message\": \"\"}") == 0);

    /* A sentence longer than the cap is cut, and the value is still a
     * complete JSON object. */
    {
        static char longtext[4 * PSX_START_REFUSAL_TEXT_CAP];
        memset(longtext, 'a', sizeof(longtext) - 1);
        longtext[sizeof(longtext) - 1] = '\0';
        psx_start_refusal_reset();
        psx_start_refusal_set("config_unreadable", "Configuration could not be read", longtext);
        n = psx_start_refusal_json(out, sizeof(out));
        CHECK(n > PSX_START_REFUSAL_TEXT_CAP - 8 && n < PSX_START_REFUSAL_TEXT_CAP + 160);
        CHECK(out[0] == '{' && out[n - 1] == '}' && out[n - 2] == '"');
    }

    /* A buffer too small for the object gets `null`, never half an object. */
    {
        char small[24];
        n = psx_start_refusal_json(small, sizeof(small));
        CHECK(n == 4 && strcmp(small, "null") == 0);
        CHECK(psx_start_refusal_json(small, 0) == 0);
        CHECK(psx_start_refusal_json(NULL, 16) == 0);
    }

    /* The launcher's rows: the last text of each, and only those two rows. */
    psx_start_refusal_reset();
    psx_start_note_launcher("bios", "CRC32 318178BF (this build expects SCPH-5552, CRC32 D786F0B9).");
    psx_start_note_launcher("disc", "first");
    psx_start_note_launcher("disc", "verdict=refused serial=SLES-02913 expected=SLES-01156 tracks=4");
    psx_start_note_launcher("memcard", "ignored");
    psx_start_note_launcher(NULL, "ignored");
    n = psx_start_launcher_status_json(out, sizeof(out));
    CHECK(n == (int)strlen(out));
    CHECK(strcmp(out,
        "{\"bios\": \"CRC32 318178BF (this build expects SCPH-5552, CRC32 D786F0B9).\", "
        "\"disc\": \"verdict=refused serial=SLES-02913 expected=SLES-01156 tracks=4\"}") == 0);
    /* The rows do not make a start refused. */
    CHECK(psx_start_refusal_kind() == NULL);
    /* One row alone is still reported; an empty text clears a row. */
    psx_start_note_launcher("bios", "");
    psx_start_launcher_status_json(out, sizeof(out));
    CHECK(strncmp(out, "{\"bios\": \"\", \"disc\": \"verdict=refused", 36) == 0);
    psx_start_note_launcher("disc", NULL);
    psx_start_launcher_status_json(out, sizeof(out));
    CHECK(strcmp(out, "null") == 0);

    if (failures) {
        fprintf(stderr, "start refusal: %d check(s) failed\n", failures);
        return 1;
    }
    printf("start refusal: ok\n");
    return 0;
}
