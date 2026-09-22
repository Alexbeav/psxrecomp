#!/usr/bin/env python3
"""Title-neutral guards for the OpenGL 15-bit -> packed-24-bit handoff."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def require_in_order(body: str, *needles: str) -> None:
    pos = -1
    for needle in needles:
        pos = body.index(needle, pos + 1)


def model_contract() -> None:
    """Exercise fill/copy/upload ownership without title-specific geometry."""
    cpu = [0x1111] * 32
    fbo = cpu.copy()

    # Ordinary 15-bit GPU work owns the FBO; the CPU mirror may be stale.
    fbo[0:8] = [0x0000] * 8                         # fill
    fbo[8:16] = fbo[0:8]                            # copy
    assert cpu != fbo

    # GP1 depth transition hands the complete authoritative image to CPU.
    cpu[:] = fbo
    assert cpu == fbo

    # A depth-24 fill is dual-owned, while packed upload data is CPU-owned.
    cpu[16:24] = [0x0000] * 8
    fbo[16:24] = [0x0000] * 8
    cpu[18:22] = [0x1234, 0x5678, 0x9ABC, 0xDEF0]
    assert cpu[16:18] == [0, 0] and cpu[22:24] == [0, 0]
    assert cpu[18:22] != fbo[18:22]


def main() -> None:
    gpu = (ROOT / "runtime/src/gpu.c").read_text(encoding="utf-8")
    facade = (ROOT / "runtime/src/gpu_render.c").read_text(encoding="utf-8")
    gl = (ROOT / "runtime/src/gpu_gl_renderer.c").read_text(encoding="utf-8")

    gp1 = function_body(gpu, "static void gp1_display_mode", "static void gp1_get_info")
    require_in_order(
        gp1,
        "old_display_depth = display_depth",
        "display_depth = (val >> 4) & 1",
        "gr_display_depth_changed",
    )
    assert "g_b->display_depth_changed" in facade

    handoff = function_body(gl, "static void depth24_set_mode", "static void glb_display_depth_changed")
    require_in_order(handoff, "if (d24 && !s_depth24_skip_up)", "ensure_cpu()", "s_depth24_skip_up = d24")

    fill = function_body(gl, "static void glb_fill_rect", "static void glb_copy_rect")
    require_in_order(fill, "gpu_fill", "gpu_display_is_depth24", "sw_fill_rect")

    upload = function_body(gl, "static void glb_vram_transfer_in", "static void glb_vram_transfer_out")
    require_in_order(upload, "depth24_upload_policy()", "sw_vram_transfer_in")
    require_in_order(upload, "depth24_is_fb_transfer", "return", "up_add_transfer")

    copy = function_body(gl, "static void glb_copy_rect", "static void glb_draw_textured_triangle")
    assert "gpu_copy_rect" in copy

    model_contract()
    print("OpenGL depth-24 coherency contract: PASS")


if __name__ == "__main__":
    main()
