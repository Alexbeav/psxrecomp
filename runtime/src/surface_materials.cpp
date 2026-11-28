/* surface_materials.cpp — see surface_materials.h and docs/SURFACE_MATERIALS.md. */

#include "surface_materials.h"
#include "hd_texture_pack.h"

#include "toml.hpp"

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

int g_surface_materials_active = 0;

namespace {

constexpr size_t kVramWords = 1024u * 512u;
constexpr int kMaxSamplesPerFrame = 8192;
constexpr int kIdCacheSlots = 4096;

struct Rule {
    uint32_t tex;
    uint32_t pal;
    bool any_palette;
    int slot;
    std::string note;
};

struct IdCacheEntry {
    uint64_t generation = 0; /* 0 = empty */
    uint32_t query = 0;      /* packed page/depth */
    uint32_t lims = 0;       /* packed u0 v0 u1 v1 */
    int status = 0;
    uint32_t tex = 0;
};

struct Sample {
    int16_t x0, y0, x1, y1;
    uint16_t texpage, clut_x, clut_y;
    uint32_t tex, pal;
    int8_t status; /* HdTextureLookupStatus */
    uint8_t slot;
};

struct State {
    SurfaceMaterial mats[SURFACE_MAT_SLOTS]{};
    int mat_count = 0;
    std::vector<Rule> rules;
    std::unordered_map<uint64_t, int> exact; /* (tex<<32|pal) -> slot */
    std::unordered_map<uint32_t, int> any;   /* tex -> slot (palette wildcard) */
    bool highlight_on = false;
    uint32_t hl_tex = 0, hl_pal = 0;
    bool hl_any = false;
    bool probe_on = false;
    bool env_done = false;
    HdTexturePack *tracker = nullptr;
    IdCacheEntry cache[kIdCacheSlots];
    std::vector<Sample> cur, prev;
    uint64_t frames = 0;
    uint64_t classified = 0, identified = 0, tagged = 0;
    std::string source;
};

State &st() {
    static State s;
    return s;
}

void err(char *buf, size_t cap, const std::string &m) {
    if (buf && cap) std::snprintf(buf, cap, "%s", m.c_str());
}

uint64_t key64(uint32_t tex, uint32_t pal) { return (uint64_t{tex} << 32) | pal; }

void refresh_active() {
    State &s = st();
    const int on = (!s.rules.empty() || s.highlight_on || s.probe_on) ? 1 : 0;
    if (on && !s.tracker) {
        /* Tracking starts now: textures uploaded before this point have no
         * identity until the game re-uploads them (scene loads always do). */
        if (!hd_texture_tracker_create(&s.tracker)) {
            g_surface_materials_active = 0;
            return;
        }
    }
    g_surface_materials_active = on;
}

void set_defaults(SurfaceMaterial *m) {
    std::memset(m, 0, sizeof(*m));
    m->ripple_amplitude = 1.25f;
    m->ripple_scale = 0.12f;
    m->ripple_speed = 1.6f;
    m->glint = 0.35f;
    m->glint_power = 24.0f;
    m->tint[0] = m->tint[1] = m->tint[2] = 1.0f;
}

float num(const toml::value &t, const char *k, float def) {
    if (!t.contains(k)) return def;
    const toml::value &v = toml::find(t, k);
    if (v.is_floating()) return (float)v.as_floating();
    if (v.is_integer()) return (float)v.as_integer();
    throw std::runtime_error(std::string("'") + k + "' must be a number");
}

/* Number arrays accept integers too: `flow = [0, -1]` is natural TOML. */
std::vector<float> nums(const toml::value &t, const char *k, size_t want) {
    const auto &arr = toml::find(t, k).as_array();
    if (arr.size() != want)
        throw std::runtime_error(std::string("'") + k + "' must have " + std::to_string(want) + " numbers");
    std::vector<float> out;
    for (const toml::value &v : arr) {
        if (v.is_floating()) out.push_back((float)v.as_floating());
        else if (v.is_integer()) out.push_back((float)v.as_integer());
        else throw std::runtime_error(std::string("'") + k + "' must contain numbers");
    }
    return out;
}

bool parse_hex32(const std::string &t, uint32_t *out) {
    if (t.empty() || t.size() > 8) return false;
    uint32_t v = 0;
    for (char c : t) {
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

int parse_table(const toml::value &cfg, char *error, size_t cap) {
    if (!cfg.contains("format_version") || toml::find<int64_t>(cfg, "format_version") != 1) {
        err(error, cap, "format_version = 1 is required");
        return 0;
    }
    SurfaceMaterial mats[SURFACE_MAT_SLOTS]{};
    std::map<std::string, int> by_name;
    int count = 0;
    if (cfg.contains("materials")) {
        const auto &table = toml::find(cfg, "materials").as_table();
        for (const auto &kv : table) {
            if (count + 1 >= SURFACE_MAT_HIGHLIGHT) {
                err(error, cap, "too many materials (max 14)");
                return 0;
            }
            const toml::value &m = kv.second;
            const int slot = ++count;
            SurfaceMaterial *out = &mats[slot];
            set_defaults(out);
            const std::string kind = toml::find<std::string>(m, "kind");
            if (kind == "water") out->kind = SURFACE_KIND_WATER;
            else {
                err(error, cap, "material '" + kv.first + "': unknown kind '" + kind + "'");
                return 0;
            }
            out->ripple_amplitude = num(m, "ripple_amplitude", out->ripple_amplitude);
            out->ripple_scale = num(m, "ripple_scale", out->ripple_scale);
            out->ripple_speed = num(m, "ripple_speed", out->ripple_speed);
            out->glint = num(m, "glint", out->glint);
            out->glint_power = num(m, "glint_power", out->glint_power);
            out->tint_mix = num(m, "tint_mix", out->tint_mix);
            if (m.contains("flow")) {
                const auto flow = nums(m, "flow", 2);
                out->flow_u = flow[0];
                out->flow_v = flow[1];
            }
            if (m.contains("tint")) {
                const auto tint = nums(m, "tint", 3);
                for (int i = 0; i < 3; i++) out->tint[i] = tint[i];
            }
            if (out->glint_power < 1.0f) out->glint_power = 1.0f;
            std::snprintf(out->name, sizeof out->name, "%s", kv.first.c_str());
            by_name[kv.first] = slot;
        }
    }
    std::vector<Rule> rules;
    std::unordered_map<uint64_t, int> exact;
    std::unordered_map<uint32_t, int> any;
    if (cfg.contains("surface")) {
        const auto &arr = toml::find(cfg, "surface").as_array();
        for (const toml::value &r : arr) {
            const std::string key = toml::find<std::string>(r, "key");
            const std::string mat = toml::find<std::string>(r, "material");
            Rule rule{};
            int anyp = 0;
            if (!surface_parse_key(key.c_str(), &rule.tex, &rule.pal, &anyp)) {
                err(error, cap, "bad surface key '" + key + "' (want xxxxxxxx-yyyyyyyy or xxxxxxxx-*)");
                return 0;
            }
            rule.any_palette = anyp != 0;
            const auto found = by_name.find(mat);
            if (found == by_name.end()) {
                err(error, cap, "surface '" + key + "' names unknown material '" + mat + "'");
                return 0;
            }
            rule.slot = found->second;
            if (r.contains("note")) rule.note = toml::find<std::string>(r, "note");
            if (rule.any_palette) {
                if (any.count(rule.tex)) { err(error, cap, "duplicate key '" + key + "'"); return 0; }
                any[rule.tex] = rule.slot;
            } else {
                if (exact.count(key64(rule.tex, rule.pal))) { err(error, cap, "duplicate key '" + key + "'"); return 0; }
                exact[key64(rule.tex, rule.pal)] = rule.slot;
            }
            rules.push_back(rule);
        }
    }
    State &s = st();
    std::memcpy(s.mats, mats, sizeof mats);
    s.mat_count = count;
    s.rules.swap(rules);
    s.exact.swap(exact);
    s.any.swap(any);
    s.mats[SURFACE_MAT_HIGHLIGHT].kind = SURFACE_KIND_HIGHLIGHT;
    std::snprintf(s.mats[SURFACE_MAT_HIGHLIGHT].name, sizeof s.mats[0].name, "highlight");
    refresh_active();
    return 1;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* JSON string escape for notes / names / paths. */
std::string jesc(const std::string &in) {
    std::string o;
    for (char c : in) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

int put(char *buf, int cap, int at, const char *fmt, ...) {
    if (at >= cap) return at;
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf + at, (size_t)(cap - at), fmt, ap);
    va_end(ap);
    if (n < 0) return at;
    return (at + n >= cap) ? cap - 1 : at + n;
}

} // namespace

extern "C" {

int surface_parse_key(const char *text, uint32_t *tex, uint32_t *pal, int *any_palette) {
    if (!text) return 0;
    const std::string t(text);
    const size_t dash = t.find('-');
    if (dash == std::string::npos || t.find('-', dash + 1) != std::string::npos) return 0;
    uint32_t a = 0, b = 0;
    if (!parse_hex32(t.substr(0, dash), &a)) return 0;
    const std::string rest = t.substr(dash + 1);
    int anyp = 0;
    if (rest == "*") anyp = 1;
    else if (!parse_hex32(rest, &b)) return 0;
    if (tex) *tex = a;
    if (pal) *pal = b;
    if (any_palette) *any_palette = anyp;
    return 1;
}

void surface_materials_init_from_env(void) {
    State &s = st();
    if (s.env_done) return;
    s.env_done = true;
    const char *path = std::getenv("PSXRECOMP_SURFACE_MATERIALS");
    if (!path || !path[0]) return;
    char e[256];
    if (surface_materials_load_file(path, e, sizeof e))
        std::fprintf(stdout, "psxrecomp: surface materials: %d material(s), %d surface rule(s) from %s\n",
                     s.mat_count, (int)s.rules.size(), path);
    else
        std::fprintf(stdout, "psxrecomp: surface materials: %s: %s (disabled)\n", path, e);
}

int surface_materials_load_file(const char *path, char *error, size_t cap) {
    if (!path || !path[0]) { err(error, cap, "empty path"); return 0; }
    try {
        const toml::value cfg = toml::parse(std::string(path));
        if (!parse_table(cfg, error, cap)) return 0;
        st().source = path;
        return 1;
    } catch (const std::exception &e) {
        err(error, cap, e.what());
        return 0;
    }
}

int surface_materials_load_text(const char *text, char *error, size_t cap) {
    if (!text) { err(error, cap, "null text"); return 0; }
    try {
        std::istringstream in{std::string(text)};
        const toml::value cfg = toml::parse(in, "surface_materials");
        if (!parse_table(cfg, error, cap)) return 0;
        st().source = "<text>";
        return 1;
    } catch (const std::exception &e) {
        err(error, cap, e.what());
        return 0;
    }
}

void surface_materials_clear(void) {
    State &s = st();
    std::memset(s.mats, 0, sizeof s.mats);
    s.mat_count = 0;
    s.rules.clear();
    s.exact.clear();
    s.any.clear();
    s.source.clear();
    refresh_active();
}

int surface_materials_lookup(uint32_t tex, uint32_t pal) {
    State &s = st();
    if (s.highlight_on && tex == s.hl_tex && (s.hl_any || pal == s.hl_pal))
        return SURFACE_MAT_HIGHLIGHT;
    const auto e = s.exact.find(key64(tex, pal));
    if (e != s.exact.end()) return e->second;
    const auto a = s.any.find(tex);
    if (a != s.any.end()) return a->second;
    return 0;
}

const SurfaceMaterial *surface_material_get(int slot) {
    if (slot <= 0 || slot >= SURFACE_MAT_SLOTS) return nullptr;
    State &s = st();
    if (slot == SURFACE_MAT_HIGHLIGHT) {
        s.mats[slot].kind = SURFACE_KIND_HIGHLIGHT;
        return &s.mats[slot];
    }
    return slot <= s.mat_count ? &s.mats[slot] : nullptr;
}

int surface_materials_count(void) { return st().mat_count; }
int surface_materials_rule_count(void) { return (int)st().rules.size(); }

void surface_materials_set_highlight(uint32_t tex, uint32_t pal, int any_palette, int on) {
    State &s = st();
    s.highlight_on = on != 0;
    s.hl_tex = tex;
    s.hl_pal = pal;
    s.hl_any = any_palette != 0;
    refresh_active();
}

void surface_materials_set_probe(int on) {
    st().probe_on = on != 0;
    refresh_active();
}

void surface_note_upload(int x, int y, int w, int h, const uint16_t *words) {
    State &s = st();
    if (!g_surface_materials_active || !s.tracker || !words) return;
    if (w <= 0 || h <= 0 || w > 1024 || h > 512) return;
    hd_texture_pack_track_upload(s.tracker, (uint16_t)(x & 1023), (uint16_t)(y & 511),
                                 (uint16_t)w, (uint16_t)h, words, (size_t)w * (size_t)h,
                                 nullptr);
}

void surface_note_vram_write(int x, int y, int w, int h) {
    State &s = st();
    if (!g_surface_materials_active || !s.tracker) return;
    if (w <= 0 || h <= 0) return;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    hd_texture_pack_invalidate(s.tracker, (uint16_t)(x & 1023), (uint16_t)(y & 511),
                               (uint16_t)w, (uint16_t)h);
}

int surface_classify_prim(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                          const int lim[4], const uint16_t *vram,
                          int x0, int y0, int x1, int y1) {
    State &s = st();
    if (!g_surface_materials_active || !s.tracker || !vram || !lim) return 0;
    const int depth = (texpage >> 7) & 3;
    if (depth > 2) return 0;
    s.classified++;
    const int u0 = clampi(lim[0], 0, 255), v0 = clampi(lim[1], 0, 255);
    const int u1 = clampi(lim[2], 0, 255), v1 = clampi(lim[3], 0, 255);

    /* Texture identity depends only on residency (generation) + the query
     * rect; cache it. The palette half is recomputed every time from live
     * VRAM — CLUTs are tiny and a draw into an untracked CLUT would not bump
     * the generation. */
    const uint32_t q = (uint32_t)(texpage & 0x19Fu);
    const uint32_t lims = (uint32_t)u0 | ((uint32_t)v0 << 8) | ((uint32_t)u1 << 16) | ((uint32_t)v1 << 24);
    const uint64_t gen = hd_texture_pack_tracking_generation(s.tracker);
    const uint32_t h = (q * 2654435761u) ^ (lims * 2246822519u);
    IdCacheEntry &ce = s.cache[(h >> 20) & (kIdCacheSlots - 1)];
    int status;
    uint32_t tex;
    if (ce.generation == gen && ce.query == q && ce.lims == lims) {
        status = ce.status;
        tex = ce.tex;
    } else {
        HdTextureDrawQuery query{};
        query.page_x = (uint16_t)((texpage & 0xFu) * 64u);
        query.page_y = (uint16_t)(((texpage >> 4) & 1u) * 256u);
        query.depth = (uint8_t)depth;
        query.u_first = (uint8_t)u0; query.u_last = (uint8_t)u1;
        query.v_first = (uint8_t)v0; query.v_last = (uint8_t)v1;
        query.clut_x = clut_x; query.clut_y = clut_y;
        query.vram = vram; query.vram_word_count = kVramWords;
        HdTextureIdentity id{};
        status = hd_texture_identify(s.tracker, &query, &id);
        tex = id.texture_hash;
        ce.generation = gen; ce.query = q; ce.lims = lims; ce.status = status; ce.tex = tex;
    }
    uint32_t pal = 0;
    int slot = 0;
    if (status == HD_TEXTURE_LOOKUP_FOUND) {
        s.identified++;
        pal = hd_texture_hash_clut(vram, kVramWords, clut_x, clut_y, (uint8_t)depth);
        slot = surface_materials_lookup(tex, pal);
        if (slot) s.tagged++;
    }
    if (s.probe_on && (int)s.cur.size() < kMaxSamplesPerFrame) {
        Sample sm{};
        sm.x0 = (int16_t)x0; sm.y0 = (int16_t)y0; sm.x1 = (int16_t)x1; sm.y1 = (int16_t)y1;
        sm.texpage = texpage; sm.clut_x = clut_x; sm.clut_y = clut_y;
        sm.tex = tex; sm.pal = pal; sm.status = (int8_t)status; sm.slot = (uint8_t)slot;
        s.cur.push_back(sm);
    }
    return slot;
}

void surface_frame_end(void) {
    State &s = st();
    s.frames++;
    if (s.probe_on) {
        /* prev = the last frame that drew any textured prim, so a probe still
         * answers during fades/loading frames that draw nothing. */
        if (!s.cur.empty()) {
            s.prev.swap(s.cur);
            s.cur.clear();
        }
    } else if (!s.prev.empty() || !s.cur.empty()) {
        s.prev.clear();
        s.cur.clear();
    }
}

float surface_time_seconds(void) {
    /* Guest frames at the nominal 60 Hz field rate; the exact rate only
     * scales ripple speed and keeps the effect deterministic per frame. */
    return (float)((double)st().frames / 60.0);
}

int surface_probe_json(int vx, int vy, char *buf, int cap) {
    State &s = st();
    int at = 0;
    at = put(buf, cap, at, "\"probe\":%d,\"x\":%d,\"y\":%d,\"hits\":[", s.probe_on ? 1 : 0, vx, vy);
    int n = 0;
    /* Topmost first: later prims are drawn over earlier ones. */
    for (size_t i = s.prev.size(); i-- > 0 && n < 32;) {
        const Sample &sm = s.prev[i];
        if (vx < sm.x0 || vx > sm.x1 || vy < sm.y0 || vy > sm.y1) continue;
        at = put(buf, cap, at,
                 "%s{\"order\":%u,\"status\":\"%s\",\"key\":\"%08x-%08x\",\"material\":%u,"
                 "\"texpage\":%u,\"clut\":[%u,%u],\"bbox\":[%d,%d,%d,%d]}",
                 n ? "," : "", (unsigned)i,
                 sm.status == HD_TEXTURE_LOOKUP_FOUND ? "found" :
                 sm.status == HD_TEXTURE_LOOKUP_NONE ? "untracked" :
                 sm.status == HD_TEXTURE_LOOKUP_AMBIGUOUS ? "ambiguous" : "error",
                 sm.tex, sm.pal, sm.slot, sm.texpage, sm.clut_x, sm.clut_y,
                 sm.x0, sm.y0, sm.x1, sm.y1);
        n++;
    }
    at = put(buf, cap, at, "]");
    return at;
}

int surface_census_json(int limit, char *buf, int cap) {
    State &s = st();
    struct Agg { uint32_t tex, pal; int count; int x0, y0, x1, y1; int slot; long area; };
    std::unordered_map<uint64_t, Agg> agg;
    int untracked = 0;
    for (const Sample &sm : s.prev) {
        if (sm.status != HD_TEXTURE_LOOKUP_FOUND) { untracked++; continue; }
        auto &a = agg[key64(sm.tex, sm.pal)];
        if (a.count == 0) {
            a = Agg{sm.tex, sm.pal, 0, sm.x0, sm.y0, sm.x1, sm.y1, sm.slot, 0};
        }
        a.count++;
        a.x0 = std::min<int>(a.x0, sm.x0); a.y0 = std::min<int>(a.y0, sm.y0);
        a.x1 = std::max<int>(a.x1, sm.x1); a.y1 = std::max<int>(a.y1, sm.y1);
        a.area += (long)(sm.x1 - sm.x0 + 1) * (long)(sm.y1 - sm.y0 + 1);
    }
    std::vector<Agg> v;
    v.reserve(agg.size());
    for (auto &kv : agg) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), [](const Agg &a, const Agg &b) {
        return a.area != b.area ? a.area > b.area : key64(a.tex, a.pal) < key64(b.tex, b.pal);
    });
    if (limit <= 0) limit = 40;
    int at = 0;
    at = put(buf, cap, at, "\"probe\":%d,\"prims\":%d,\"untracked\":%d,\"unique\":%d,\"keys\":[",
             s.probe_on ? 1 : 0, (int)s.prev.size(), untracked, (int)v.size());
    for (int i = 0; i < (int)v.size() && i < limit; i++) {
        const Agg &a = v[(size_t)i];
        at = put(buf, cap, at,
                 "%s{\"key\":\"%08x-%08x\",\"prims\":%d,\"area\":%ld,\"material\":%d,\"bbox\":[%d,%d,%d,%d]}",
                 i ? "," : "", a.tex, a.pal, a.count, a.area, a.slot, a.x0, a.y0, a.x1, a.y1);
    }
    at = put(buf, cap, at, "]");
    return at;
}

int surface_status_json(char *buf, int cap) {
    State &s = st();
    int at = 0;
    at = put(buf, cap, at,
             "\"active\":%d,\"probe\":%d,\"source\":\"%s\",\"materials\":%d,\"rules\":%d,"
             "\"tracked_uploads\":%u,\"classified\":%llu,\"identified\":%llu,\"tagged\":%llu,"
             "\"highlight\":%s",
             g_surface_materials_active, s.probe_on ? 1 : 0, jesc(s.source).c_str(),
             s.mat_count, (int)s.rules.size(),
             (unsigned)hd_texture_pack_tracking_upload_count(s.tracker),
             (unsigned long long)s.classified, (unsigned long long)s.identified,
             (unsigned long long)s.tagged, s.highlight_on ? "true" : "false");
    if (s.highlight_on) {
        if (s.hl_any) at = put(buf, cap, at, ",\"highlight_key\":\"%08x-*\"", s.hl_tex);
        else at = put(buf, cap, at, ",\"highlight_key\":\"%08x-%08x\"", s.hl_tex, s.hl_pal);
    }
    at = put(buf, cap, at, ",\"list\":[");
    for (int i = 1; i <= s.mat_count; i++) {
        const SurfaceMaterial &m = s.mats[i];
        at = put(buf, cap, at, "%s{\"slot\":%d,\"name\":\"%s\",\"kind\":%d}", i > 1 ? "," : "",
                 i, jesc(m.name).c_str(), m.kind);
    }
    at = put(buf, cap, at, "]");
    return at;
}

} // extern "C"
