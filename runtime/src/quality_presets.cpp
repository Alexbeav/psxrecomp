/* quality_presets.cpp -- graphics presets (quality_presets.h,
 * docs/QUALITY_PRESETS.md).
 *
 * A title offers presets as [quality.low/medium/high/ultra] tables of [video]
 * keys. On first launch, and whenever the machine's fingerprint changes while
 * a preset (not Custom) is in force, the host is detected and the matching
 * preset is chosen and saved to settings.toml. A player who changes any
 * setting a preset governs turns the state into Custom (the launcher does
 * this); Custom is never overridden again, only re-detected on request. */
#include "quality_presets.h"

#include "host_overlay.h"

#include "config_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace psxq {

unsigned offered_mask(const PSXRecompV4::GameConfig& gc) {
    unsigned m = 0;
    for (const auto& p : gc.quality_presets) {
        const int t = psx_quality_from_name(p.name.c_str());
        if (t >= 0 && t < PSX_QUALITY_COUNT) m |= 1u << t;
    }
    return m;
}

std::vector<std::string> governed_keys(const PSXRecompV4::GameConfig& gc) {
    std::vector<std::string> keys;
    for (const auto& p : gc.quality_presets)
        for (const auto& k : p.keys)
            if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    return keys;
}

Decision decide(unsigned offered, const PSXRecompV4::UserSettings& us,
                PsxHostInfo host, const char* env, bool redetect,
                const std::function<int(char*, size_t)>& gl_probe, bool no_detect) {
    Decision d;
    if (!(offered & 0xFu)) return d;          /* no presets: nothing changes */
    char fp[17];
    psx_quality_fingerprint(&host, fp);       /* OS facts only: stable per launch */
    d.fingerprint = fp;

    const int saved = us.has_quality_preset
        ? psx_quality_from_name(us.quality_preset.c_str()) : PSX_QUALITY_NONE;
    const bool same_hw = us.has_quality_hardware && us.quality_hardware == d.fingerprint;
    const bool due = redetect || saved == PSX_QUALITY_NONE ||
                     (saved != PSX_QUALITY_CUSTOM && !same_hw);

    if (due && no_detect) {
        /* Headless and hidden-window runs (CI, scripted tests) never detect
         * and never write settings.toml: a saved preset still applies, else
         * the title's [video] block does. */
        d.detection_skipped = true;
        if (saved >= 0 && saved < PSX_QUALITY_COUNT)
            d.preset = d.base = psx_quality_pick_offered(saved, offered);
    } else if (due) {
        /* The GPU OpenGL will actually use: a hybrid laptop or a desktop
         * with an iGPU may name another adapter in the OS. The OS name (or
         * the Linux PCI id) is only a fallback, and a result reached without
         * the GL probe is used for this run but not saved, so the next
         * launch probes again. */
        bool probed = false;
        if (gl_probe) {
            char r[sizeof host.gpu];
            if (gl_probe(r, sizeof r) && r[0]) {
                std::snprintf(host.gpu, sizeof host.gpu, "%s", r);
                probed = true;
            }
        }
        char reason[192];
        d.detected = psx_quality_classify(&host, reason, sizeof reason);
        d.reason = reason;
        d.detected_now = true;
        d.gl_probed = probed;
        d.persist = probed;
        d.preset = d.base = psx_quality_pick_offered(d.detected, offered);
    } else if (saved == PSX_QUALITY_CUSTOM) {
        d.preset = PSX_QUALITY_CUSTOM;
        int base = us.has_quality_base ? psx_quality_from_name(us.quality_base.c_str())
                                       : PSX_QUALITY_NONE;
        if (base < 0 || base >= PSX_QUALITY_COUNT) {
            const int det = us.has_quality_detected
                ? psx_quality_from_name(us.quality_detected.c_str()) : PSX_QUALITY_NONE;
            base = (det >= 0 && det < PSX_QUALITY_COUNT) ? det : PSX_QUALITY_ULTRA;
        }
        d.base = psx_quality_pick_offered(base, offered);
        d.hardware_changed_custom = !same_hw;
    } else {
        d.preset = d.base = psx_quality_pick_offered(saved, offered);
        /* The title dropped the saved preset: save the substitute. */
        d.persist = d.preset != saved;
    }
    if (!d.detected_now && us.has_quality_detected)
        d.detected = psx_quality_from_name(us.quality_detected.c_str());

    char sum[256];
    psx_quality_summary(&host, sum, sizeof sum);
    d.summary = sum;

    if (env && env[0]) {
        const int t = psx_quality_from_name(env);
        if (t >= 0 && t < PSX_QUALITY_COUNT) {
            d.preset = d.base = psx_quality_pick_offered(t, offered);
            d.env_override = true;
            d.persist = false;               /* one run: never written */
        }
    }
    return d;
}

void mask_user_settings(PSXRecompV4::UserSettings& us,
                        const std::vector<std::string>& keys) {
    for (const std::string& k : keys) {
        if (k == "internal_resolution" || k == "supersampling") {
            us.has_internal_resolution = false;
            us.has_supersampling = false;
        } else if (k == "dynamic_resolution") {
            us.has_dynamic_resolution = false;
        } else if (k == "dynamic_resolution_min") {
            us.has_dynamic_resolution_min = false;
        } else if (k == "frame_generation") {
            us.has_frame_generation = false;
        } else if (k == "render_thread") {
            us.has_render_thread = false;
        } else if (k == "present_thread") {
            us.has_present_thread = false;
        } else if (k == "texture_filtering") {
            us.has_texture_filter = false;
        } else if (k == "fmv_filter") {
            us.has_fmv_filter = false;
        } else if (k == "antialiasing") {
            us.has_antialiasing = false;
        } else if (k == "geometry_correction") {
            us.has_geometry_correction = false;
        } else if (k == "perspective_texturing") {
            us.has_perspective_texturing = false;
        } else if (k == "frame_interpolation") {
            us.has_frame_interpolation = false;
        } else if (k == "frame_interpolation_fps") {
            us.has_frame_interpolation_fps = false;
        }
        /* The preset-only keys (PSXRecompV4::quality_key_supported) have no
         * settings.toml form: the preset's value already stands. Any other
         * key is rejected when game.toml loads. */
    }
}

void record(PSXRecompV4::UserSettings& us, const Decision& d) {
    if (d.preset == PSX_QUALITY_NONE) return;
    us.quality_preset = psx_quality_name(d.preset);
    us.has_quality_preset = true;
    if (d.base >= 0 && d.base < PSX_QUALITY_COUNT) {
        us.quality_base = psx_quality_name(d.base);
        us.has_quality_base = true;
    }
    if (!d.fingerprint.empty() && (d.detected_now || !us.has_quality_hardware)) {
        us.quality_hardware = d.fingerprint;
        us.has_quality_hardware = true;
    }
    if (d.detected >= 0 && d.detected < PSX_QUALITY_COUNT) {
        us.quality_detected = psx_quality_name(d.detected);
        us.has_quality_detected = true;
    }
}


void copy_key(PSXRecompV4::UserSettings& d, const PSXRecompV4::UserSettings& s,
              const std::string& k) {
#define QCOPY(f) do { d.has_##f = s.has_##f; d.f = s.f; } while (0)
    if (k == "internal_resolution" || k == "supersampling") {
        QCOPY(internal_resolution); QCOPY(supersampling);
    } else if (k == "dynamic_resolution") { QCOPY(dynamic_resolution);
    } else if (k == "dynamic_resolution_min") { QCOPY(dynamic_resolution_min);
    } else if (k == "frame_generation") { QCOPY(frame_generation);
    } else if (k == "render_thread") { QCOPY(render_thread);
    } else if (k == "present_thread") { QCOPY(present_thread);
    } else if (k == "texture_filtering") { QCOPY(texture_filter);
    } else if (k == "fmv_filter") { QCOPY(fmv_filter);
    } else if (k == "antialiasing") { QCOPY(antialiasing);
    } else if (k == "geometry_correction") { QCOPY(geometry_correction);
    } else if (k == "perspective_texturing") { QCOPY(perspective_texturing);
    } else if (k == "frame_interpolation") { QCOPY(frame_interpolation);
    } else if (k == "frame_interpolation_fps") { QCOPY(frame_interpolation_fps);
    }
#undef QCOPY
}

Edit live_edit(const PsxHostVideoSettings& c, const PsxHostVideoSettings& n,
               const std::vector<std::string>& keys) {
    for (const std::string& k : keys) {
        const bool diff =
            (k == "texture_filtering" && c.texture_filter != n.texture_filter) ||
            (k == "fmv_filter" && c.fmv_filter != n.fmv_filter) ||
            (k == "antialiasing" && c.present_linear != n.present_linear) ||
            (k == "dynamic_resolution" && c.dynamic_resolution != n.dynamic_resolution) ||
            (k == "frame_generation" && c.frame_generation != n.frame_generation) ||
            (k == "render_thread" && c.render_thread != n.render_thread) ||
            (k == "present_thread" && c.present_thread != n.present_thread) ||
            ((k == "internal_resolution" || k == "supersampling") &&
             (c.internal_resolution != n.internal_resolution ||
              c.supersampling != n.supersampling));
        if (diff) return Edit::GOVERNED_EDIT;
    }
    return Edit::NONE;
}

Edit launcher_custom_edit(const std::vector<std::string>& keys, int base, int current,
                          const std::function<bool(const std::string&)>& row_changed,
                          int* picked) {
    if (current == PSX_QUALITY_CUSTOM) return Edit::GOVERNED_EDIT;   /* stays Custom */
    for (const std::string& k : keys)
        if (PSXRecompV4::quality_key_player_editable(k) && row_changed(k))
            return Edit::GOVERNED_EDIT;
    if (base >= 0 && base < PSX_QUALITY_COUNT && base != current) {
        if (picked) *picked = base;
        return Edit::PICK;
    }
    return Edit::NONE;
}

Rows rows_from_runtime(const PSXRecompV4::RuntimeConfig& r,
                       bool has_dynres_env, int dynres_env,
                       bool has_dynres_min_env, int dynres_min_env) {
    Rows o;
    o.texture_filter = r.video_texture_filter;
    int fmv = r.video_fmv_filter;   /* cfg -> launcher (cfg_fmv_filter_to_launcher) */
    if (fmv < 0 || fmv >= PSXRecompV4::VIDEO_FMV_FILTER_COUNT)
        fmv = PSXRecompV4::VIDEO_FMV_FILTER_DEFAULT;
    o.fmv_filter = fmv + 1;
    o.antialiasing = r.video_antialiasing ? 1 : 0;
    o.dynamic_resolution = (has_dynres_env ? dynres_env : (r.video_dynamic_resolution ? 1 : 0)) ? 1 : 0;
    o.dynamic_resolution_min = has_dynres_min_env ? dynres_min_env : r.video_dynamic_resolution_min;
    o.render_thread = r.video_render_thread ? 1 : 0;
    o.present_thread = r.video_present_thread ? 1 : 0;
    o.frame_generation = r.video_frame_generation ? 1 : 0;
    o.geometry_correction = r.video_geometry_correction ? 1 : 0;
    o.perspective_texturing = r.video_perspective_texturing ? 1 : 0;
    o.frame_interp = r.video_frame_interpolation ? 1 : 0;
    o.frame_interp_fps = r.video_frame_interpolation_fps;
    return o;
}

bool row_differs(const std::string& k, const Rows& a, const Rows& b) {
    if (k == "texture_filtering") return a.texture_filter != b.texture_filter;
    if (k == "fmv_filter") return a.fmv_filter != b.fmv_filter;
    if (k == "antialiasing") return (a.antialiasing != 0) != (b.antialiasing != 0);
    if (k == "dynamic_resolution") return (a.dynamic_resolution != 0) != (b.dynamic_resolution != 0);
    if (k == "dynamic_resolution_min") return a.dynamic_resolution_min != b.dynamic_resolution_min;
    if (k == "frame_generation") return (a.frame_generation != 0) != (b.frame_generation != 0);
    if (k == "render_thread") return (a.render_thread != 0) != (b.render_thread != 0);
    if (k == "present_thread") return (a.present_thread != 0) != (b.present_thread != 0);
    if (k == "geometry_correction") return (a.geometry_correction != 0) != (b.geometry_correction != 0);
    if (k == "perspective_texturing") return (a.perspective_texturing != 0) != (b.perspective_texturing != 0);
    if (k == "frame_interpolation") return (a.frame_interp != 0) != (b.frame_interp != 0);
    if (k == "frame_interpolation_fps") return a.frame_interp_fps != b.frame_interp_fps;
    return false;   /* preset-only keys have no row */
}

LauncherEdit classify_launcher(int preset1, int base1, const Rows& submitted,
                               int current, int current_base,
                               const std::vector<std::string>& keys,
                               const std::function<bool(int, Rows*)>& preset_rows) {
    LauncherEdit out;
    if (preset1 < 1 || preset1 > 5) return out;
    const int preset = preset1 - 1;
    if (preset != PSX_QUALITY_CUSTOM) {
        if (preset != current) { out.edit = Edit::PICK; out.picked = preset; }
        return out;
    }
    /* A player may pick a different tier and edit one of its rows before
     * leaving the editor: Custom belongs to that tier. The frontends flag
     * Custom on any row their snapshot tracks; only a row this title's
     * presets govern makes it Custom. */
    const int base = base1 - 1;
    Rows expect;
    const bool have = base >= 0 && base < PSX_QUALITY_COUNT && preset_rows && preset_rows(base, &expect);
    if (have && current != PSX_QUALITY_CUSTOM) {
        int picked = PSX_QUALITY_NONE;
        const Edit e = launcher_custom_edit(
            keys, base, current,
            [&](const std::string& k) { return row_differs(k, submitted, expect); }, &picked);
        if (e == Edit::NONE) return out;                       /* unrelated rows only */
        if (e == Edit::PICK) { out.edit = e; out.picked = picked; return out; }
    }
    out.base_changed = have && base != current_base;
    if (current != PSX_QUALITY_CUSTOM || out.base_changed) out.edit = Edit::GOVERNED_EDIT;
    if (have) { out.set_base = true; out.base = base; }
    return out;
}

namespace {
int Rows::* const kRowFields[] = {
    &Rows::texture_filter, &Rows::fmv_filter, &Rows::antialiasing,
    &Rows::dynamic_resolution, &Rows::dynamic_resolution_min,
    &Rows::frame_generation, &Rows::render_thread, &Rows::present_thread,
    &Rows::geometry_correction, &Rows::perspective_texturing,
    &Rows::frame_interp, &Rows::frame_interp_fps,
};
}  // namespace

void MenuSelection::record(const Rows& selected, const Rows& live_after) {
    sel_ = selected;
    pending_ = 0;
    for (unsigned i = 0; i < sizeof kRowFields / sizeof kRowFields[0]; ++i)
        if (selected.*kRowFields[i] != live_after.*kRowFields[i]) pending_ |= 1u << i;
    valid_ = true;
}

Rows MenuSelection::seed(const Rows& live) const {
    Rows out = live;
    if (!valid_) return out;
    for (unsigned i = 0; i < sizeof kRowFields / sizeof kRowFields[0]; ++i)
        if (pending_ & (1u << i)) out.*kRowFields[i] = sel_.*kRowFields[i];
    return out;
}

static void copy_meta(PSXRecompV4::UserSettings& d, const PSXRecompV4::UserSettings& s) {
    d.has_quality_preset = s.has_quality_preset;     d.quality_preset = s.quality_preset;
    d.has_quality_base = s.has_quality_base;         d.quality_base = s.quality_base;
    d.has_quality_hardware = s.has_quality_hardware; d.quality_hardware = s.quality_hardware;
    d.has_quality_detected = s.has_quality_detected; d.quality_detected = s.quality_detected;
}

bool settle_save(PSXRecompV4::UserSettings& save, const PSXRecompV4::UserSettings& disk,
                 Decision& run, Edit edit, int picked,
                 const std::vector<std::string>& keys) {
    if (run.preset == PSX_QUALITY_NONE) return false;   /* no presets offered */
    auto keep_disk = [&] {
        for (const std::string& k : keys) copy_key(save, disk, k);
        copy_meta(save, disk);
    };
    if (run.env_override) {           /* one run: nothing quality-related persists */
        keep_disk();
        return false;
    }
    if (edit == Edit::PICK && picked >= 0 && picked < PSX_QUALITY_COUNT) {
        const bool changed = run.preset != picked;
        run.preset = run.base = picked;
        for (const std::string& k : keys) copy_key(save, disk, k);
        copy_meta(save, disk);
        record(save, run);
        return changed;
    }
    if (edit == Edit::GOVERNED_EDIT || run.preset == PSX_QUALITY_CUSTOM) {
        const bool changed = run.preset != PSX_QUALITY_CUSTOM;
        run.preset = PSX_QUALITY_CUSTOM;   /* base stays: Custom "from" it */
        copy_meta(save, disk);
        record(save, run);
        return changed;
    }
    keep_disk();                          /* preset in force, nothing governed changed */
    if (run.persist) record(save, run);
    return false;
}

}  // namespace psxq

