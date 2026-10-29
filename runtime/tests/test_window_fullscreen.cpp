#include "window_fullscreen.h"
#if defined(PSX_SDL3)
#include <SDL3/SDL_main.h>
#endif
#include <cstdio>

static int failures;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s (%s)\n", message, SDL_GetError()); ++failures; }
}

static void drawable_size(SDL_Window* window, int* width, int* height) {
#if defined(PSX_SDL3)
    check(SDL_GetWindowSizeInPixels(window, width, height), "read drawable pixels");
#else
    SDL_Surface* surface = SDL_GetWindowSurface(window);
    check(surface != nullptr, "read drawable surface");
    *width = surface ? surface->w : 0;
    *height = surface ? surface->h : 0;
#endif
}

static void check_fullscreen_scale(SDL_Window* window,
                                    const PsxWindowFullscreen& state) {
#if defined(PSX_SDL3)
    SDL_SyncWindow(window);
#endif
    int before_w = 0, before_h = 0;
    drawable_size(window, &before_w, &before_h);
    for (int scale = 1; scale <= 2; ++scale) {
        check(psx_window_resize_windowed(window, &state, scale * 640,
                                         scale * 480) == -1,
              "fullscreen refuses a window-scale resize");
        int width = 0, height = 0;
        drawable_size(window, &width, &height);
        check(width == before_w && height == before_h,
              "two window scales leave the fullscreen drawable unchanged");
        std::printf("mode=%d window_scale=%d drawable=%dx%d\n",
                    state.mode, scale, width, height);
    }
}

int main() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    SDL_Window* window = SDL_CreateWindow("Fullscreen regression", 100, 100,
                                          640, 480, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    if (!window) return 1;
    PsxWindowFullscreen state;
    SDL_SetWindowPosition(window, 100, 100);
    int old_x, old_y, old_w, old_h;
    SDL_GetWindowPosition(window, &old_x, &old_y);
    SDL_GetWindowSize(window, &old_w, &old_h);
#if defined(PSX_SDL3)
    const SDL_DisplayID original_display = SDL_GetDisplayForWindow(window);
    SDL_DisplayMode original_mode{};
    check(SDL_GetCurrentDisplayMode(original_display, &original_mode) == 0, "read desktop display mode");
    SDL_ShowCursor();
#endif
    for (int i = 0; i < 3; ++i) {
        check(psx_window_fullscreen_set(window, &state, 1) == 0, "enter borderless");
        check_fullscreen_scale(window, state);
#if defined(_WIN32) && defined(PSX_SDL3)
        check(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN), "borderless never arms native fullscreen");
        check((SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS) != 0, "borderless has no decorations");
        check(SDL_GetWindowFullscreenMode(window) == nullptr, "borderless has no exclusive display mode");
        SDL_Rect bounds{};
        SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &bounds);
        int full_w, full_h; SDL_GetWindowSize(window, &full_w, &full_h);
        check(full_w == bounds.w && full_h == bounds.h, "borderless covers the current display");
        check(!SDL_CursorVisible(), "fullscreen cursor hidden");
        SDL_DisplayMode current{};
        check(SDL_GetCurrentDisplayMode(original_display, &current) == 0 &&
              current.w == original_mode.w && current.h == original_mode.h &&
              current.refresh_rate == original_mode.refresh_rate,
              "borderless does not change the desktop display mode");
#endif
        check(psx_window_fullscreen_set(window, &state, 0) == 0, "leave borderless");
        int x, y, w, h;
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowSize(window, &w, &h);
        check(x == old_x && y == old_y && w == old_w && h == old_h, "original window geometry restored");
        check(!(SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS), "window border restored");
        check((SDL_GetWindowFlags(window) & SDL_WINDOW_RESIZABLE) != 0, "resizability restored");
#if defined(PSX_SDL3)
        check(SDL_CursorVisible(), "windowed cursor visibility restored");
#endif
    }
    check(psx_window_fullscreen_set(window, &state, 2) == 0, "enter exclusive");
    check_fullscreen_scale(window, state);
#if defined(PSX_SDL3)
    check(SDL_GetWindowFullscreenMode(window) != nullptr, "exclusive selects an explicit display mode");
#endif
    check(psx_window_fullscreen_set(window, &state, 1) == 0, "exclusive to borderless");
    check(psx_window_fullscreen_set(window, &state, 0) == 0, "borderless to windowed");
    check(psx_window_fullscreen_set(window, &state, 3) == -1 && state.mode == 0, "invalid mode leaves window unchanged");
    check(psx_window_resize_windowed(window, &state, 320, 240) == 0,
          "windowed scale can resize after leaving fullscreen");
#if defined(PSX_SDL3)
    SDL_SyncWindow(window);
#endif
    int resized_w = 0, resized_h = 0;
    SDL_GetWindowSize(window, &resized_w, &resized_h);
    check(resized_w == 320 && resized_h == 240,
          "positive windowed resize reaches the requested size");
#if defined(PSX_SDL3)
    SDL_HideCursor();
    check(psx_window_fullscreen_set(window, &state, 1) == 0, "enter borderless with cursor already hidden");
    check(psx_window_fullscreen_set(window, &state, 0) == 0 && !SDL_CursorVisible(),
          "windowed mode preserves a previously hidden cursor");
    SDL_ShowCursor();
    SDL_MaximizeWindow(window);
    SDL_SyncWindow(window);
    const bool was_maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
    check(psx_window_fullscreen_set(window, &state, 1) == 0, "maximized to borderless");
    check(psx_window_fullscreen_set(window, &state, 0) == 0, "borderless restores maximized window");
    SDL_SyncWindow(window);
    check(!was_maximized || (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED),
          "original maximized state restored");
#endif
    SDL_DestroyWindow(window);
    SDL_Quit();
    return failures ? 1 : 0;
}
