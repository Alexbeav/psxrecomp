#!/usr/bin/env python3
"""Structural regression checks for interpreter fallback hot-path guards."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def body(source, name):
    match = re.search(rf"\b(?:static\s+)?(?:inline\s+)?(?:int|void|uint8_t|uint16_t|uint32_t|uint64_t|DWORD|OverlayPreparedImage|PublishItem)\s*\*?\s*(?:WINAPI\s+)?{name}\s*\([^;]*?\)\s*\{{",
                      source, re.S)
    if not match:
        raise AssertionError(f"missing function {name}")
    start, depth = match.end(), 1
    for pos in range(start, len(source)):
        depth += source[pos] == "{"
        depth -= source[pos] == "}"
        if depth == 0:
            return source[start:pos]
    raise AssertionError(f"unterminated function {name}")


# C and C++ comments, kept apart from string and character literals so that a
# code-generator template (a string) is still scanned while a comment is not.
_C_TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|R"([^()\\\s]{0,16})\(.*?\)\1"'
    r'|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def code_without_comments(text):
    return _C_TOKENS.sub(lambda m: " " if m.group(0)[0] == "/" else m.group(0), text)


def removed_cycle_limit_references():
    """Sources whose code (not comments) names the inline cycle limit PS1B-507 removed.

    Covers the runtime, its unit tests and the code generator, whose emitted C lives in
    string templates. A restored limit must come back on purpose: with this guard
    changed, a writer at every deadline change, and the timing proof PS1B-507 asked for.
    """
    found = []
    for directory in ("runtime/src", "runtime/include", "runtime/tests", "recompiler/src"):
        if not (ROOT / directory).is_dir():
            continue
        for path in sorted((ROOT / directory).rglob("*")):
            if path.suffix not in (".c", ".cc", ".cpp", ".h", ".hpp", ".inc") or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if "g_psx_cycle_fast_limit" not in text:
                continue
            if re.search(r"\bg_psx_cycle_fast_limit\b", code_without_comments(text)):
                found.append(path.relative_to(ROOT).as_posix())
    return found


def main():
    interp = (ROOT / "runtime/src/dirty_ram_interp.c").read_text(encoding="utf-8")
    capture = (ROOT / "runtime/src/overlay_capture.c").read_text(encoding="utf-8")
    loader = (ROOT / "runtime/src/overlay_loader.c").read_text(encoding="utf-8")
    autocompile = (ROOT / "runtime/src/autocompile.c").read_text(encoding="utf-8")
    memory = (ROOT / "runtime/src/memory.c").read_text(encoding="utf-8")
    gte = (ROOT / "runtime/src/gte.cpp").read_text(encoding="utf-8")
    traps = (ROOT / "runtime/src/traps.c").read_text(encoding="utf-8")
    interrupts = (ROOT / "runtime/src/interrupts.c").read_text(encoding="utf-8")
    cycles = (ROOT / "runtime/src/psx_cycles.c").read_text(encoding="utf-8")
    cyc_header = (ROOT / "runtime/include/psx_cyc.h").read_text(encoding="utf-8")
    cycles_header = (ROOT / "runtime/include/psx_cycles.h").read_text(encoding="utf-8")
    instr_cost = (ROOT / "runtime/include/psx_instr_cost.h").read_text(encoding="utf-8")
    starvation = (ROOT / "runtime/include/starvation_ring.h").read_text(encoding="utf-8")

    record = body(interp, "exec_pc_table_record")
    if "uint32_t *slot = &g_dirty_ram_exec_pc_bitmap[word >> 5]" not in record:
        raise AssertionError("executed-PC recording is not a direct bitmap OR")
    if "pc_table_get_or_insert" in record:
        raise AssertionError("executed-PC recording regressed to hash probing")
    if "sizeof(g_dirty_ram_exec_pc_bitmap)" not in capture:
        raise AssertionError("autocapture does not snapshot the 64 KiB bitmap")
    write_window = body(capture, "write_json_window")
    if ("size += 4u" not in write_window or
            "phys + size <= ram_size - 4u" not in write_window or
            "window_hi" in write_window):
        raise AssertionError(
            "capture delay-slot guard is missing or stops at an artificial window boundary")
    if "g_dirty_ram_exec_pc_table" in capture:
        raise AssertionError("capture still references the removed execution table")
    before_dma = body(capture, "overlay_capture_before_dma")
    commit = before_dma.find("preserve_snapshot_async(evidence_lo, evidence_hi)")
    clear_exec = before_dma.find("memset(&g_dirty_ram_exec_pc_bitmap", commit)
    clear_dispatch = before_dma.find(
        "memset(&g_dirty_ram_dispatch_pc_bitmap", commit)
    if min(commit, clear_exec, clear_dispatch) < 0 or not commit < clear_exec < clear_dispatch:
        raise AssertionError("outgoing variant is not queued before evidence reset")
    if "first_page = lo >> page_shift" not in before_dma:
        raise AssertionError("pre-DMA capture is not scoped to touched whole pages")
    for guard in ("first_page - 1u", "last_page + 1u", "exec_pc_bitmap"):
        if guard not in capture:
            raise AssertionError(f"pre-DMA capture lost executed-page halo: {guard}")
    autocap_tick = body(capture, "overlay_autocapture_tick")
    failed_write = autocap_tick.find("if (!sig && job)")
    clear_owner = autocap_tick.find("s_autocap_write_job = NULL", failed_write)
    retry_pending = autocap_tick.find("if (s_autocap_write_job)", clear_owner)
    enabled_gate = autocap_tick.find("if (!s_autocap_enabled || !s_active)")
    if min(failed_write, clear_owner, retry_pending, enabled_gate) < 0 or not (
            failed_write < clear_owner < retry_pending < enabled_gate):
        raise AssertionError("failed periodic capture is not retained and retried before new epochs")
    failed_branch = autocap_tick[failed_write:clear_owner]
    for required in ("job->attempts++", "job->retry_frame", "return;"):
        if required not in failed_branch:
            raise AssertionError("periodic capture failure can discard its immutable evidence epoch")
    retry_branch = autocap_tick[retry_pending:enabled_gate]
    if ("SDL_AtomicGet(&s_autocap_write_state) == 0" not in retry_branch or
            "autocap_write_launch(job)" not in retry_branch or
            "autocap_write_job_free" in retry_branch or "memset(" in retry_branch):
        raise AssertionError("periodic capture retry mutates or frees newer live evidence")
    provider_try = body(capture, "autocap_provider_request_try")
    accepted = provider_try.find("if (cp->request && cp->request())")
    accepted_sig = provider_try.find("s_autocap_sig_at_req =", accepted)
    retry_sig = provider_try.find("s_autocap_provider_sig_pending = 0", accepted_sig)
    retry_delay = provider_try.find("s_autocap_provider_retry_frame = frame +", retry_sig)
    if min(accepted, accepted_sig, retry_sig, retry_delay) < 0 or not (
            accepted < accepted_sig < retry_sig < retry_delay):
        raise AssertionError("compiler spawn failure can mark durable evidence as requested")
    if "s_autocap_provider_sig_pending = sig" not in autocap_tick:
        raise AssertionError("durable capture is not retained until provider request acceptance")
    wait_pending = body(capture, "overlay_capture_wait_pending")
    if "preserve_snapshot_enqueue_owned(job)" not in wait_pending:
        raise AssertionError("shutdown drops a failed periodic capture instead of draining it")
    dispatch_inner = body(interp, "dirty_ram_dispatch_inner")
    if "(!current_page_dirty || next_page != current_page)" not in dispatch_inner:
        raise AssertionError("page fast path does not preserve clean-miss behavior")
    # Load values are carried across executor boundaries for every PC (PS1B-220,
    # docs/ps1b-220-load-delay.md); the earlier BIOS-only scope is gone. The
    # interpreter keeps the old GPR value visible to the next instruction and
    # arms the pending value only after the load itself executed.
    if "return exec_one_fetched_context(" not in body(interp, "exec_one_fetched"):
        raise AssertionError("interpreter steps bypass the load-value context")
    exec_ctx = body(interp, "exec_one_fetched_context")
    value_begin = exec_ctx.find("psx_load_value_begin(cpu);")
    value_cancel = exec_ctx.find("psx_load_value_cancel(cpu, delayed_value_writer(cpu,insn))")
    inner = exec_ctx.find("exec_one_fetched_inner(cpu, pc, insn, next_pc_out)")
    restore = exec_ctx.find("cpu->gpr[ld_rt] = before;", inner)
    value_arm = exec_ctx.find("psx_load_value_arm(cpu,ld_rt,value);", inner)
    if min(value_begin, value_cancel, inner, restore, value_arm) < 0 or not (
            value_begin < value_cancel < inner < restore < value_arm):
        raise AssertionError("load-delay value is not carried across the interpreter step")
    # RFE backend-contract parity: the recompiled rfe and every overlay shard
    # (ABI v12) call psx_rfe_mark_escape after popping SR, and the generated
    # trampoline runs psx_rfe_escape_check after each return. The interpreter
    # must do BOTH — mark at its RFE case and take the escape at the committed
    # transfer — or an interpreted handler whose EPC lands in dirty RAM keeps
    # interpreting inside psx_check_interrupts' window and in_exception never
    # clears (vblank vetoed forever: the dbg-build boot livelock).
    if "psx_rfe_mark_escape();" not in interp:
        raise AssertionError("interpreter RFE does not honor the rfe escape contract")
    rfe_take = dispatch_inner.find("psx_rfe_escape_check(cpu);")
    local_flow = dispatch_inner.find("allow_local_dirty_flow && target != 0")
    if rfe_take < 0 or local_flow < 0 or rfe_take > local_flow:
        raise AssertionError(
            "interpreter does not take a pending rfe escape before local dirty flow")

    dispatch = body(loader, "overlay_loader_dispatch")
    cached = dispatch.find("lazy_miss_cached(phys)")
    lookup = dispatch.find("idx_head(phys)")
    record_miss = dispatch.rfind("lazy_miss_record(phys)")
    if min(cached, lookup, record_miss) < 0 or not cached < lookup < record_miss:
        raise AssertionError("negative miss cache is not guarding lookup/recorded at final miss")
    if "lazy_miss_invalidate_loader();" not in body(loader, "overlay_loader_rescan"):
        raise AssertionError("cache rescan does not invalidate negative misses")
    if "lazy_miss_invalidate_loader();" not in body(loader, "load_one_dll_prepared"):
        raise AssertionError("DLL publication does not invalidate negative misses")
    lazy_match = body(loader, "lazy_man_matches")
    if "man_delay_slots_hashed(&lm->fn)" not in lazy_match:
        raise AssertionError("legacy manifests can expose unhashed delay slots")
    rebuild = body(loader, "rebuild_lazy_manifest_index")
    if "overlay_watch_set_range(lo, lm->fn.len[r])" not in rebuild:
        raise AssertionError("unloaded manifest pages are not generation-watched")
    if "manifest_ok = 1" not in rebuild:
        raise AssertionError("strictly parsed manifests are not marked usable")
    if "cache_idx_has_basename" in loader:
        raise AssertionError("immutable repair artifacts are still deduped by logical name")
    scan_one = body(loader, "scan_one_cache_dir")
    for guard in ("cache_name_is_immutable(fd.cFileName)",
                  "cache_idx_has_path(full)", "e->logical_crc = crc",
                  "e->tier = (uint8_t)tier"):
        if guard not in scan_one:
            raise AssertionError(f"artifact-aware cache indexing lost: {guard}")
    has_crc = body(loader, "overlay_loader_has_cached_crc")
    for guard in ("manifest_ok", "!s_cache_idx[i].load_failed",
                  "logical_crc == crc", "lazy_man_crc(",
                  "man_delay_slots_hashed("):
        if guard not in has_crc:
            raise AssertionError(f"cached-CRC query accepts unusable artifacts: {guard}")
    try_load = body(loader, "try_load_region")
    if "lazy_is_loadable(li, region_start, phys, 0)" not in try_load:
        raise AssertionError("exact manifest entries are still gated by dirty-run base")
    if "lazy_is_loadable(li, region_start, phys, 1)" not in try_load:
        raise AssertionError("non-exact range fallback lost region narrowing")
    if "lazy_candidate_preferred" not in try_load:
        raise AssertionError("lazy artifact selection is still directory-order dependent")
    for guard in ("load_failed", "goto retry_artifact"):
        if guard not in try_load and guard not in body(loader, "lazy_is_loadable"):
            raise AssertionError(f"failed artifact can suppress a valid fallback: {guard}")
    range_lookup = body(loader, "overlay_find_by_range")
    for guard in ("s_range_candidate_generation", "range_candidate_preferred"):
        if guard not in range_lookup:
            raise AssertionError(f"loaded range selection lost tier/new-artifact ordering: {guard}")
    ac_poll = body(autocompile, "autocompile_poll_main")
    if "one idempotent batch-end" not in ac_poll or "overlay_loader_rescan();" not in ac_poll:
        raise AssertionError("direct shard handoff is not reconciled into the additive index")
    if ("overlay_loader_commit_published(published->image)" not in ac_poll or
            "overlay_loader_load_published(" in ac_poll):
        raise AssertionError("emulation thread regressed to first-mapping published DLLs")
    watcher = body(autocompile, "watch_thread")
    preparer = body(autocompile, "publish_prepare_thread_main")
    if "out_append(buf, (int)got);" not in watcher:
        raise AssertionError("compile watcher no longer drains child output")
    if ("overlay_loader_prepare_published(item->path)" in watcher or
            "overlay_loader_prepare_published(item->path)" not in preparer):
        raise AssertionError("pipe draining and DLL preparation are not separated")
    if ("s_publish_commit_active" not in preparer or
            "s_publish_commit_active++" not in body(autocompile,
                                                     "publish_ready_pop") or
            "publish_commit_finished();" not in ac_poll):
        raise AssertionError("worker mapping can overlap main-thread loader commit")
    if "AC_PUBLISH_CAP" in autocompile:
        raise AssertionError("live publication queue can silently overflow a fixed path ring")
    prepare_image = body(loader, "overlay_loader_prepare_published")
    commit_image = body(loader, "overlay_loader_commit_published")
    validated = prepare_image.find(
        "published_path_valid(dll_path, canon, sizeof(canon))")
    guard_open = prepare_image.find(
        "CreateFileA(canon, GENERIC_READ, FILE_SHARE_READ")
    first_map = prepare_image.find("LoadLibraryA(canon)")
    if min(validated, guard_open, first_map) < 0 or not (
            validated < guard_open < first_map):
        raise AssertionError(
            "Windows published image must be canonical-validated and "
            "guard-held before its worker first-map")
    if "LoadLibraryA(dll_path)" in prepare_image:
        raise AssertionError(
            "worker first-map regressed to mapping raw (non-canonical) pipe text")
    if ("load_one_dll_prepared(canon, handle)" not in commit_image or
            "overlay_library_close(handle)" not in commit_image):
        raise AssertionError("prepared image commit does not consume every speculative reference")
    watched_write = body(memory, "overlay_watch_note_write")
    if "g_dirty_ram_exec_page_bitmap" not in watched_write:
        raise AssertionError("RAM writes do not clear stale per-page capture evidence")
    if "overlay_capture_before_dma" in watched_write:
        raise AssertionError("universal RAM-write hook regressed to snapshot I/O")
    if "overlay_loader_note_code_write()" not in watched_write:
        raise AssertionError("manifested code writes do not invalidate negative misses")
    if "g_dirty_ram_code_gen++" in watched_write:
        raise AssertionError("watched writes churn the interpreter's global code epoch")
    candidate = body(loader, "overlay_loader_is_candidate")
    if "exact_entry_has(phys)" not in candidate or "idx_head" in candidate:
        raise AssertionError("candidate presence regressed from bitmap to hash probes")
    # dll_already_loaded is a hash probe since bab84528. Every hit is still
    # confirmed by cache_path_equal, and the hash folds exactly what that
    # compare ignores on Windows (separator and ASCII case).
    if "cache_path_equal(s_loaded_paths[entry - 1], path)" not in body(
            loader, "dll_already_loaded"):
        raise AssertionError("published/scanned path separator aliases can double-load DLLs")
    path_hash = body(loader, "loaded_path_hash")
    if "if (c == '\\\\') c = '/';" not in path_hash or "cache_ascii_lower(c)" not in path_hash:
        raise AssertionError("loaded-path hash does not fold what the path compare ignores")
    scheduler = body(traps, "psx_scheduler_run")
    shadow_sched_fixup = scheduler.find(
        "overlay_loader_shadow_scheduler_escape_fixup();")
    scheduler_reset = scheduler.find("g_psx_dispatch_depth = 0")
    if min(shadow_sched_fixup, scheduler_reset) < 0 or shadow_sched_fixup > scheduler_reset:
        raise AssertionError("scheduler longjmp can bypass shadow cleanup")
    shadow_cleanup = body(loader, "shadow_escape_cleanup")
    for guard in ("gte_precision_speculative_end();",
                  "g_shadow_mmio_watch = s_shadow_saved_mmio_watch;",
                  "s_native_exec  = s_shadow_saved_native_exec;",
                  "s_suppress_irq = s_shadow_saved_supp;",
                  "g_exec_phase   = s_shadow_saved_exec_phase;"):
        if guard not in shadow_cleanup:
            raise AssertionError(f"shadow nonlocal cleanup lost: {guard}")
    for switch_fn, mutation in (
            ("psx_request_thread_switch", "psx_save_context_to_tcb"),
            ("psx_change_thread_fiber", "psx_bind_current_host_thread")):
        switch_body = body(traps, switch_fn)
        bail = switch_body.find(
            "overlay_loader_shadow_native_thread_switch_bail()")
        first_mutation = switch_body.find(mutation)
        if min(bail, first_mutation) < 0 or bail > first_mutation:
            raise AssertionError(
                f"{switch_fn} can mutate scheduler state before shadow bail")
    syscall = body(traps, "psx_syscall")
    for guard in ("switch_result < 0", "return 1;"):
        if guard not in syscall:
            raise AssertionError(f"syscall lost native shadow bail: {guard}")
    shadow_switch_bail = body(
        loader, "overlay_loader_shadow_native_thread_switch_bail")
    for guard in ("s_shadow_scheduler_bail = 1;", "g_psx_call_bail = 1;"):
        if guard not in shadow_switch_bail:
            raise AssertionError(f"native scheduler divergence can escape: {guard}")
    interrupt_poll = body(interrupts, "psx_check_interrupts")
    scheduler_jumps = [m.start() for m in re.finditer(
        r"longjmp\(g_scheduler_jmpbuf", interrupt_poll)]
    if len(scheduler_jumps) != 2:
        raise AssertionError("unexpected direct interrupt scheduler escape count")
    previous_jump = -1
    for jump in scheduler_jumps:
        guard = interrupt_poll.rfind(
            "overlay_loader_shadow_native_thread_switch_bail()", 0, jump)
        if guard <= previous_jump:
            raise AssertionError(
                "direct interrupt scheduler escape bypasses native shadow bail")
        previous_jump = jump
    posix_sweep = body(loader, "posix_abi_sweep_file")
    if not (posix_sweep.find("cache_name_is_immutable(file->name)") <
            posix_sweep.find("psx_overlay_posix_library_open")):
        raise AssertionError("ABI preflight can execute a legacy POSIX constructor")
    abi_sweep = body(loader, "abi_preflight_sweep")
    if not (abi_sweep.find("cache_name_is_immutable(fd.cFileName)") <
            abi_sweep.find("LoadLibraryA(full)")):
        raise AssertionError("ABI preflight can execute a legacy Windows DllMain")

    # The interpreter's one-cycle charge goes straight to the inline
    # psx_advance_cycles (PS1B-507 removed the dead inline limit in front of it;
    # see removed_cycle_limit_references below). Lockstep replay charges nothing
    # here, and the exact diagnostic modes take the slow path inside the advance.
    interp_step = body(interp, "interp_cyc_step")
    if "psx_advance_cycles(1u)" not in interp_step:
        raise AssertionError("interpreter step no longer charges through psx_advance_cycles")
    if "g_ls_replay_active" not in interp_step:
        raise AssertionError("interpreter step charges guest time during lockstep replay")
    advance = body(cycles_header, "psx_advance_cycles")
    if ("g_ls_replay_active | g_event_step_conservative" not in advance or
            "psx_advance_cycles_slow(cycles)" not in advance):
        raise AssertionError("inline cycle advance does not send exact diagnostic modes to the slow path")
    if "PSX_NO_DEBUG_TOOLS" not in starvation or "STARVATION_RING_ENABLED 0" not in starvation:
        raise AssertionError("production still enables the diagnostic starvation ring")
    # The mask is a header inline over the instruction word since 9f9ac75b (a
    # switch on the primary opcode, no role tables). It must not call out to
    # another decoder on the per-instruction path.
    if "static inline uint32_t psx_cyc_dep_res_mask(uint32_t insn)" not in instr_cost:
        raise AssertionError("load-delay dependency mask is no longer a header inline")
    dep_mask = body(instr_cost, "psx_cyc_dep_res_mask")
    dep_calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", dep_mask)) - {"switch", "UINT32_C"}
    if dep_calls:
        raise AssertionError("load-delay dependency masks regressed to a second instruction decoder")
    gte_execute = body(gte, "gte_execute")
    direct_import = gte_execute.find("gte_import_cpu_state(&gte, cpu);")
    command = gte_execute.find("gte_run_command(&gte, cmd);")
    direct_export = gte_execute.find("gte_export_cpu_state(cpu, &gte);")
    deadline = gte_execute.find("psx_gte_set(cpu, psx_gte_cmd_latency(cmd));")
    if min(direct_import, command, direct_export, deadline) < 0 or not (
            direct_import < command < direct_export < deadline):
        raise AssertionError("GTE direct marshaling or command deadline ordering regressed")
    gte_marshaling = (gte_execute + body(gte, "gte_import_cpu_state") +
                      body(gte, "gte_export_cpu_state"))
    for old_selector in ("gte_mtc2(&gte", "gte_ctc2(&gte",
                         "gte_mfc2(&gte", "gte_cfc2(&gte"):
        if old_selector in gte_marshaling:
            raise AssertionError("GTE command bridge regressed to selector dispatch loops")
    # Inline cycle limit. It had no nonzero writer from 31015cea on, so both
    # readers always fell back to psx_advance_cycles (PS1B-50, PS1B-176 O2).
    # PS1B-507 measured the dead check at about 0.74 ns per interpreted
    # instruction and a restored limit at the same cost as none, and removed
    # the symbol. No code may name it again without changing this guard.
    stale = removed_cycle_limit_references()
    if stale:
        raise AssertionError(f"removed inline cycle limit is referenced again: {stale[0]}")
    if "!defined(PSX_COSIM) && !STARVATION_RING_ENABLED" not in memory:
        raise AssertionError("runtime load fast path leaks into COSIM/diagnostic builds")

    # Memory timing was reimplemented on 2026-09-20 (5821073d, 8b8555c4).
    # Word and half loads take an inline main-RAM path in psx_cyc.h; byte and
    # LWC2 loads take a direct-RAM path in memory.c. Both charge timing before
    # the value is read and fall back to the full accessor otherwise.
    for fn, slow in (("psx_cyc_load_word", "psx_cyc_load_word_slow"),
                     ("psx_cyc_load_half", "psx_cyc_load_half_slow")):
        inline_load = body(cyc_header, fn)
        for guard in ("#ifndef PSX_OVERLAY_DLL_BUILD", "physical < UINT32_C(0x800000)",
                      "!g_ls_mode", "!g_ds_recording", "!g_dma_cpu_read_wait"):
            if guard not in inline_load:
                raise AssertionError(f"{fn} main-RAM fast path lost guard: {guard}")
        timing = inline_load.find("psx_cyc_ram_load_timing(cpu, rt, reg_mask);")
        value = inline_load.find("memcpy(&result, g_psx_ram")
        watch = inline_load.find("if (g_ram_read_watch_active) debug_server_trace_ram_read_watch(")
        fallback = inline_load.find(f"return {slow}(cpu, addr, rt, reg_mask);")
        if min(timing, value, watch, fallback) < 0 or not timing < value < watch < fallback:
            raise AssertionError(f"{fn} does not order timing, RAM fast path, fallback")
    direct_ram = body(memory, "t172_memory_direct_ram")
    for guard in ("addr < 0xc0000000u", "(addr & 0x1fffffffu) < 0x800000u", "!g_ls_mode",
                  "!g_ds_recording", "!g_ls_replay_active", "!g_ram_read_watch_active",
                  "!g_dma_exec_depth"):
        if guard not in direct_ram:
            raise AssertionError(f"main-RAM value fast path lost guard: {guard}")
    direct_def = memory.find("static int t172_memory_direct_ram(")
    direct_gate = memory.rfind("#if defined(PSX_NO_DEBUG_TOOLS) && !defined(PSX_COSIM)", 0, direct_def)
    if direct_gate < 0 or "#endif" in memory[direct_gate:direct_def]:
        raise AssertionError("main-RAM value fast path leaks into diagnostic/COSIM builds")
    for fn, value_read in (("t172_memory_load", "psx_read_byte(addr)"),
                           ("psx_cyc_lwc2_read", "psx_read_word(addr)")):
        load_body = body(memory, fn)
        timing = load_body.find("t172_memory_before(")
        fast = load_body.find("t172_memory_direct_ram(addr)")
        fallback = load_body.find(value_read)
        tail = load_body.find("if (tail) psx_advance_cycles(tail);")
        if min(timing, fast, fallback, tail) < 0 or not timing < fast < fallback < tail:
            raise AssertionError(f"{fn} does not order timing, RAM fast path, fallback")
    for fn in ("psx_cyc_load_word_slow", "psx_cyc_load_half_slow", "psx_cyc_load_byte",
               "psx_cyc_load_word_timing_only"):
        if "t172_memory_load(" not in body(memory, fn):
            raise AssertionError(f"{fn} bypasses the timed memory load")

    gte_exec = body(gte, "gte_execute")
    for probe in ("s_gte_exec_count++", "s_gte_caller_ra =",
                  "int16_t intpl_pre_ir[4]", "gte_rtp_record(&gte, cmd)",
                  "gte_intpl_record(&gte, intpl_pre_ir, intpl_pre_fc)"):
        pos = gte_exec.find(probe)
        begin = gte_exec.rfind("#ifndef PSX_NO_DEBUG_TOOLS", 0, pos)
        end = gte_exec.find("#endif", pos)
        if pos < 0 or begin < 0 or end < 0:
            raise AssertionError(f"production GTE path still executes diagnostic probe: {probe}")

    print("PASS: capture/cache and interpreter cycle hot-path guards intact")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
