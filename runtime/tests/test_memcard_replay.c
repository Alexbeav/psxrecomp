/* PS1B-316: a power-on replay plays on the memory cards it recorded, and the
 * player's card files are neither read nor written while it does. */
#include "memcard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint8_t file_image[MEMCARD_SIZE];

static int read_file(const char *path, uint8_t *out) {
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(out, 1, MEMCARD_SIZE, f) : 0;
    if (f) fclose(f);
    return n == MEMCARD_SIZE;
}

int main(int argc, char **argv) {
    static uint8_t replay[2 * MEMCARD_SIZE], player1[MEMCARD_SIZE], got[MEMCARD_SIZE];
    char dir[600], card1[700], card2[700];
    uint8_t sector[MEMCARD_SECTOR_SIZE];
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "memcard_replay_scratch");
    snprintf(card1, sizeof card1, "%s/card1.mcd", dir);
    snprintf(card2, sizeof card2, "%s/card2.mcd", dir);
    remove(card1);
    remove(card2);

    /* The player has card 1 inserted (formatted on first use) and slot 2
     * empty. A save before playback must reach the file first. */
    const MemcardSlotConfig slots[2] = { { card1, 1 }, { card2, 0 } };
    memcard_init_slots(dir, slots);
    CHECK(memcard_is_present(0) && !memcard_is_present(1), "player: card 1 in, slot 2 empty");
    memset(sector, 0x42, sizeof sector);
    CHECK(memcard_write_sector(0, 100, sector) == 0, "player save pending");
    CHECK(memcard_export_raw(0, player1) == 0, "player card 1 image");

    /* The replay booted with both cards, different contents. */
    for (unsigned i = 0; i < sizeof replay; ++i) replay[i] = (uint8_t)(i * 3u + 7u);
    CHECK(memcard_replay_begin(replay, 3u) == 0, "replay cards in");
    CHECK(read_file(card1, file_image) && !memcmp(file_image, player1, MEMCARD_SIZE),
          "the player's pending save was flushed before the swap");
    CHECK(memcard_is_present(0) && memcard_is_present(1), "the replay's two cards are inserted");
    CHECK(memcard_read_sector(1, 5, sector) == 0 &&
          !memcmp(sector, replay + MEMCARD_SIZE + 5 * MEMCARD_SECTOR_SIZE, MEMCARD_SECTOR_SIZE),
          "the guest reads the replay's card 2");

    /* The guest saves during playback: memory only. */
    memset(sector, 0x99, sizeof sector);
    CHECK(memcard_write_sector(0, 200, sector) == 0 && memcard_write_sector(1, 200, sector) == 0,
          "guest writes accepted");
    memcard_flush_all();
    CHECK(read_file(card1, file_image) && !memcmp(file_image, player1, MEMCARD_SIZE),
          "a flush during playback leaves the player's card 1 file alone");
    CHECK(!read_file(card2, file_image), "and creates no file for card 2");

    /* A second begin (a new playback) swaps images but keeps the player's
     * bindings for the end. */
    CHECK(memcard_replay_begin(replay, 1u) == 0 && memcard_is_present(0) && !memcard_is_present(1),
          "second replay: only card 1");

    memcard_replay_end();
    CHECK(memcard_is_present(0) && !memcard_is_present(1), "the player's slots are back");
    CHECK(memcard_export_raw(0, got) == 0 && !memcmp(got, player1, MEMCARD_SIZE),
          "the player's card 1 is back, without the guest's replay save");
    memcard_replay_end();   /* no-op */
    CHECK(memcard_is_present(0), "a second end changes nothing");

    remove(card1);
    remove(card2);
    if (failures) { fprintf(stderr, "%d of %d checks failed\n", failures, checks); return 1; }
    printf("PASS: memcard replay cards, %d checks\n", checks);
    return 0;
}
