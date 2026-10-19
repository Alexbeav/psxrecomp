/* video_texture_filter.h - the one texture-filter enum for settings.toml,
 * the launcher handoff and live apply. */
#ifndef PSX_VIDEO_TEXTURE_FILTER_H
#define PSX_VIDEO_TEXTURE_FILTER_H
#ifdef __cplusplus
extern "C" {
#endif

/* Values match [video] texture_filtering and gr_set_texture_filter().
 * STABLE_WORLD is set by mods, not by settings. */
#define PSX_TEXTURE_FILTER_NEAREST      0
#define PSX_TEXTURE_FILTER_BILINEAR     1
#define PSX_TEXTURE_FILTER_STABLE_WORLD 2
#define PSX_TEXTURE_FILTER_XBR          3

/* A launcher value to the enum: in-range values pass through (xBR included),
 * anything else is nearest, the native default. Older launchers send 0/1. */
int psx_texture_filter_from_ui(int value);

#ifdef __cplusplus
}
#endif
#endif
