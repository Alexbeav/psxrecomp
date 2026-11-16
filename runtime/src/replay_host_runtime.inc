/* Player recorder host. Included after modern input/settings helpers so
 * callbacks share their actual production state instead of a parallel model. */
#include "replay_frame_host.hpp"
static ReplayFrameHost s_replay_frame_host;

static const char *g_replay_cli_path = nullptr;
static const char *g_replay_cli_verdict = nullptr;
static bool g_replay_cli_fast = false;
static std::string s_replay_disc_serial;
static std::string s_replay_saved_settings;
static char s_replay_export_dir[1024];
/* The launcher's "Record replay" for this process (launcher_record_replay.h). */
static bool g_launcher_record_replay = false;
/* The vblank replay_frame_boundary is running for (power-on replays start at 0). */
static uint64_t s_replay_vblank = UINT64_MAX;
/* Product identity for replays: the exe's SHA-256 and the loaded BIOS image's
 * CRC-32, hashed once (the digest thread or the first replay). */
static std::filesystem::path s_replay_exe_file, s_replay_bios_file;
static std::string s_replay_exe_sha256;
static uint32_t s_replay_bios_crc32 = 0;
static bool s_replay_bios_crc_ok = false;
static bool s_replay_product_hashed = false;

extern "C" void replay_host_osd(const char *text, int ms) {
    std::fprintf(stdout, "replay_osd: %s\n", text);
    std::fflush(stdout);
    host_osd_push(text, ms);
}

static int g_replay_scripted_record = 0;

extern "C" int replay_host_can_record(char *why, size_t cap) {
    char slot0[600];
    if (psx_netplay_active()) { std::snprintf(why, cap, "netplay is active"); return 0; }
    if (input_route_session_recording()) { std::snprintf(why, cap, "a route is recording"); return 0; }
    if (input_route_session_owns_ports() && !g_replay_scripted_record) {
        std::snprintf(why, cap, "an input route is playing"); return 0;
    }
    if (sio_get_multitap() || g_offline_pad_count > 2) { std::snprintf(why, cap, "a multitap is attached"); return 0; }
    if (psx_selfcheck_input_locked()) { std::snprintf(why, cap, "a deterministic self-check owns input"); return 0; }
    if (!memcard_replay_supported()) { std::snprintf(why, cap, "extended memory cards are not supported by this replay format"); return 0; }
    if (!savestate_slot_path(0, slot0, sizeof(slot0))) {
        std::snprintf(why, cap, "save states are not available for this game"); return 0;
    }
    if (psx_rewind_is_open()) { std::snprintf(why, cap, "rewind is open"); return 0; }
    /* Replays carry pad input only (PS1B-313): a mouse, neGcon or GunCon
     * would be neither recorded nor fed during playback. */
    for (int slot = 0; slot < 2; ++slot) {
        if (sio_get_port_device(slot) != SIO_DEVICE_PAD) {
            std::snprintf(why, cap, "port %d needs a pad or the keyboard", slot + 1);
            return 0;
        }
    }
    return 1;
}

/* The replay identity needs the SHA-256 of the whole disc image (~1.7 s for
 * RE3's 712 MB bin). A host thread hashes it at startup so the first F11 or
 * playback does not stall a frame; this mutex keeps the product identity and
 * the digest cache consistent between that thread and the frame path. */
static std::mutex s_replay_identity_mutex;
static std::thread s_replay_digest_thread;
static void replay_digest_thread_join(void) {
    disc_digest_cache_cancel(1);
    if (s_replay_digest_thread.joinable())
        s_replay_digest_thread.join();
}
extern "C" int replay_host_identity(InputRouteV3 *meta, char *why, size_t cap) {
    std::lock_guard<std::mutex> lock(s_replay_identity_mutex);
    return replay_identity_identity(meta, psx_bios_hle_enabled(),
                                        psx_bios_hle_boot_skip_enabled(), why, cap);
}

/* STOPGAP (PS1B-191): native and interpreted overlay code do not yet produce
 * identical guest cycles, and background compiles switch tiers at wall-clock
 * times, so a replay would depend on the overlay cache. Recording and playback
 * both run overlays interpreted from the anchor on; the tier returns when the
 * replay ends (replay_overlay_unpin_if_idle). The pin is the counted one a
 * netplay session also holds (overlay_interp_pin_acquire), so neither undoes
 * the other. */
static bool s_replay_overlay_pinned = false;
static bool replay_overlay_pin(void) {
    static const bool no_pin = [] {   /* diagnostic: measure without the pin */
        const char *e = std::getenv("PSX_REPLAY_NO_OVERLAY_PIN");
        return e && e[0] == '1';
    }();
    if (s_replay_overlay_pinned || no_pin)
        return true;
    s_replay_overlay_pinned = overlay_interp_pin_acquire(OVERLAY_TIER_REPLAY);
    return s_replay_overlay_pinned;
}
static void replay_overlay_unpin(void) {
    if (!s_replay_overlay_pinned) return;
    s_replay_overlay_pinned = false;
    overlay_interp_pin_release(OVERLAY_TIER_REPLAY);
}
static void replay_overlay_unpin_if_idle(void) {
    if (replay_session_state() == REPLAY_IDLE) replay_overlay_unpin();
}

extern "C" int replay_host_request_anchor(void) {
    const bool held = s_replay_overlay_pinned;
    if (!replay_overlay_pin()) return 0;
    if (!savestate_request_anchor()) {
        if (!held) replay_overlay_unpin();
        return 0;
    }
    return 1;
}
extern "C" int replay_host_take_anchor(uint8_t **data, size_t *size) {
    return savestate_take_anchor(data, size);
}
extern "C" int replay_host_load_anchor(const void *data, size_t size) {
    const bool held = s_replay_overlay_pinned;
    if (!replay_overlay_pin()) return 0;
    if (!savestate_request_load_blob_quiet(data, size)) {
        if (!held) replay_overlay_unpin();
        return 0;
    }
    return 1;
}
extern "C" int replay_host_take_load_result(void) {
    if (savestate_take_load_completed()) return 1;
    if (savestate_take_load_failed()) return -1;
    return 0;
}

/* The settings that change guest timing but are not in a save state. */
static std::string replay_mods_fingerprint(void) {
    std::string out;
    for (char c : PSXRecompV4::mod_runtime_fingerprint())
        if (c >= 0x21 && c <= 0x7e && out.size() < 200) out.push_back(c);
    return out.empty() ? std::string("none") : out;
}

/* PS1B-316 added what a power-on replay needs at vblank 0, where no anchor
 * restores it: the game's pending post-BIOS CD speed, the two options that
 * change guest behaviour (auto-skip FMV pokes RAM or holds Start; idle skip
 * advances the clock), and port 2 and both pad types as startup set them.
 *
 * game_entry_low_ram=kept says that this build leaves guest RAM 0..15 alone
 * at game entry. Builds before it zeroed those bytes there and wrote no such
 * line, so a recording without the line is one of theirs (PS1G-39).
 *
 * low_ram_stores=all says that this build lets every guest store to RAM
 * 0..15 land. Builds before it dropped some of them after game entry and
 * wrote no such line (PS1B-456). */
extern "C" void replay_host_settings_capture(char *out, size_t cap) {
    std::snprintf(out, cap,
                  "cd_speed=%d\ncd_instant_rate=%d\nturbo_loads=%d\nturbo_load_wall=%d\n"
                  "p1_connected=%d\np1_config_capable=%d\nmods=%s\n"
                  "cd_game_speed=%d\nauto_skip_fmv=%d\nidle_skip=%d\n"
                  "p1_analog=%d\np2_connected=%d\np2_config_capable=%d\np2_analog=%d\n"
                  "game_entry_low_ram=kept\nlow_ram_stores=all\n",
                  cdrom_get_speed(), cdrom_get_instant_rate(), g_turbo_loads_enabled,
                  g_turbo_load_wall_multiplier, sio_get_pad_connected(0),
                  sio_get_pad_config_capable(0), replay_mods_fingerprint().c_str(),
                  cdrom_get_game_speed(), g_auto_skip_fmv, psx_idle_skip_is_enabled(),
                  sio_get_pad_analog(0), sio_get_pad_connected(1),
                  sio_get_pad_config_capable(1), sio_get_pad_analog(1));
}

static void replay_settings_apply_text(const char *text, char *differs, size_t cap) {
    if (differs && cap) differs[0] = 0;
    const char *p = text ? text : "";
    while (*p) {
        const char *end = std::strchr(p, '\n');
        std::string line = end ? std::string(p, (size_t)(end - p)) : std::string(p);
        p = end ? end + 1 : p + line.size();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        const int v = std::atoi(value.c_str());
        bool ok = true;
        if (key == "cd_speed") cdrom_set_speed(v);
        else if (key == "cd_instant_rate") { if (v > 0) cdrom_set_instant_rate(v); }
        else if (key == "turbo_loads") g_turbo_loads_enabled = v ? 1 : 0;
        else if (key == "turbo_load_wall") g_turbo_load_wall_multiplier = v;
        else if (key == "p1_connected") sio_set_pad_connected(0, v ? 1 : 0);
        else if (key == "p1_config_capable") sio_set_pad_config_capable(0, v ? 1 : 0);
        else if (key == "mods") ok = value == replay_mods_fingerprint();
        else if (key == "cd_game_speed") cdrom_set_game_speed(v);
        else if (key == "auto_skip_fmv") g_auto_skip_fmv = v ? 1 : 0;
        else if (key == "idle_skip") { extern int g_idle_skip_enabled; g_idle_skip_enabled = v ? 1 : 0; }
        else if (key == "p2_connected") sio_set_pad_connected(1, v ? 1 : 0);
        else if (key == "p2_config_capable") sio_set_pad_config_capable(1, v ? 1 : 0);
        else if (key == "p1_analog" || key == "p2_analog") {
            /* A type switch resets the pad's config latch, so only a real
             * change is applied (normally none after an anchor load). */
            const int slot = key[1] - '1';
            if (sio_get_pad_analog(slot) != (v ? 1 : 0))
                sio_set_pad_analog(slot, v ? 1 : 0, 0x80, 0x80, 0x80, 0x80);
        }
        else if (key == "game_entry_low_ram") memory_set_low_ram_view_old(value != "kept");
        else if (key == "low_ram_stores") ok = value == "all";
        else ok = false;
        if (!ok && differs && cap) {
            const size_t n = std::strlen(differs);
            std::snprintf(differs + n, cap - n, "%s%s", n ? ", " : "",
                          key == "mods" ? "enabled mods" : key.c_str());
        }
    }
}

extern "C" void replay_host_settings_apply(const char *settings, char *differs, size_t cap) {
    char now[INPUT_ROUTE_REPLAY_SETTINGS_MAX + 1];
    replay_host_settings_capture(now, sizeof(now));
    s_replay_saved_settings = now;
    /* Older recordings cleared these bytes at entry. Select their comparison
     * view first; a current recording's explicit line switches it off. */
    memory_set_low_ram_view_old(1);
    replay_settings_apply_text(settings, differs, cap);
}

extern "C" void replay_host_settings_restore(void) {
    replay_settings_apply_text(s_replay_saved_settings.c_str(), nullptr, 0);
}

extern "C" int replay_host_thumb(uint32_t *out) {
    static_assert(REPLAY_THUMB_W == SAVESTATE_THUMB_W && REPLAY_THUMB_H == SAVESTATE_THUMB_H,
                  "replay thumbnails reuse the save-state size");
    savestate_render_thumb(out);
    return 1;
}
extern "C" const char *replay_host_game_title(void) { return s_picker_game_name.c_str(); }
extern "C" int replay_host_frame_rate(void) { return psx_video_timing_is_pal() ? 50 : 60; }

/* The legacy entry-clear view changes comparison bytes only, never guest RAM.
 * Older dropped-store semantics remain a reported settings difference. */
extern "C" const uint8_t *replay_host_ram(void) {
    extern uint8_t *g_psx_ram;
    const uint8_t *low = memory_low_ram_view();
    if (low == g_psx_ram) return g_psx_ram;
    static std::vector<uint8_t> view;
    view.assign(g_psx_ram, g_psx_ram + psx_ram_live_bytes());
    std::memcpy(view.data(), low, 16);
    return view.data();
}
extern "C" uint64_t replay_host_cycle(void) {
    extern uint64_t psx_cycle_count;
    return psx_cycle_count;
}
/* The rollback state digests (netplay_state_digest.h), so a replay checks the
 * same partitions a rollback resimulation is verified against. */
static CPUState *s_replay_cpu;
extern "C" int replay_host_state_digest(uint32_t out[4]) {
    static const bool no_digests = [] {   /* diagnostic: measure without digests */
        const char *e = std::getenv("PSX_REPLAY_NO_DIGESTS");
        return e && e[0] == '1';
    }();
    if (!s_replay_cpu || no_digests)
        return 0;
    const auto t0 = std::chrono::steady_clock::now();
    out[0] = netplay_core_digest(s_replay_cpu);
    const auto t1 = std::chrono::steady_clock::now();
    static int log_parts = -1;
    if (log_parts < 0) {
        const char *e = std::getenv("PSX_REPLAY_DIGEST_PARTS");
        log_parts = e && e[0] == '1';
    }
    if (log_parts) {   /* diagnostic: which core partition a divergence is in */
        NetplayCoreParts p;
        netplay_core_digest_parts(s_replay_cpu, &p);
        std::fprintf(stdout, "replay_digest_parts: cpu=%08x clock_irq=%08x timers=%08x ram=%08x "
                     "dirty=%08x pc=%08x\n", p.cpu, p.clock_irq, p.timers, p.ram, p.dirty,
                     s_replay_cpu->pc);
    }
    const auto t2 = std::chrono::steady_clock::now();
    out[1] = netplay_av_digest();
    const auto t3 = std::chrono::steady_clock::now();
    out[2] = netplay_aux_digest();
    const auto t4 = std::chrono::steady_clock::now();
    out[3] = netplay_baseline_ext_digest();
    const auto t5 = std::chrono::steady_clock::now();
    if (log_parts) {
        auto us = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
            return (long long)std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
        };
        std::fprintf(stdout, "replay_digest_us: core=%lld av=%lld aux=%lld ext=%lld\n",
                     us(t0, t1), us(t2, t3), us(t3, t4), us(t4, t5));
    }
    return 1;
}

/* Replay slots sit next to the save-state slots: state_<key>_slotNN.pst
 * becomes replay_<key>_slotNN.psxrpl. */
extern "C" int replay_host_slot_base(char *dir, size_t dir_cap, char *prefix, size_t prefix_cap) {
    char slot0[600];
    if (!savestate_slot_path(0, slot0, sizeof(slot0))) return 0;
    std::string path = slot0;
    const size_t sep = path.find_last_of("/\\");
    std::string name = sep == std::string::npos ? path : path.substr(sep + 1);
    const std::string folder = sep == std::string::npos ? std::string(".") : path.substr(0, sep);
    const size_t slot = name.rfind("_slot");
    if (slot == std::string::npos || name.compare(0, 6, "state_") != 0) return 0;
    name = "replay_" + name.substr(6, slot - 6);
    std::snprintf(dir, dir_cap, "%s", folder.c_str());
    std::snprintf(prefix, prefix_cap, "%s", name.c_str());
    return 1;
}

extern "C" const char *replay_host_export_dir(void) {
    const char *root = savestate_root_dir();
    if (!root || !root[0]) root = savestate_dir();
    std::snprintf(s_replay_export_dir, sizeof(s_replay_export_dir), "%s/replays", root ? root : ".");
    return s_replay_export_dir;
}

extern "C" const char *replay_host_disc_serial(void) {
    return s_replay_disc_serial.empty() ? "replay" : s_replay_disc_serial.c_str();
}

/* ---- Power-on replays (PS1B-316) ---- */

/* Vblank 0 of this process, unless a save state was staged for boot. */
extern "C" int replay_host_at_power_on(void) {
    static const bool load_slot = std::getenv("PSX_LOAD_SLOT") != nullptr;
#if defined(RECOMP_LAUNCHER_MODULE_HOST)
    return s_replay_vblank == 0 && !load_slot && g_launcher_start_slot < 0;
#else
    return s_replay_vblank == 0 && !load_slot;
#endif
}
extern "C" void replay_host_power_on_begin(void) {
    replay_overlay_pin();
}

static_assert(MEMCARD_SIZE == INPUT_ROUTE_REPLAY_CARD_BYTES, "replay card images are whole cards");
extern "C" int replay_host_cards_capture(uint8_t *images, uint32_t *mask) {
    *mask = 0;
    for (int card = 0; card < 2; ++card)
        if (memcard_export_raw(card, images + (size_t)card * MEMCARD_SIZE) == 0)
            *mask |= 1u << card;
    return 1;
}
extern "C" int replay_host_cards_install(const uint8_t *images, uint32_t mask) {
    return memcard_replay_begin(images, mask) == 0;
}
extern "C" void replay_host_cards_restore(void) {
    memcard_replay_end();
}

/* SHA-256 of the exe and CRC-32 of the BIOS image, once per process. Hashing
 * a large exe takes a moment, so the startup digest thread normally does it;
 * the caller holds s_replay_identity_mutex. */
static void replay_product_hash_locked(void) {
    if (s_replay_product_hashed) return;
    s_replay_product_hashed = true;
    std::vector<uint8_t> buf(1u << 20);
    std::ifstream exe(s_replay_exe_file, std::ios::binary);
    if (!s_replay_exe_file.empty() && exe) {
        psx_sha256_ctx ctx;
        uint8_t digest[32];
        char hex[65];
        psx_sha256_init(&ctx);
        while (exe.read(reinterpret_cast<char *>(buf.data()), (std::streamsize)buf.size()) || exe.gcount() > 0)
            psx_sha256_update(&ctx, buf.data(), (size_t)exe.gcount());
        if (exe.eof()) {
            psx_sha256_final(&ctx, digest);
            for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
            s_replay_exe_sha256 = hex;
        }
    }
    std::ifstream bios(s_replay_bios_file, std::ios::binary);
    if (!s_replay_bios_file.empty() && bios) {
        uint32_t crc = 0xFFFFFFFFu;
        while (bios.read(reinterpret_cast<char *>(buf.data()), (std::streamsize)buf.size()) || bios.gcount() > 0)
            crc = crc32_update(crc, buf.data(), (size_t)bios.gcount());
        s_replay_bios_crc32 = crc ^ 0xFFFFFFFFu;
        s_replay_bios_crc_ok = bios.eof();
    }
}

/* Hash the disc image (and, the first time, the exe and the BIOS) on a host
 * thread. Joined at exit, after cancelling the hash, so shutdown never races
 * it; a new start waits for the previous one first. */
static void replay_digest_prefetch_start(void) {
    static bool join_registered = false;
    if (s_replay_digest_thread.joinable())
        s_replay_digest_thread.join();
    s_replay_digest_thread = std::thread([] {
        std::lock_guard<std::mutex> lock(s_replay_identity_mutex);
        replay_identity_prefetch_disc_digest();
        replay_product_hash_locked();
    });
    if (!join_registered) {
        std::atexit(replay_digest_thread_join);
        join_registered = true;
    }
}

/* The replay identity names the disc in the drive (PS1B-316). Both mounts
 * that happen under a running game come here: the in-game disc change and a
 * save state that mounted its own disc. Without it a replay recorded after
 * the mount names the launch disc, so a later launch on that disc plays it
 * against the wrong image and a launch on the right disc is refused.
 * `serial` and `mount` are what a launch on this disc would pass to
 * input_route_session_set_product. */
static void replay_identity_follow_disc(const std::string& serial,
                                        const std::string& mount) {
    /* A hash of the previous disc still running is of no use: stop it, so the
     * frame does not wait for a whole image to be read. */
    disc_digest_cache_cancel(1);
    if (s_replay_digest_thread.joinable())
        s_replay_digest_thread.join();
    disc_digest_cache_cancel(0);
    {
        std::lock_guard<std::mutex> lock(s_replay_identity_mutex);
        replay_identity_set_disc(serial.c_str(), mount.c_str());
        input_route_session_set_disc(serial.c_str(), mount.c_str());
    }
    s_replay_disc_serial = serial;
    replay_digest_prefetch_start();
}

extern "C" void replay_host_product(char *out, size_t cap) {
    std::string lines;
    char line[96];
    {
        std::lock_guard<std::mutex> lock(s_replay_identity_mutex);
        replay_product_hash_locked();
        if (!s_replay_exe_sha256.empty()) lines += "exe_sha256=" + s_replay_exe_sha256 + "\n";
        std::snprintf(line, sizeof(line), "codegen=%08x\n", (unsigned)overlay_loader_codegen_hash());
        lines += line;
        if (s_replay_bios_crc_ok) {
            std::snprintf(line, sizeof(line), "bios_crc32=%08x\n", (unsigned)s_replay_bios_crc32);
            lines += line;
        }
    }
    lines += std::string("renderer=") +
             (g_vk_active ? "vulkan" : g_gl_active ? "opengl" : "software") + "\n";
    lines += std::string("platform=") + replay_platform_name() + "\n";
    if (const char *seed = std::getenv("PSX_REPLAY_TEST_INPUT_SEED");
        seed && seed[0] && g_replay_scripted_record) {
        std::snprintf(line, sizeof(line), "input_seed=%lu\n", std::strtoul(seed, nullptr, 10));
        lines += line;
    }
    std::snprintf(out, cap, "%s", lines.c_str());
}

/* P1 as the host would deliver it this frame: the armed route or debug
 * override when there is one, else the device. */
static void replay_live_p1(int override, uint16_t *buttons, uint8_t sticks[4]) {
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0x80u;
    *buttons = 0xFFFFu;
    /* Determinism test only: PSX_REPLAY_TEST_INPUT_SEED drives a scripted
     * recording with pseudo-random D-pad and face-button presses that change
     * every 8 frames (never Start/Select). */
    static int seed_read = 0;
    static uint32_t seed = 0, frame = 0;
    static uint16_t word = 0xFFFFu;
    if (!seed_read) {
        seed_read = 1;
        if (const char *e = std::getenv("PSX_REPLAY_TEST_INPUT_SEED")) seed = (uint32_t)std::strtoul(e, nullptr, 10);
    }
    if (seed && g_replay_scripted_record && replay_session_state() != REPLAY_IDLE &&
        replay_session_state() != REPLAY_PLAYING && replay_session_state() != REPLAY_LOADING) {
        if (frame++ % 8u == 0u) {
            seed = seed * 1664525u + 1013904223u;
            const uint16_t held = (uint16_t)((1u << (4u + (seed >> 28) % 4u)) |
                                             (1u << (12u + (seed >> 24) % 4u)));
            word = (seed >> 20) & 1u ? (uint16_t)~held : 0xFFFFu;
        }
        *buttons = word;
        return;
    }
    if (override >= 0) { *buttons = (uint16_t)override; return; }
    PsxNetPad pad;
    PadExtHooks hooks = pad_ext_main_hooks();
    if (!pad_ext_resolve(&hooks, 0, &pad)) return;
    *buttons = pad.buttons;
    sticks[0] = pad.lx; sticks[1] = pad.ly; sticks[2] = pad.rx; sticks[3] = pad.ry;
}

/* Once per vblank, after the route boundary: runs the scripted entry points
 * and the replay boundary. Replaces *override with the replay's P1 word. */
/* Diagnostic: PSX_REPLAY_FRAME_TIMES=FILE writes "vblank,us,state" per
 * vblank at exit, the host time between consecutive replay boundaries. */
static std::vector<uint64_t> s_replay_frame_times;   /* us << 8 | state */
static std::vector<uint64_t> s_replay_frame_cycles;  /* guest cycles << 1 | GPU PAL bit */
static std::string s_replay_frame_times_path;
static void replay_frame_times_write(void) {
    FILE *f = std::fopen(s_replay_frame_times_path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "vblank,us,state,cycles,pal\n");
    for (size_t i = 0; i < s_replay_frame_times.size(); ++i)
        std::fprintf(f, "%zu,%llu,%u,%llu,%u\n", i, (unsigned long long)(s_replay_frame_times[i] >> 8),
                     (unsigned)(s_replay_frame_times[i] & 0xFF),
                     (unsigned long long)(s_replay_frame_cycles[i] >> 1),
                     (unsigned)(s_replay_frame_cycles[i] & 1));
    std::fclose(f);
}
static void replay_frame_times_tick(void) {
    static int armed = -1;
    static std::chrono::steady_clock::time_point last;
    if (armed < 0) {
        const char *e = std::getenv("PSX_REPLAY_FRAME_TIMES");
        armed = e && e[0];
        if (armed) {
            s_replay_frame_times_path = e;
            s_replay_frame_times.reserve(1u << 16);
            s_replay_frame_cycles.reserve(1u << 16);
            std::atexit(replay_frame_times_write);
        }
        last = std::chrono::steady_clock::now();
        return;
    }
    if (!armed) return;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now - last).count();
    last = now;
    s_replay_frame_times.push_back(us << 8 | (uint64_t)replay_session_state());
    static uint64_t last_cycle;
    const uint64_t cyc = replay_host_cycle();
    s_replay_frame_cycles.push_back((cyc - last_cycle) << 1 | (uint64_t)(psx_video_timing_is_pal() ? 1 : 0));
    last_cycle = cyc;
}

static void replay_frame_boundary(int *override) {
    replay_frame_times_tick();
    host_osd_set_rec(replay_session_rec_visible((uint64_t)SDL_GetTicks()));
    auto &h = s_replay_frame_host;
    int &env_read=h.env_read, &exit_at_end=h.exit_at_end, &fast_play=h.fast_play, &fast_saved=h.fast_saved;
    const char *&play_path=h.play_path, *&record_file=h.record_file;
    long long &record_at=h.record_at, &record_frames=h.record_frames, &recorded=h.recorded;
    bool &record_boot=h.record_boot;
    std::string &boot_file=h.boot_file;
    uint64_t &vblanks=h.vblanks;
    ReplayState &previous=h.previous;
    if (!env_read) {
        env_read = 1;
        play_path = g_replay_cli_path ? g_replay_cli_path : std::getenv("PSX_REPLAY_FILE");
#if defined(RECOMP_LAUNCHER_MODULE_HOST)
        if (!g_replay_cli_path && !g_launcher_start_replay.empty())
            play_path = g_launcher_start_replay.c_str();
#endif
        if (play_path && !play_path[0]) play_path = nullptr;
        record_file = std::getenv("PSX_REPLAY_RECORD_FILE");
        if (record_file && !record_file[0]) record_file = nullptr;
        if (const char *e = std::getenv("PSX_REPLAY_RECORD_AT")) record_at = std::atoll(e);
        if (const char *e = std::getenv("PSX_REPLAY_RECORD_FRAMES")) record_frames = std::atoll(e);
        const char *x = std::getenv("PSX_REPLAY_EXIT_AT_END");
        exit_at_end = x && std::strcmp(x, "1") == 0;
        const char *verdict = g_replay_cli_verdict ? g_replay_cli_verdict : std::getenv("PSX_REPLAY_VERDICT");
        if (verdict && verdict[0]) replay_session_set_verdict_path(verdict);
        const char *fast = std::getenv("PSX_REPLAY_FAST");
        fast_play = g_replay_cli_fast || (fast && std::strcmp(fast, "1") == 0);
        /* Scripted: the environment asked (PSX_REPLAY_TEST_INPUT_SEED may then
         * drive P1); the launcher's checkbox records the player. */
        const char *boot_env = std::getenv("PSX_REPLAY_RECORD_BOOT");
        record_boot = !play_path && PSXRecompV4::record_boot_replay_requested(g_launcher_record_replay, boot_env);
        g_replay_scripted_record =
            record_file != nullptr || PSXRecompV4::record_boot_replay_requested(false, boot_env);
    }
    const uint64_t vb = vblanks++;
    s_replay_vblank = vb;
    if (play_path) {
        const char *path = play_path;
        play_path = nullptr;
        if (!replay_session_play_file(path) && exit_at_end) {
            std::fflush(stdout);
            psx_crash_trace_set_exit_origin("replay_not_played");
            std::exit(4);
        }
    }
    if (record_file && record_at >= 0 && (long long)vb == record_at) {
        if (!replay_session_record_to(record_file) && exit_at_end) {
            std::fflush(stdout);
            psx_crash_trace_set_exit_origin("replay_not_recorded");
            std::exit(5);
        }
    }
    if (record_boot && vb == 0) {
        /* <save root>/replays/<title>-boot-<UTC>.psxrpl: the save root is
         * anchored on the exe folder (PS1B-310), never the working directory. */
        char path[1024];
        const char *dir = replay_host_export_dir();
        const char *title = replay_host_game_title();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir), ec);
        const bool named = replay_session_boot_path(dir, title && title[0] ? title : replay_host_disc_serial(),
                                                    (int64_t)std::time(nullptr), path, sizeof(path));
        if (!named) replay_host_osd("Replay not recorded: no free file name in the replays folder", 2600);
        if (named && replay_session_record_power_on(path)) {
            boot_file = path;
        } else if (exit_at_end) {
            std::fflush(stdout);
            psx_crash_trace_set_exit_origin("replay_not_recorded");
            std::exit(5);
        }
    }
    const ReplayState state = replay_session_state();
    replay_overlay_unpin_if_idle();
    if (state == REPLAY_IDLE && previous == REPLAY_IDLE) return;
    uint16_t live, b;
    uint8_t live_sticks[4], s[4];
    replay_live_p1(*override, &live, live_sticks);
    if (replay_session_boundary(live, live_sticks, &b, s)) {
        g_replay_p1_buttons = b;
        std::memcpy(g_replay_p1_sticks, s, 4);
        *override = b;
        if ((record_file || !boot_file.empty()) && replay_session_state() == REPLAY_RECORDING &&
            record_frames > 0 && ++recorded == record_frames)
            replay_session_toggle_record();   /* ends on the next boundary */
    }
    const ReplayState now = replay_session_state();
    replay_overlay_unpin_if_idle();
    if (fast_play) {   /* latched fast-forward only while a replay plays */
        const bool playing = now == REPLAY_LOADING || now == REPLAY_PLAYING;
        if (playing && fast_saved < 0) {
            fast_saved = g_manual_turbo_latched;
            g_manual_turbo_latched = 1;
        } else if (!playing && fast_saved >= 0) {
            g_manual_turbo_latched = fast_saved;
            fast_saved = -1;
        }
    }
    if (exit_at_end && previous != REPLAY_IDLE && now == REPLAY_IDLE) {
        int status = 0;
        if (previous == REPLAY_RECORDING || previous == REPLAY_ARMING) {
            const char *want = record_file ? record_file : boot_file.empty() ? nullptr : boot_file.c_str();
            FILE *written = want ? std::fopen(want, "rb") : nullptr;
            status = written ? 0 : 5;
            if (written) std::fclose(written);
        } else {
            const ReplayResult r = replay_session_last_result();
            status = r == REPLAY_RESULT_IN_SYNC ? 0 : r == REPLAY_RESULT_OUT_OF_SYNC ? 3 : 4;
        }
        std::fflush(stdout);
        psx_crash_trace_set_exit_origin("replay_end");
        std::exit(status);
    }
    previous = now;
}

