#version 450
/* Swapchain present: sample the displayed VRAM rect and write it OPAQUE.
 * VRAM alpha carries the PSX mask/STP bit (0 on most pixels); copying it into
 * the swapchain with a transfer blit let AMD's compositor treat the window as
 * partly transparent (desktop showing through as black/white blocks). */
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(push_constant) uniform PC {
    vec4 u_src_rect;    /* displayed region in normalized src coords: x0,y0,x1,y1 */
} pc;
void main() {
    o_col = vec4(texture(u_src, mix(pc.u_src_rect.xy, pc.u_src_rect.zw, v_uv)).rgb, 1.0);
}
