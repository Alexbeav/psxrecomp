// internal_resolution.h: preset ids, the value encoding shared with the
// launcher, and the integer scale each preset asks for. Presets x reference
// 240/480 x backend maxima (the GL clamp at 16384 is 16x; 18x needs the
// windowed high-resolution surface or a 32768-limit GPU).

#include "internal_resolution.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

static void expect(const char* what, long long got, long long want) {
    if (got != want) {
        std::printf("FAIL %s: got %lld want %lld\n", what, got, want);
        ++g_failures;
    }
}

int main() {
    // Presets at the NTSC 240-line reference (R4 races are 320x240).
    struct { const char* id; int s240; int s480; } table[] = {
        { "native", 1, 1 }, { "720p", 3, 2 }, { "1080p", 5, 3 }, { "1440p", 6, 3 },
        { "4k", 9, 5 }, { "5k", 12, 6 }, { "8k", 18, 9 },
    };
    for (const auto& t : table) {
        int v = 0;
        expect(t.id, psx_ir_parse(t.id, &v), 1);
        expect(t.id, psx_resolve_internal_scale(v, 240, 0, 32), t.s240);
        expect(t.id, psx_resolve_internal_scale(v, 480, 0, 32), t.s480);
        expect("id round trip", std::strcmp(psx_ir_id_for(v), t.id), 0);
    }
    // Backend maxima: software/Vulkan 4, full-VRAM GL at a 16384 limit 16.
    int v8k = 0;
    psx_ir_parse("8K", &v8k);
    expect("8k sw", psx_resolve_internal_scale(v8k, 240, 0, 4), 4);
    expect("8k gl16", psx_resolve_internal_scale(v8k, 240, 0, 16), 16);
    expect("8k lines", v8k, 4320);

    // Match display: the monitor's pixel height, 1 until it is known.
    int vd = 0;
    expect("display parse", psx_ir_parse("display", &vd), 1);
    expect("display value", vd, PSX_IR_DISPLAY);
    expect("display unknown", psx_resolve_internal_scale(vd, 240, 0, 32), 1);
    expect("display 1080", psx_resolve_internal_scale(vd, 240, 1080, 32), 5);
    expect("display 1964 (14in MBP)", psx_resolve_internal_scale(vd, 240, 1964, 32), 9);
    expect("display 2160", psx_resolve_internal_scale(vd, 240, 2160, 32), 9);
    expect("display 2880 (5K)", psx_resolve_internal_scale(vd, 240, 2880, 32), 12);
    expect("display 4320 (8K)", psx_resolve_internal_scale(vd, 240, 4320, 32), 18);
    expect("display id", std::strcmp(psx_ir_id_for(vd), "display"), 0);

    // Integer line counts and rejects.
    int v = 0;
    expect("lines parse", psx_ir_parse("1600", &v), 1);
    expect("lines value", v, 1600);
    expect("lines scale", psx_resolve_internal_scale(v, 240, 0, 32), 7);
    expect("custom has no id", psx_ir_id_for(1600) == nullptr, 1);
    v = 77;
    expect("reject junk", psx_ir_parse("4kk", &v), 0);
    expect("reject empty", psx_ir_parse("", &v), 0);
    expect("reject 1", psx_ir_parse("1", &v), 0);
    expect("reject huge", psx_ir_parse("99999", &v), 0);
    expect("untouched on reject", v, 77);
    expect("unset scale", psx_resolve_internal_scale(PSX_IR_UNSET, 240, 0, 32), 1);
    expect("bad reference defaults to 240", psx_resolve_internal_scale(2160, 0, 0, 32), 9);

    // Legacy supersampling shown as a preset when one resolves to it.
    expect("legacy 1", psx_ir_from_supersampling(1, 240), PSX_IR_NATIVE);
    expect("legacy 2", psx_ir_from_supersampling(2, 240), 480);
    expect("legacy 3", psx_ir_from_supersampling(3, 240), 720);
    expect("legacy 4", psx_ir_from_supersampling(4, 240), 960);
    expect("legacy 5", psx_ir_from_supersampling(5, 240), 1080);
    expect("legacy 9", psx_ir_from_supersampling(9, 240), 2160);
    expect("legacy 18", psx_ir_from_supersampling(18, 240), 4320);
    // And the legacy value resolves back to the same scale.
    for (int n = 1; n <= 32; n++)
        expect("legacy round trip",
               psx_resolve_internal_scale(psx_ir_from_supersampling(n, 240), 240, 0, 32), n);

    if (g_failures) return 1;
    std::printf("internal_resolution: all checks passed\n");
    return 0;
}
