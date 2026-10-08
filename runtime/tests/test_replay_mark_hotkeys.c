#include "replay_mark_hotkeys.h"
#include "psx_keybinds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void) {
    int checks = 0, failures = 0;
    uint8_t keys[SDL_NUM_SCANCODES] = {0}, filtered[SDL_NUM_SCANCODES];
    unsigned held = 0;
    psx_keybinds_reset_player(1);
    /* Default RShift is guest Select; a custom F11/F12 mapping is also legal. */
    psx_keybinds_set_button(1, PSX_KB_CROSS, SDL_SCANCODE_F11);
    psx_keybinds_set_button(1, PSX_KB_RS_RIGHT, SDL_SCANCODE_F12);
    keys[SDL_SCANCODE_RSHIFT] = keys[SDL_SCANCODE_F11] = keys[SDL_SCANCODE_F12] = 1;
    const uint8_t *out = replay_mark_keyboard(keys, 1, filtered, &held);
    CHECK(held == 3 && !out[SDL_SCANCODE_RSHIFT] && !out[SDL_SCANCODE_F11] && !out[SDL_SCANCODE_F12]);
    CHECK(psx_keybinds_pad_word(out, 1) == 0xffff);
    uint8_t sticks[4] = {128,128,128,128}; psx_keybinds_sticks(out, 1, sticks);
    CHECK(sticks[2] == 128 && sticks[3] == 128);
    keys[SDL_SCANCODE_RSHIFT] = 0; /* modifier released first */
    out = replay_mark_keyboard(keys, 1, filtered, &held);
    CHECK(held == 3 && psx_keybinds_pad_word(out, 1) == 0xffff);
    keys[SDL_SCANCODE_X] = 1; /* unrelated live input still passes */
    out = replay_mark_keyboard(keys, 1, filtered, &held);
    CHECK(out[SDL_SCANCODE_X] && keys[SDL_SCANCODE_F11]);
    keys[SDL_SCANCODE_F11] = keys[SDL_SCANCODE_F12] = 0;
    CHECK(replay_mark_keyboard(keys, 1, filtered, &held) == keys && held == 0);
    keys[SDL_SCANCODE_LSHIFT] = keys[SDL_SCANCODE_F12] = keys[SDL_SCANCODE_LCTRL] = 1;
    CHECK(replay_mark_keyboard(keys, 1, filtered, &held) == keys && held == 0);
    keys[SDL_SCANCODE_LCTRL] = 0;
    CHECK(replay_mark_keyboard(keys, 0, filtered, &held) == keys && held == 0);
    CHECK(replay_mark_keyboard(keys, 1, filtered, &held) == filtered && held == 2);
    CHECK(replay_mark_keyboard(NULL, 1, filtered, &held) == NULL && held == 0);
    if (failures) return 1;
    printf("PASS: host-only replay mark keyboard, %d checks\n", checks);
    return 0;
}
