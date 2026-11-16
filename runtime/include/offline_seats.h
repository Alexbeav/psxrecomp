// offline_seats.h - how many console ports the offline pad sampler reads.

#pragma once

/* game.toml players bounds the offline sampler, so a one-player title reads
 * console port 1 only. Two cases still need port 2 read:
 *   - the plugs were swapped live (Ctrl+F6), so the player's pad is in port 2;
 *   - a seat holds a PS1 Mouse (PS1B-279). Mouse titles often expect a pad in
 *     the other port for the same player (Syndicate Wars keeps pause, group
 *     mode and view rotation on the pad; Quake II moves on the pad and aims
 *     with the mouse), so a pad there must be live, not a connected idle pad.
 * Nothing changes for a title without a mouse seat. */
static inline int offline_sampled_seat_count(int pad_count, int ports_swapped,
                                             int mouse_seat, int max_players)
{
    int n = pad_count < 1 ? 1 : pad_count;
    if ((ports_swapped || mouse_seat) && n < 2) n = 2;
    if (n > max_players) n = max_players;
    return n;
}
