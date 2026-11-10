/* White-box policy coverage against the real user/protocol request handlers.
 * Device/serializer stubs must never be reached; no slot files are written. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include "../src/savestate.c"

/* A failed headless check must report to stderr without a CRT dialog. */
static void policy_fail(const char *condition, int line) {
    fprintf(stderr, "FAIL policy line %d: %s\n", line, condition);
    exit(1);
}
#undef assert
#define assert(condition) ((condition) ? (void)0 : policy_fail(#condition, __LINE__))

static int refusals;
static char refusal_reason[128];
static int snapshot_ready = 1;
static int snapshot_ready_calls;
static int mod_callback, override_callback, replacement_callback;
static unsigned host_depth;
int g_psx_guest_time_frozen;
static int context_safe = 1;
static int anchor_fixture, anchor_load_fail, anchor_saves, anchor_loads, anchor_rollbacks;
static jmp_buf anchor_resume;
int psx_mod_snapshot_ready(void) { ++snapshot_ready_calls; return snapshot_ready; }
void psx_frontend_on_savestate_refused(int load, int slot, const char* reason) {
    (void)load; (void)slot;
    ++refusals;
    snprintf(refusal_reason, sizeof(refusal_reason), "%s", reason);
}
int psx_netplay_active(void) { return 0; }
int psx_netplay_is_host(void) { return 1; }
int psx_hle_scheduler_enabled(void) { return 1; }
/* Match the current provider's union, including its already integrated guards. */
int psx_mod_function_entry_active(void) {
    return mod_callback || override_callback || replacement_callback;
}
unsigned psx_snapshot_host_call_depth(void) { return host_depth; }
int psx_is_dispatchable(uint32_t pc) { return pc != 0; }
uint32_t psx_last_irq_check_pc(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc(void) { return 0; }
uint32_t psx_irq_resume_context_snapshot_pc(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
int psx_irq_resume_context_snapshot_safe_at(uint32_t pc) {
    (void)pc; return context_safe && !host_depth && !psx_mod_function_entry_active();
}
int psx_irq_resume_context_snapshot_site(void) { return 0; }
void interrupts_resync_after_restore(void) { assert(anchor_fixture); }
void psx_cycles_resync_after_restore(struct CPUState* cpu) { (void)cpu; assert(anchor_fixture); }
void cdrom_accelerate_after_savestate(void) { assert(anchor_fixture); }
void psx_scheduler_resume_at(uint32_t pc) { assert(anchor_fixture && pc == 0x80010000); longjmp(anchor_resume,1); }
void psx_frontend_on_savestate_loaded(void) { assert(anchor_fixture); }
void psx_frontend_on_savestate_notify(int load, int slot, int ok) {
    (void)load; (void)slot; (void)ok; assert(0);
}
void gpu_get_display_info(GpuDisplayInfo* out) { (void)out; assert(0); }
int gpu_hd_textures_residency_save(uint8_t **data, size_t *size) {
    (void)data; (void)size; assert(0); return 0;
}
int gpu_hd_textures_residency_load(const uint8_t *data, size_t size) {
    (void)data; (void)size; assert(0); return 0;
}
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo* out, uint32_t x, uint32_t y) {
    (void)out; (void)x; (void)y; assert(0); return 0;
}
int boot_state_save(const CPUState* cpu, uint32_t bios, uint32_t entry, const char* path) {
    (void)cpu; (void)bios; (void)entry; (void)path; assert(0); return 0;
}
int boot_state_load_checked(const char* path, uint32_t bios, uint32_t entry,
                           CPUState* cpu, BootStateResumeAccept accept) {
    (void)path; (void)bios; (void)entry; (void)cpu; (void)accept; assert(0); return 0;
}
/* This fixture's declared buffer wire is one CPUState, not a real PST. */
int boot_state_peek_cpu_pc_buffer(const uint8_t* data, size_t size, uint32_t* pc) {
    CPUState saved;
    if (!anchor_fixture || !data || !pc || size != sizeof(saved)) return 0;
    memcpy(&saved, data, sizeof(saved));
    *pc = saved.pc;
    return 1;
}
int boot_state_load_buffer_checked(const uint8_t* data, size_t size, uint32_t bios,
                            uint32_t entry, CPUState* cpu, BootStateResumeAccept accept) {
    (void)bios; (void)entry; assert(anchor_fixture && size == sizeof(*cpu));
    ++anchor_loads;
    memcpy(cpu,data,size);
    if (anchor_load_fail) { cpu->pc = 0xDEADC0DE; return 0; }
    return accept(cpu->pc);
}
int boot_state_load_buffer_quiet(const uint8_t* data, size_t size, uint32_t bios,
                                 uint32_t entry, CPUState* cpu) {
    (void)bios; (void)entry; assert(anchor_fixture && size == sizeof(*cpu));
    ++anchor_rollbacks; memcpy(cpu,data,size); return 1;
}
int boot_state_save_buffer(const CPUState* cpu, uint32_t bios, uint32_t entry,
                          uint8_t** data, size_t* size) {
    (void)bios; (void)entry; assert(anchor_fixture); ++anchor_saves;
    *size = sizeof(*cpu); *data = malloc(*size); assert(*data);
    memcpy(*data,cpu,*size); return 1;
}
int boot_state_save_buffer_raw(const CPUState* cpu, uint32_t bios, uint32_t entry,
                                uint8_t** data, size_t* size) {
    if (!anchor_fixture) {
        fputs("UNSAFE_SERIALIZER_TOUCH=boot_state_save_buffer_raw\n", stderr);
        exit(77);
    }
    (void)bios; (void)entry; assert(anchor_fixture);
    *size = sizeof(*cpu); *data = malloc(*size); assert(*data);
    memcpy(*data,cpu,*size); return 1;
}
int boot_state_check_buffer(const uint8_t* data, size_t size, uint32_t bios,
                             uint32_t entry, char* reason, size_t cap) {
    (void)data; (void)size; (void)bios; (void)entry; (void)reason; (void)cap;
    assert(0); return 0;
}

static void check_pending_status(int pending) {
    PSXSavestateStatus status;
    char json[512], expected[32];
    savestate_get_status(&status);
    assert(status.pending == pending);
    savestate_status_json(json, sizeof(json));
    snprintf(expected, sizeof(expected), "\"pending\":%d", pending);
    assert(strstr(json, expected));
    if (!pending) assert(strstr(json, "\"wait\":\"\""));
}

int main(void) {
    static CPUState cpu;
    const uint8_t blob = 1;
    static const PSXModNetplayProfile profile = {
        "test.simulation", "test-no-snapshots-v1", 0, 0, 0, 1u, NULL, NULL, 0
    };
    memset(&cpu, 0, sizeof(cpu));
    s_configured = 1; /* No configure/filesystem needed to test request staging. */
    {
        char quick[700], manual[700], sidecar[700];
        assert(savestate_slot_path(SAVESTATE_QUICK_SLOT, quick, sizeof quick));
        assert(strstr(quick, "_quick.pst"));
        for (int slot = 0; slot < SAVESTATE_SLOTS; ++slot) {
            assert(savestate_slot_path(slot, manual, sizeof manual));
            assert(strcmp(manual, quick));
        }
        assert(savestate_thumb_path(SAVESTATE_QUICK_SLOT, sidecar, sizeof sidecar));
        assert(strstr(sidecar, "_quick.thumb"));
        assert(savestate_request_save(SAVESTATE_QUICK_SLOT) &&
               s_save_pending == SAVESTATE_QUICK_SLOT);
        assert(savestate_request_load(SAVESTATE_QUICK_SLOT) &&
               s_load_pending == SAVESTATE_QUICK_SLOT);
        savestate_set_title_blocked(1);
        assert(!savestate_pending());
        assert(!savestate_request_save(SAVESTATE_QUICK_SLOT));
        assert(!savestate_request_load(SAVESTATE_QUICK_SLOT));
        savestate_set_title_blocked(0);
        assert(!savestate_slot_path(SAVESTATE_QUICK_SLOT + 1, quick, sizeof quick));
        assert(!savestate_request_save(SAVESTATE_QUICK_SLOT + 1));
        refusals = 0;
    }
    assert(!savestate_title_blocked());
    assert(savestate_request_save(2) && savestate_pending());
    check_pending_status(1);
    snapshot_ready = 0;
    savestate_poll(&cpu, 0x80010000); /* Defer without touching the serializer. */
    assert(savestate_pending() && s_save_pending == 2);
    snapshot_ready = 1;
    context_safe = 0;
    savestate_poll(&cpu, 0x80010000);
    assert(s_save_pending == 2 && savestate_pending());
    context_safe = 1;
    savestate_set_title_blocked(1);
    assert(savestate_title_blocked() && !savestate_pending());
    check_pending_status(0);
    assert(savestate_take_save_failed());
    assert(!savestate_request_save(2));
    assert(!savestate_request_load(2));
    assert(!savestate_request_save_protocol(2));
    assert(!savestate_request_load_protocol(2));
    assert(!savestate_request_load_blob_protocol(&blob, 1));
    assert(refusals == 5 && strstr(refusal_reason, "game mode"));
    savestate_poll(&cpu, 0x80010000); /* No serializer or device call. */
    savestate_set_title_blocked(0);
    assert(savestate_request_load_blob_protocol(&blob, 1));
    assert(s_load_blob && s_load_blob_len == 1 && savestate_pending());
    check_pending_status(1);
    snapshot_ready = 0;
    savestate_poll(&cpu, 0x80010000);
    assert(s_load_blob && s_load_blob_len == 1 && savestate_pending());
    snapshot_ready = 1;
    context_safe = 0;
    savestate_poll(&cpu, 0x80010000);
    assert(s_load_blob && s_load_blob_len == 1 && savestate_pending());
    context_safe = 1;
    savestate_set_title_blocked(1);
    assert(!s_load_blob && s_load_blob_len == 0 && !savestate_pending());
    check_pending_status(0);
    assert(savestate_take_load_failed());
    savestate_set_title_blocked(0); /* The next session resets before activation. */
    assert(savestate_request_save_protocol(3));
    assert(savestate_request_load_protocol(3));
    savestate_set_title_blocked(1); /* Clear both staged operations. */
    assert(!savestate_pending());
    check_pending_status(0);
    savestate_set_title_blocked(0);
    assert(psx_mod_register_netplay_profile(&profile));
    assert(savestate_request_save_protocol(1)); /* Registered, inactive: stock. */
    savestate_set_title_blocked(1);
    savestate_set_title_blocked(0);
    psx_mod_netplay_set_active(1);
    assert(!savestate_request_save_protocol(1));
    assert(!savestate_request_load_protocol(1));
    assert(!savestate_request_load_blob_protocol(&blob, 1));
    assert(strstr(refusal_reason, "netplay mode"));
    psx_mod_netplay_set_active(0);
    assert(savestate_request_save_protocol(1));
    savestate_set_title_blocked(1);
    assert(!savestate_pending());
    /* Isolated scopes affect slot sidecars, never the save root. Formatting
     * only: no files or device serializers are touched here. */
    {
        char shared[700], scoped[700], thumb[700], hd[700], tiny[16];
        const char* first = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        const char* second = "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        savestate_set_title_blocked(0);
        s_entry_pc = 0x80010000u;
        strcpy(s_dir, "saves/openbios");
        strcpy(s_root, "saves");
        assert(savestate_set_content_scope(NULL));
        assert(savestate_slot_path(2, shared, sizeof(shared)));
        assert(!strcmp(shared, "saves/openbios/state_80010000_slot02.pst"));
        assert(savestate_request_load_blob_protocol(&blob, 1));
        assert(savestate_set_content_scope(first));
        assert(!savestate_pending() && !s_load_blob && savestate_take_load_failed());
        assert(savestate_slot_path(2, scoped, sizeof(scoped)));
        assert(strstr(scoped, first) && strcmp(shared, scoped));
        assert(!strcmp(savestate_root_dir(), "saves") && !strcmp(savestate_dir(), "saves/openbios"));
        assert(savestate_thumb_path(2, thumb, sizeof(thumb)) && strstr(thumb, first));
        assert(savestate_hd_path(2, hd, sizeof(hd)) && strstr(hd, first));
        assert(strstr(thumb, ".thumb") && strstr(hd, ".hdres"));
        assert(!savestate_slot_path(2, tiny, sizeof(tiny)) && !tiny[0]);
        savestate_set_disc_scope(2);
        assert(savestate_slot_path(2, scoped, sizeof(scoped)) && strstr(scoped, "_disc2_mods_"));
        savestate_set_disc_scope(0);
        assert(savestate_request_save(2));
        s_save_defer_slot = 3;
        assert(savestate_set_content_scope(second));
        assert(!savestate_pending() && s_save_defer_slot == -1 && savestate_take_save_failed());
        assert(savestate_slot_path(2, scoped, sizeof(scoped)) && strstr(scoped, second));
        assert(!savestate_set_content_scope("../invalid"));
        assert(!savestate_slot_path(2, scoped, sizeof(scoped)) && !scoped[0]);
        assert(!savestate_request_save(2) && !savestate_request_load(2));
        assert(!savestate_request_load_blob_protocol(&blob, 1));
        assert(strstr(refusal_reason, "verified content plan"));
        assert(savestate_set_content_scope(""));
        assert(savestate_slot_path(2, scoped, sizeof(scoped)) && !strcmp(shared, scoped));
    }
    /* Real cold poller: load-first reproduces the missing common guard without
     * pretending the save-only PC helper lacks its existing depth check. */
    {
        unsigned checked = 0;
        const int operations[] = {1, 0, 2, 3};
        for (int guard = 0; guard < 6; guard++) {
            mod_callback = guard == 0;
            host_depth = guard == 1 ? 2u : 0u;
            override_callback = guard == 2;
            g_psx_guest_time_frozen = guard == 3;
            replacement_callback = guard == 4;
            snapshot_ready = guard != 5;
            for (unsigned n = 0; n < sizeof operations / sizeof operations[0]; n++) {
                int operation = operations[n];
                savestate_set_title_blocked(0);
                if (operation == 0 || operation == 3)
                    assert(savestate_request_save_protocol(2));
                if (operation == 1 || operation == 3)
                    assert(savestate_request_load(2));
                if (operation == 2)
                    assert(savestate_request_load_blob_protocol(&blob, 1));
                unsigned generation = s_status_generation;
                uint8_t *queued_blob = s_load_blob;
                int readiness_before = snapshot_ready_calls;
                printf("defer-policy guard=%d operation=%d\n", guard, operation);
                fflush(stdout);
                savestate_poll(&cpu, 0x80010000u);
                assert((operation == 0 || operation == 3) ? s_save_pending == 2 : s_save_pending < 0);
                assert(operation == 0 ? s_load_pending < 0 : s_load_pending >= 0);
                assert(s_status_generation == generation && savestate_pending());
                assert(s_load_blob == queued_blob);
                const int mod_busy = mod_callback || override_callback || replacement_callback;
                const char *expected = mod_busy ? "mod_callback" : guard == 5 ? "mod_snapshot" :
                    guard == 1 ? "resume_pc" : "host_continuation";
                assert(!strcmp(s_status_wait, expected));
                assert(snapshot_ready_calls - readiness_before == ((guard == 1 || guard == 5) ? 1 : 0));
                checked++;
                savestate_set_title_blocked(1); /* Cancel without slot I/O. */
            }
        }
        mod_callback = override_callback = replacement_callback = g_psx_guest_time_frozen = 0;
        host_depth = 0; snapshot_ready = context_safe = 1;
        savestate_set_title_blocked(0);
        printf("STATE_DEFER_POLICY_CASES=%u\n", checked);
    }
    /* Replay anchors retain all modern safety checks and never touch slots.
     * Unsafe execution serializes nothing; title cancellation releases them. */
    {
        uint8_t* anchor = NULL; size_t length = 0;
        anchor_fixture = 1;
        savestate_set_title_blocked(0);
        cpu.pc = 0x80010000;
        assert(savestate_request_anchor());
        assert(!savestate_request_anchor());
        assert(!savestate_request_save(2));
        assert(!savestate_request_load_protocol(2));
        snapshot_ready = 0;
        savestate_poll(&cpu,cpu.pc);
        assert(anchor_saves == 0 && savestate_pending());
        snapshot_ready = 1; context_safe = 0;
        savestate_poll(&cpu,cpu.pc);
        assert(anchor_saves == 0 && savestate_pending());
        savestate_set_title_blocked(1);
        assert(!savestate_pending() && savestate_take_anchor(&anchor,&length) == -1);
        assert(!anchor && !length);
        savestate_set_title_blocked(0); context_safe = 1;
        assert(savestate_request_anchor());
        assert(savestate_take_anchor(&anchor,&length) == 0);
        g_psx_guest_time_frozen = 1;
        savestate_poll(&cpu,cpu.pc);
        assert(anchor_saves == 0 && anchor_loads == 0 && savestate_pending());
        assert(!strcmp(s_status_wait, "host_continuation"));
        g_psx_guest_time_frozen = 0;
        if (!setjmp(anchor_resume)) { savestate_poll(&cpu,cpu.pc); assert(0); }
        assert(anchor_saves == 1 && anchor_loads == 1 && anchor_rollbacks == 0);
        puts("STATE_DEFER_QUEUED_ANCHOR_RESUMED=1");
        assert(savestate_take_anchor(&anchor,&length) == 1 && length == sizeof(cpu));
        free(anchor); anchor = NULL; length = 0;
        assert(!savestate_pending());
        anchor_load_fail = 1;
        assert(savestate_request_anchor());
        savestate_poll(&cpu,cpu.pc);
        assert(anchor_saves == 2 && anchor_loads == 2 && anchor_rollbacks == 1);
        assert(cpu.pc == 0x80010000 && !savestate_pending());
        assert(savestate_take_anchor(&anchor,&length) == -1 && !anchor && !length);
        assert(!s_anchor_blob && !s_load_blob && !s_anchor_wait_load);
    }
    printf("STATE_DEFER_SAFE_ANCHOR saves=%d loads=%d rollbacks=%d\n",
           anchor_saves, anchor_loads, anchor_rollbacks);
    puts("savestate title policy and replay anchor tests passed");
    return 0;
}


int boot_state_peek_cpu_pc(const char *path,uint32_t *pc) {(void)path;(void)pc;assert(0);return 0;}
uint32_t psx_rewind_discard_history(void) {return 0;}
