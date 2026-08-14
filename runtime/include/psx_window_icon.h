#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

/* Load assets/psxrecomp.png beside the exe (or SDL base path) and apply as
 * the window / taskbar icon. No-op when the PNG is missing. */
void psx_apply_window_icon(struct SDL_Window *window, const char *argv0);

#ifdef __cplusplus
}
#endif
