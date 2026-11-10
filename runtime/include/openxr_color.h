/* Host presentation only: PSX/display RGB values are already encoded. */
#pragma once
#include <stdint.h>
#define PSX_XR_RGBA8 0x8058
#define PSX_XR_SRGB8_ALPHA8 0x8C43
static inline int64_t psx_xr_color_format(const int64_t *formats, uint32_t count) {
    int rgba = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i] == PSX_XR_SRGB8_ALPHA8) return PSX_XR_SRGB8_ALPHA8;
        if (formats[i] == PSX_XR_RGBA8) rgba = 1;
    }
    return rgba ? PSX_XR_RGBA8 : 0;
}
/* Used only when gamma must be applied or the runtime offers a linear target.
 * Gamma precedes decoding, matching desktop presentation. Alpha is linear. */
#define PSX_XR_COLOR_FS \
    "#version 330\n" \
    "in vec2 v_uv; uniform sampler2D u_tex; uniform float u_gamma;\n" \
    "uniform int u_linear; out vec4 frag;\n" \
    "void main(){ vec4 c=texture(u_tex,v_uv);\n" \
    " if(u_gamma != 1.0) c.rgb=pow(max(c.rgb,vec3(0.0)),vec3(1.0/u_gamma));\n" \
    " if(u_linear != 0) c.rgb=mix(c.rgb/12.92,\n" \
    "   pow((c.rgb+0.055)/1.055,vec3(2.4)),greaterThan(c.rgb,vec3(0.04045)));\n" \
    " frag=c; }\n"
