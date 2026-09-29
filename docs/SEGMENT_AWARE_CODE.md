# Segment-aware code

Status: design proposal (2026-09-28). The four owner decisions in §10 were
settled on 2026-09-29; each follows this document's recommendation.
Stacked on RetroPortingToolKit/psxrecomp#417 (`fix/overlay-segment-alias`).
This change adds only this document and an acceptance test; it does not change
behaviour.

Acceptance test: `recompiler/tests/test_segment_aware_codegen.py` (ctest
`segment_aware_codegen`). Synthetic EXE: `tools/segment_testrom/gen_segment_exe.py`.

## 1. Summary

A PS1 PC carries a segment:
- KUSEG `0x0xxxxxxx`
- KSEG0 `0x8xxxxxxx` (cached)
- KSEG1 `0xAxxxxxxx` (uncached)

The three map the same physical RAM. The CPU keeps the segment in:
- every link value;
- every EPC;
- every I-cache tag.

KSEG1 also changes what each fetch costs. Compiled code bakes a single segment
into all of these, so it is only correct for PCs in that segment.

#417 made overlay shards fail closed for other segments. Three problems
remain:

1. **KUSEG-linked EXEs.** Static game code for these executables runs at KUSEG
   PCs through KSEG0-baked bodies (Kula World SCES-01000, Alien Resurrection
   SLUS-00633).
2. **Overlay code at KUSEG or KSEG1.** Overlay code that genuinely executes at
   KUSEG or KSEG1 is interpreted forever, because nothing records or compiles
   its segment.
3. **Uncached fetch charging (new finding).** Compiled KSEG1 code charges the
   uncached +4 fetch only at cache-line leaders. Beetle and the interpreter
   charge it for every instruction. This already affects the BIOS ROM
   today: 5,477 of the 9,592 KSEG1 instruction sites in OpenBIOS are
   undercharged.

**Recommendation: per-segment compiled variants, not segment-relative
emission.**
- Code identity becomes the full runtime virtual address: segment plus
  physical address.
- The emitter takes the segment as a compile-time constant. This generalizes
  the BIOS emitter's existing `relocate_ra`.
- A body exists for each `(segment, entry)` that is actually executed:
  - static code: the EXE's link segment, plus variants requested with
    segment-qualified seeds;
  - overlays: the segments the capture observed.
- A PC with no body for its segment is a loud dispatch miss. It never runs
  another segment's body.
- Uncached variants charge fetch on every instruction.

For every existing KSEG0 title the emitted code is byte-identical, so the
static fast path costs nothing. Segment-relative emission would cost
+3.8 % code size for the constants alone, and +27.6 % with exact KSEG1
charging (measured on R4, §6).

## 2. What the segment changes: the Beetle model

Beetle is the oracle (CLAUDE.md §2). The citations below are to
`libretro/beetle-psx-libretro` `mednafen/psx/cpu.c` @ `a7f0811`. The in-tree
transcription, `runtime/src/psx_icache.c`, cites the older in-tree
`cpu.cpp:534-601`; the semantics are the same.

| Architectural value | Beetle | Depends on segment? |
|---|---|---|
| Link of `jal`/`jalr`/`bgezal`/`bltzal` | `GPR[31] = new_PC`, i.e. PC+8 (`DO_BRANCH`, 1012-1031) | yes: PC's segment |
| `j`/`jal` target | `((PC+4) & 0xF0000000) + (target << 2)` (JAL 1810-1818) | preserves PC[31:28] |
| Branch target | PC-relative | preserves the segment inside any RAM window |
| EPC | `CP0.EPC = PC` (−4 in a delay slot) (`CPU_Exception`, 846-850) | yes |
| I-cache hit | `ICache[(addr & 0xFFC) >> 2].TV == addr`: the **full virtual address** (744) | yes: KUSEG and KSEG0 aliases never share a line |
| Uncached fetch | `addr >= 0xA0000000` or BIU bit 11 clear: **+4 on every fetch**, no fill, clears the load give-back (746-763) | yes: KSEG1 |
| Cached miss | +3, plus 1 per word refilled to the line end; tags written with the full address (764-831) | yes |
| Data loads and stores | `addr_mask[addr >> 29]`: KSEG0/KSEG1 reduced to physical | no |

The interpreter (`dirty_ram_interp.c`) already follows every segment row: it
links with `pc + 8` (1978), computes `j` targets from `pc + 4` (1872), and
fetches at `cpu->pc` (1521). It does not model the BIU cache-disable case
(§9).

**Invariant used by this design.** Direct control flow never changes segment:
- `j`/`jal` keep PC[31:28];
- branches stay within ±128 KiB of the PC.

Only indirect transfers switch segment: `jr`/`jalr`, exception entry, and the
return through EPC. All three already go through dispatch with the full PC.
So a body compiled for segment S only ever reaches other S bodies by direct
edges.

**Oracle deviation (decided 2026-09-29, §10).** Beetle's own comment
(719-730) says hardware clears bit 31 of a KSEG0 fetch address before the tag
compare and the tag write. On hardware, KUSEG and KSEG0 aliases therefore *do*
share lines. I-cache tags follow the oracle, as `psx_icache.c` does today. The
acceptance test pins this: its Beetle model and `psx_icache.c` must agree.
Moving to hardware semantics would be a separate, oracle-backed change. §5.6
keeps the tag computation in one place so that change stays one line. The
hardware difference is recorded under ACCURACY_BURNDOWN axis 4
("KUSEG/KSEG0/KSEG1 mirroring").

## 3. Where psxrecomp loses the segment today

### 3.1 Baked PCs in the game and overlay emitter (`code_generator.cpp`)

Every one of these sites writes the compile address, which is always KSEG0.

| Site | Lines | R4 static count |
|---|---|---|
| `cpu->gpr[31] = 0x…u` (jal, jalr, bgezal, bltzal links) | 2094, 2100, 2109 | 5,373 |
| `psx_icache_fetch(cpu, 0x…u)` (fetch tags) | 1820-1826 | 61,181 |
| `psx_check_interrupts_at(cpu, 0x…u)` (resume PC → EPC) | 170-175, ~20 callers | 30,596 |
| `cpu->pc = 0x…u; return;` (CPS exits, stale-static guard) | 115, 2199-2547, 2895, 3148 | 6,927 |
| `g_debug_last_store_pc = 0x…u` (before every `sb`/`sh`/`sw`/`swl`/`swr`/`swc2`) | 1307, 1446, 1615-1619, 1666 | 22,936 |
| CPS continuation keys `case 0x…u: goto block_…` | 2747-2773, 3065-3110 | — |
| Reserved-instruction EPC `cpu->cop0[14] = 0x…u` | 2059 | — |
| `psx_slice_block(cpu, 0x…u, …)` (the interpreter resumes here) | 1789 | — |

The R4 figures come from all 50 generated shards: 2,994 functions and
141,335 emitted instruction sites.

**The store-PC stamp is guest-affecting, not a debug hook.** Its name and the
emitted `extern` comment describe debug attribution, but the runtime reads it
in every build:
- `memory.c` `psx_write_word_raw` drops a word store to RAM `0x0`-`0xF` when
  the stamp equals one of a list of exact PCs (1742-1765). An opt-in Tomba
  card filter (`PSX_TOMB_CARD_EVCB_PROTECT`, 1789) keys on it the same way.
- The interpreter stamps the full executing PC (`dirty_ram_interp.c`
  2288-2348). A compiled body that stamps a different segment than the one it
  runs in can make a filter match in one execution path and miss in the
  other.
- #420 (`fix/fingerprint-guest-facts`, ABI v24) makes overlay shards write the
  host's copy in every build. Before it, overlay stores wrote a private copy,
  so after an overlay store the filters saw the PC of an older store.

So the stamp is a baked PC like the others in the table, and §5.2 routes it
through `runtime_pc()`.

Two other groups carry PCs but are identity keys, not architectural state:
- debug and identity hooks: `cosim_*`, `debug_server_*`,
  `psx_mod_function_entry`;
- `.ranges` manifests.

The dispatch file (`main_psx.cpp` 1460-1724):
- keys its table by KSEG0 address and looks it up with the physical address
  (`psx_game_find_entry`, `want = addr & 0x1FFFFFFF`);
- then sets `cpu->pc = entry->resume_pc`, which is a KSEG0 constant.

A KUSEG or KSEG1 PC therefore enters the KSEG0 body and continues in KSEG0.
`test_kuseg_dispatch_lookup.py` currently asserts exactly this alias
behaviour; PR C in §8 changes it.

### 3.2 KUSEG-linked EXEs

`ps1_exe_parser.cpp:286-301` rewrites the header's KUSEG addresses to KSEG0 in
place and records no original segment. It also rejects a KSEG1 entry (158).
Three things follow:
- The whole game compiles for KSEG0 while hardware runs it at KUSEG. Every
  link it saves on the stack and every EPC is `0x8…` instead of `0x0…`. Every
  fetch tag differs from the one the BIOS, the interpreter and Beetle use.
- **Seeds in the game's own segment are silently dropped.**
  `main_psx.cpp:766` range-checks seeds against the normalized
  `load_address`, so a seed written as `0x000100B8` is ignored. It must be
  written `0x800100B8`.
- R4 is linked at KSEG0 and is unaffected.

### 3.3 Aliases

A KSEG1 or KSEG0 alias of static text resolves to the same body (§3.1). The
uncached cases are rare but real: code that ORs `0xA0000000` into a function
address to run it uncached, and cache-maintenance trampolines. Those run with
the wrong fetch cost and the wrong links. There is no way to request a body
for a second segment: a segment-qualified seed is folded into the KSEG0 body
or dropped.

### 3.4 Overlays (after #417)

Segment is lost at every stage:
- **Capture:** it records bytes and entry PCs per physical word
  (`g_dirty_ram_{exec,dispatch}_pc_bitmap`). It writes every PC back out as
  `PSX_OVERLAY_CODE_SEGMENT | offset` (`overlay_capture.c` 365, 420-446).
- **Interpreter dispatch:** it sees the full PC, but drops the segment at
  `dirty_ram_interp.c:2769`.
- **Cache names and manifests:**
  - filenames and namespaces start from the physical address
    (`{phys}_{crc}`, `ov_{phys}_…`);
  - `.ranges` F entries are forced to KSEG0 (`overlay_loader.c:942`);
  - exports must be named `func_{KSEG0 entry}`;
  - candidates are indexed by physical address.
- **Gate:** #417's gate (`overlay_loader.c:3668`) interprets every
  non-KSEG0 PC.

In R4 that is about 27 dispatches per frame: OpenBIOS enters its RAM patch
slots `0x0000281C` and `0x0000357C` at KUSEG. Nothing ever asks for a KUSEG
shard.

### 3.5 Uncached fetch in compiled code

Both emitters emit `psx_icache_fetch` only at cache-line leaders:
- block leaders, jump-table targets, and `addr & 0xC == 0`
  (`code_generator.cpp:1821`, `full_function_emitter.cpp:781`).

The stated reason is that "intra-line followers reached by fall-through are
guaranteed hits". That holds for cached segments only. Every KSEG1 fetch
misses and costs +4 (§2).

The BIOS main ROM runs in place at `0xBFC0…` (KSEG1). In OpenBIOS, 5,477 of
its 9,592 KSEG1 instruction sites have no fetch call. Each is:
- 4 cycles short;
- missing the load give-back clear.

The interpreter charges every one. Compiled and interpreted ROM code
therefore disagree, against the "one shared per-instruction cost" rule
(CLAUDE.md RULE −1). Ruler #2 and ruler #1 both run in cached RAM, so neither
caught it.

## 4. Options

### A. Segment-relative emission

One body per function takes the segment at run time: from a field set by
dispatch, or from a parameter. It emits `seg | phys` at every site in §3.1.

- **Code and speed.** Every baked constant becomes an OR with a live register.
  That is about 127k sites in R4 (22,936 of them store-PC stamps), before
  continuation keys and switch tables.
  CPS continuation switches can no longer be `case` constants; they need a
  physical-address switch plus a segment check.
- **Exact KSEG1 charging.** The body cannot know statically whether it is
  uncached. So each non-leader instruction (about 80k sites in R4) needs a
  run-time `if (seg >= KSEG1) fetch` test. A block-level surcharge is not
  exact, because each uncached fetch clears the load give-back between
  instructions.
- **ABI.** Every generated function needs the segment source:
  - direct calls must carry it;
  - dispatch must scope it;
  - the overlay DLL ABI (`PSX_OVERLAY_ABI_TAG`) and mod entry hooks change.
- **Measured cost** (§6): +3.8 % `__TEXT` for the constants, +27.6 % with
  exact KSEG1 charging.
- **Benefit:** one body serves any segment, and no variant set has to be
  chosen.

### B. Per-segment compiled variants (recommended)

- The emitter bakes the segment as a constant, as it does now, but reads it
  from a per-compile `code_seg` instead of assuming KSEG0.
- A body is compiled for each `(segment, entry)` the program executes.
- Uncached variants emit a fetch charge on every instruction; cached ones keep
  the leader rule.

Costs:
- Code grows only by the variants actually requested. These are almost always
  small closures, such as a cache-flush trampoline.
- The variant set must be known. The segment-miss ring (§5.5) and capture
  (§5.7) supply that evidence.

Reasons to choose B:
- It keeps the fast path exactly as it is for the home segment.
- It keeps each body's cost model static, so it can be checked against Beetle
  per instruction.
- It follows the model the BIOS emitter already uses: per-window runtime PCs
  through `BiosAddressModel::runtime_pc`.
- Option A spends 4-28 % of every title's code on a case most titles never
  hit.

## 5. Design

### 5.1 Code identity is the runtime virtual address

A compiled body is identified by `seg | phys`:
- `func_` names use it: `func_00010000` for a KUSEG-linked entry,
  `func_A00100C0` for a KSEG1 variant.
- Direct C calls and CPS exits then resolve to the same-segment variant with
  no extra lookup.
- KSEG0 names do not change.

### 5.2 Emitter: one `runtime_pc()` for every baked PC

- `CodeGenerator` gets `runtime_pc(compile_addr) = code_seg | (compile_addr &
  0x1FFFFFFF)`, modelled on `bios_runtime_pc`.
- Every site in §3.1 goes through it, the store-PC stamp included. That is
  9 helpers and about 40 format sites, plus 8 store-PC format sites.
- The helper also covers `generate_alias_group` bodies and the dispatch
  emitter's rows (`addr`, `resume_pc`).
- Identity keys (block labels, `.ranges`) may keep the compile address. The
  store-PC stamp is not an identity key (§3.1).
- `code_seg` defaults to KSEG0.
- **Acceptance:** regenerating any KSEG0 title is byte-identical. That proves
  the refactor has zero fast-path cost. Closes nothing in the ledger on its
  own; enables §5.3-5.6.

The BIOS emitter's precedent has holes to close in the same pass:
- the fallthrough `cpu->pc = next_addr` (`full_function_emitter.cpp:1652`);
- the `strict_translator` syscall, break and unaligned-access PCs, which use
  the ROM address;
- the `strict_translator` store-PC stamp, which also uses the ROM address.
  Moving it changes which `memory.c` filters match. Several of their keys
  are ROM addresses of code that runs relocated, so those keys must be
  re-keyed to runtime PCs in the same change (§9).

### 5.3 EXE parser keeps the link segment

- `PS1Executable` records `link_segment = initial_pc & 0xE0000000`.
- It keeps physical helpers for byte addressing instead of rewriting the
  header.
- Load address and entry must share the segment:
  - KUSEG and KSEG0 are accepted;
  - KSEG1 is accepted and compiles as an uncached home segment.
- The game emitter runs with `code_seg = link_segment`.
- Seeds are range-checked by physical address, so `0x000100B8` is a valid
  home seed.

Closes `link-segment`, `fetch-tag-segment`, `irq-resume-segment`,
`resume-pc-segment`, `store-pc-segment`, `home-seed-accepted` and
`alias-fetch-coherence`.

### 5.4 Variant requests and closure

- A seed whose segment differs from the link segment requests a variant, for
  example `0xA00100C0`. This reuses the seeds file; no new configuration
  surface is needed. Decided 2026-09-29 (§10): segment-qualified seeds are the
  only request mechanism, with no `game.toml` table.
- The recompiler compiles the variant's **direct-edge closure** in that
  segment:
  - direct calls;
  - tail jumps;
  - CPS continuation targets.
- By the invariant in §2, that closure is everything the variant can reach
  without dispatch.
- Bodies are emitted once per `(segment, entry)`. Cached variants use the
  leader rule; KSEG1 variants use §5.6.

Closes `segment-variants`.

### 5.5 Dispatch is exact; a segment miss is a dispatch miss

- The table is keyed by the full VA. `psx_game_find_entry` keeps its
  physical-word index. The index now points at the first row for that word,
  and the lookup requires `row.addr == addr`.
- Rows for one physical word are adjacent. A title with no variants has one
  row per word, which is today's shape.
- `resume_pc` is per-variant, so `cpu->pc = entry->resume_pc` is correct by
  construction.
- A physical hit with no row for the PC's segment is a **segment miss**:
  - it is counted and recorded in a TCP-visible ring with the full PC;
  - it takes the existing clean-text-miss path (`dirty_ram_interp.c`
    `clean_game_text_miss`).
- Resolving it follows the project rule for any dispatch miss: add the
  segment-qualified seed, then regenerate.
- It never runs another segment's body.
- Decided 2026-09-29 (§10): a segment miss in static game code interprets
  loudly until the title is regenerated. It does not fail fast.

Closes `segment-miss`. `psx_call_contract`'s segment-masked return check
(`cpu_state.h:308`) can become exact in the same PR.

### 5.6 Per-instruction fetch charging for uncached code

The rule is: a body whose runtime PCs are uncached charges fetch on **every**
instruction.
- Cached bodies keep the leader rule. It is exact for them; the ledger's model
  check proves it against Beetle for the probe block.
- The charge is `psx_icache_fetch_uncached(cpu)`. It clears the load
  give-back and adds +4 when the I-cache model is active, skips during
  lockstep replay, and does no tag lookup.
- It is equivalent to `psx_icache_fetch(cpu, kseg1_pc)` because psxrecomp
  never writes a KSEG1 tag (it does not model tag-test mode).
- The tag compare stays in `psx_icache.c`. Tags follow Beetle (decided, §10),
  and this is the one place a later, oracle-backed move to hardware tags (§2)
  would change.

It applies in three places:
- **BIOS emitter:** `relocate_ra(rom) >= 0xA0000000`, which is the ROM run in
  place. Closes `bios-kseg1-fetch-charge`.
- **Game and overlay emitter:** `code_seg == KSEG1`. Closes
  `kseg1-fetch-charge` together with §5.4.
- **Interpreter:** unchanged; it already charges every fetch.

BIU bit 11 (cache disable) makes every fetch uncached in Beetle. It is a run-time
state, not a segment, and is listed under §9.

### 5.7 Segment-aware overlay capture and cache keys

The bytes are segment-free; execution is not. So the byte identity (region,
CRC, per-function `code_crc`, pair dedup) stays physical, and the segment
joins only the *entry* identity and the *compiled artifact* key.

**Capture**
- `g_dirty_ram_dispatch_pc_bitmap` keeps its meaning (dispatched in any
  segment), and region building keeps using it.
- Add one sibling bitmap per segment: KUSEG, KSEG0 and KSEG1.
- The siblings are set at the interpreter dispatch that already sets the bit,
  from the full `addr` before `dirty_ram_interp.c:2769` masks it. The cost is
  one bit set per interpreted dispatch, not per instruction. Host memory is
  3 × RAM/32 (192 KiB retail, 768 KiB in 8 MiB mode).
- The execution bitmap needs no segment: direct edges keep the entry's
  segment.

**Capture JSON**
- Schema v3 adds `"dispatch_entry_segments": {"kuseg": [...], "kseg0":
  [...], "kseg1": [...]}`.
- If it is absent, entries are KSEG0 only, so every existing v2 capture stays
  valid.
- The #417 counter `segment_alias_interp` becomes the trigger: a miss records
  its entry into these bitmaps, and the next compile builds the shard.

**Compile (`compile_overlays.py`)**
- One shard per `(region, segment with entries)`.
- `make_psxexe(seg | phys, …)` relies on §5.3's parser.
- `_canonical_guest_addr` and the `| 0x80000000` sites become `seg | phys`.
- The static-overlay generator accepts non-KSEG0 variants: it emits the
  segment in its table and matches exactly, replacing the #417 rejection at
  2535.

**Cache keys** (layout decided 2026-09-29, §10)
- KSEG0 artifacts keep today's path and names, so no existing cache
  invalidates.
- Other segments go in a per-segment subdirectory of the same cache tag:
  `…/cg<N>_<hash>_gc<hash>_f<n>/seg-kuseg/{phys}_{crc}.{dll,so}` (and
  `seg-kseg1/`).
- The filename grammar (`psx_overlay_cache_name_parse`, `8_8` or `8_8_8`)
  does not change.
- Static namespaces add `s<seg >> 29>` for non-KSEG0 shards (`ov_s0_{phys}_…`
  for KUSEG, `ov_s5_…` for KSEG1), so static variants can link side by side.
- Exports are `func_{VA}` (§5.1), so they are distinct per segment.
- `pair_id` hashes the C source, so variants never pair-alias one another.
- A variant shard stamps its own segment's store PCs (§5.2). This relies on
  #420 (ABI v24), which forwards overlay stamps to the host's
  `g_debug_last_store_pc` in every build, so the `memory.c` filters see them.

**Manifest**
- `.ranges` gains `S <segment>`; if it is absent, the segment is KSEG0.
- `F` entries carry the full VA and must match `S`. `parse_manifest` stops
  forcing KSEG0 (942).
- `R` lines stay physical extents: they validate bytes, not execution.

**Loader**
- Candidates keep the physical index and gain a `seg` field.
- The #417 gate generalizes from "PC is KSEG0" to "a candidate for this
  physical address has `seg == pc & 0xE0000000`".
- Other aliases still interpret. Rule 18 permits that for runtime-installed
  code, and it is counted and fed back to capture.
- `overlay_idle_note_is_internal_or_return` (2293) compares full VAs.

**Docs:** update AOT_OVERLAY_PLAN's "canonical KSEG0 entries" contract and
AOT_SHARDING's KSEG0 window. OVERLAY_CACHE_V2's per-function key already
names `guest_entry_vaddr`; this design makes that VA include the segment.

**Expected R4 effect:** the OpenBIOS KUSEG patch slots are captured as
KUSEG entries of the kernel page and get a KUSEG shard. `segment_alias_interp`
drops to 0 on a warm cache.

## 6. Cost for the static fast path

| | Option A (segment-relative) | Option B (variants), this design |
|---|---|---|
| Home-segment code | +3.8 % `__TEXT`; +27.6 % with exact KSEG1 charging | **0**: byte-identical for KSEG0 titles |
| Per-instruction work | an OR at each baked PC, plus a KSEG1 test at about 80k non-leader sites | none added |
| Dispatch | segment scoping on every dispatch | one `row.addr == addr` compare per dispatch; with no variants, the same row count as today |
| ABI | every generated function and the overlay DLL ABI | unchanged (it relies on #420's v24 store-PC forwarding, which is independent of this design) |
| Extra code | none | the requested variant closures only |
| KSEG1 fetch fix | runtime test everywhere | a per-instruction charge only in KSEG1 bodies: 5,477 added call sites in OpenBIOS ROM code, paid only while ROM code runs |

**How the A numbers were measured**
- R4 shards 00, 17 and 33 (911,454 B `__TEXT`), compiled with the build's own
  command (Apple clang 21, arm64, `-O3`).
- Rewritten in a scratch copy to load the segment once per function and OR it
  into every link, fetch-tag, IRQ-resume, CPS-exit and store-PC constant:
  946,046 B (+3.8 %).
- With `if (seg >= KSEG1) fetch` added before each non-leader instruction:
  1,163,390 B (+27.6 %).
- The first measurement (2026-09-28) left out the 1,261 store-PC stamps in
  these shards, because it treated them as debug-only. Without them the
  figures were 940,382 B (+3.2 %) and 1,159,826 B (+27.2 %).
- This is a size measurement, not a timing one. The per-instruction test
  sits on the hottest path and is only exact if it runs every time.

**Why B's figure is 0**
- It follows by construction, not measurement: a KSEG0-linked title (R4
  included) is emitted with `code_seg = KSEG0`, which is today's output. PR B
  (§8) proves it by byte-identical regeneration.

**What B adds at run time**
- One compare per dispatch.
- The capture bitmaps (§5.7).
- The recovered native coverage: the ~27 per-frame interpreted KUSEG alias
  dispatches in R4 become native.

## 7. Validation against Beetle

### 7.1 Acceptance ledger (this PR)

`test_segment_aware_codegen.py` builds the synthetic KUSEG-linked EXE and runs
the real `psxrecomp-game` and `psxrecomp-bios` on it (OpenBIOS for the latter,
0.3 s). It checks three things:
- **Emitted PC constants** in the bodies that dispatch picks for link-segment
  PCs.
- **Dispatch behaviour**, using the emitted lookup compiled and queried:
  - home PCs;
  - requested variants;
  - unrequested aliases, which must miss.
- **Fetch cycles.** The emitted fetch sequence runs through the real
  `runtime/src/psx_icache.c`. The result must equal the per-instruction
  sequence Beetle executes, computed by a Python transcription of
  `ReadInstruction`.

**Model checks (must always pass)**
- `psx_icache.c` equals the Beetle transcription on:
  - cold then warm cached runs;
  - a KSEG0 → KUSEG alias refill;
  - repeated KSEG1;
  - partial refill.
- The leader rule is exact for the cached probe block.
- A mutation of the model to hardware bit-31 tags, or to a +5 uncached cost,
  fails.

**Gap ledger.** `KNOWN_GAPS` in the test lists each id with the section that
closes it. The test passes only when the observed gaps equal the ledger. Today
all eleven are open:

| id | observed today | closed by |
|---|---|---|
| `link-segment` | 7 link constants, e.g. `0x80010018` | §5.3 |
| `fetch-tag-segment` | 25 fetch tags in KSEG0 | §5.3 |
| `irq-resume-segment` | 4 resume PCs in KSEG0 | §5.3 |
| `resume-pc-segment` | 10 exit PCs / continuation keys in KSEG0 | §5.3 |
| `store-pc-segment` | 10 store-PC stamps in KSEG0, e.g. `0x8001000C` | §5.3 |
| `home-seed-accepted` | KUSEG seed loaded 0 of 1 | §5.3 |
| `alias-fetch-coherence` | interp-then-compiled `leaf`: 14 cycles vs Beetle 7 (the #417 shape) | §5.3 |
| `segment-variants` | `0xA00100C0` resolves to the `0x800100C0` body | §5.4 |
| `segment-miss` | unrequested KSEG0/KSEG1 aliases resolve; KUSEG PCs resolve to KSEG0 rows | §5.5 |
| `kseg1-fetch-charge` | no KSEG1 body (Beetle: 40 cycles for the 10-instruction run) | §5.4 + §5.6 |
| `bios-kseg1-fetch-charge` | 5,477 of 9,592 OpenBIOS KSEG1 instruction sites uncharged | §5.6 |

Each implementation PR removes the ids it closes. The exception is PR A
(§8): it is based on master, which does not have this test. When A lands,
this PR is rebased onto it, and the ledger drops `bios-kseg1-fetch-charge` in
that rebase. The overlay half (§5.7)
gets its own acceptance case in `runtime/tests/test_overlay_segment_gate.py`:
- a KUSEG-compiled fixture shard runs natively for its KUSEG PC;
- the same shard is interpreted for the KSEG0 and KSEG1 aliases.

### 7.2 Oracle runs (implementation PRs)

**The synthetic EXE is also an oracle probe.** It writes into a results block
at `0x00011000`, outside the image:
- the three link values;
- the executing-segment link from each of the three `probe_run` entries;
- a Timer 2 delta around each of those entries.

It then spins. Procedure:
- Sideload it in Beetle (mednafen auto-detects `PS-X EXE`), or boot it from a
  `tools/cycle_testrom`-style disc.
- Read the block from Beetle, from the interpreter (`PSX_FORCE_INTERP=1`) and
  from the compiled build, and compare.
- Expected values: links `0x0001xxxx`; segment probes `0x0001…`, `0x8001…`
  and `0xA001…`. The KSEG1 − KUSEG T2 delta should show the +4-per-fetch
  surcharge.

**Other runs**
- **§5.6 BIOS fix:** LLE boot (`bios_hle = false`) cycle parity against
  Beetle to the shell, plus ruler #1. This changes BIOS timing and must be
  validated before it lands.
- **KUSEG titles:** regenerate Kula World and Alien Resurrection and smoke
  them. Use a warm/cold A/B with the #417 procedure (seeded,
  `PSX_OVERLAY_AUTOCOMPILE_OFF=1`, compare cyc/mc/sp plus an order-independent
  write sum).
- **KSEG0 titles:** byte-identical regeneration (§5.2).

## 8. Rollout

Small PRs in this order. Each PR updates the ledger and FAITHFUL_TIMING_PLAN's
log.

- **A: `fix/uncached-fetch-per-insn`** (§5.6, BIOS emitter). Independent of
  the rest and the smallest. Closes `bios-kseg1-fetch-charge`. Needs the LLE
  Beetle parity run. Status (2026-09-29): being prepared, based on master.
  Once it lands, this PR is rebased and drops the id from the ledger (§7.1).
- **B: `refactor/emitter-runtime-pc`** (§5.2). No behaviour change for game
  code, including the store-PC stamps; proven by byte-identical regeneration
  of KSEG0 titles. The BIOS-emitter holes in §5.2 do change BIOS output. The
  store-PC one ships with the `memory.c` filter re-key (§9).
- **C: `feat/kuseg-linked-exe`** (§5.3, §5.5). Also rewrites the alias
  assertions in `test_kuseg_dispatch_lookup.py`: an alias with no body now
  misses. Closes seven ids.
- **D: `feat/segment-variants`** (§5.4, and §5.6 in the game emitter). Closes
  `segment-variants` and `kseg1-fetch-charge`.
- **E: `feat/overlay-segment-keys`** (§5.7). Includes the runtime acceptance
  case, and the R4 warm-cache check that `segment_alias_interp` reaches 0.

## 9. Adjacent gaps (not in scope, recorded)

- **BIU bit 11 does not reach the fetch model.** `memory.c` stores
  `0xFFFE0130` but `psx_icache.c` ignores it. Beetle charges +4 for every
  fetch while the cache is disabled (`CPU_SetBIU`, 484-505). This matters
  only for RAM code run with the cache off.
- **IsC stores are dropped** (`memory.c:1731`). Beetle writes the I-cache tag
  and valid bits through them (`WriteMemory_IsC_misc`, 623-641), so a
  `FlushCache` in psxrecomp does not invalidate the I-cache model.
- **BIOS store-PC filter keys are ROM addresses.** The `strict_translator`
  stamp is the ROM address; the interpreter stamps the runtime PC. Several
  `memory.c` filter keys lie in SCPH1001's relocated windows
  (`bios/SCPH1001.toml`):
  - Kernel Part 2, which runs at `0x500`: `0xBFC10A00` and the Tomba key
    `0xBFC117E4`;
  - Shell, which runs at `0x80030000`: `0xBFC3EEB4`, `0xBFC405E4`,
    `0xBFC40788` and `0xBFC41C50`.

  For these keys a filter fires only while that code runs compiled. If the
  same code runs interpreted, the store lands. Routing the BIOS stamp through
  `runtime_pc()` (§5.2) without re-keying would silently disable them.
  Re-keying them to runtime PCs in the same change also makes the
  interpreted path agree.
- **Syscall EPC in compiled code** comes from `cpu->pc` (`traps.c:1040`), but
  the emitted `syscall` site does not set it. Confirm what `cpu->pc` holds
  there; §5.2's pass is the place to make it the syscall's own
  `runtime_pc`.

## 10. Decisions (2026-09-29)

The owner delegated the four open questions and accepted this document's
recommendations. They are settled; implementation PRs follow them.

1. **I-cache tags follow Beetle, the oracle.** Tags compare the full virtual
   address. Hardware clears bit 31 of KSEG0 fetches first; that difference
   is recorded in ACCURACY_BURNDOWN axis 4 (§2). A move to hardware tags
   would be a separate, oracle-backed change.
2. **Segment misses in static game code interpret loudly until the title is
   regenerated.** They are counted, recorded in the TCP-visible ring and
   taken down the existing clean-text-miss path (§5.5). There is no
   fail-fast mode.
3. **Extra segments are requested through segment-qualified seeds** (§5.4).
   There is no `game.toml` table.
4. **The overlay cache uses per-segment subdirectories** (`seg-kuseg/`,
   `seg-kseg1/`) under the existing cache tag (§5.7). The filename grammar
   does not change.
