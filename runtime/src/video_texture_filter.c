/* video_texture_filter.c - see video_texture_filter.h. */
#include "video_texture_filter.h"

int psx_texture_filter_from_ui(int value) {
    return (value >= PSX_TEXTURE_FILTER_NEAREST && value <= PSX_TEXTURE_FILTER_XBR)
               ? value : PSX_TEXTURE_FILTER_NEAREST;
}
