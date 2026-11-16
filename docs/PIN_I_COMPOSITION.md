# Preliminary Pin I composition

This private candidate combines the standalone replay source e841503c with the
prepared Studio core f96977c2 and UI 0dc6fb75. It is not a product release or a
new replay qualification. The immutable `psx-replay-final-20261105` tag retains
its own source, products, providers and evidence.

The combined source uses overlay ABI 36 and codegen version 18. The historical
upstream v25 fixture remains unchanged. Its 47 callback slots retain their
offsets and types; slot 3 is now named `cpu_charge`. Eight modern callbacks
follow the shared prefix; live instruction-observer forwarding follows those hooks. Source-bus builds append one additional callback and
set the source-bus flavor bit. Existing v26 overlays are not compatible with
this candidate. Layout tests check the actual composed contract.

Snapshots use explicit BOOT17/18 layouts. SPU state contains the base and
private voices, the SPK3 source key/decoder/reverb state, and a 20-byte sample
epoch and guest-clock phase. CD input has one separate SCD2 section. Old BOOT16
and earlier containers are refused. Cross-restore and cache-state fidelity are
not qualified.

This deliberately loses the old e841 positive SPK2 migration behavior. The
coarse `PSX_TAS_RESUME_COMPATIBLE_BUILD` setting does not admit those old
sections into this layout. The old migration proof remains evidence for the
standalone pin. The candidate's regression retains every oracle R1 reverb
value, checks the current wire roundtrip, and requires malformed and legacy
sections to be refused. This is changed compatibility coverage, not a claim
that the old positive migration test still passes.

Sound fixtures use the guest-clock producer and output queue. They do not add
a direct-render compatibility implementation. Disc-scoped state selection
retains the verified product roster, mounts the correct disc before loading,
and restores the previous disc after a failed load. Modern checked loads,
quiet rollback and content namespaces remain in place.

The inherited replay evidence stays on its measured source and provider:
23 cold completions and 3 completions made from matched prefixes and resumed
suffixes. Those results are not executions of this new composition. No new
hardware, product, cache, snapshot compatibility or TAS pass is inferred.

Inherited unit fixtures keep their original behavioral assertions. Snapshot
handoff tests use the real container, CPU wire, I-cache and host registry with
explicit hardware doubles for other devices. Those tests do not qualify
source-profile interpreter, scheduler or GPU snapshot fidelity. The full
GPU/DMA admission test keeps actual GPU/DMA/I-cache state owners and adapts
its valid baseline to BOOT17 and the required CD-input section before testing
malformed GPU state. No old container is relabeled as compatible.

The historical controller-route panel and its key names remain available to
component tests. Modern host integration uses the shared in-game menu; live
controller port swapping through the historical shortcut is not yet wired or
qualified in this preliminary composition.

The source profile retains its real DSM machine wire and scheduler return-TCB
snapshot owner. Products link the route session/observer, publication ring,
state-I/O hashes, text guard and atomic replacement owners. No fixture double
is linked into a product. Source slice admission services devices first and
retains GPU-return deadlines, source DMA halts, access/coprocessor bounds and
delayed GTE value ownership; the modern non-source gate remains separate.
Deadline-cache and phase-profile performance measurements from the standalone
pin do not qualify this composition. The source-profile bound still addresses
the measured retail 2 MiB image; expanded RAM is not source-profile qualified.

The composed slice diagnostic uses psx-slice-diag-v2. It reports actual slice,
IRQ, instruction, last-gate/exit and block-facts counters. Legacy standalone
phase/deadline histograms are not collected here and are not emitted as false
zero measurements. GPU/video and IRQ/field observer values read the actual
scalar owners without servicing devices. No old performance result transfers.

Current failed-suite repair batch restores safety operations lost in composition:
- CLI toolchain read-only and transactional install/recovery/pointer rules coexist with Studio project-local storage and modern native-tool discovery. No global toolchain operation is performed by this repair.
- Preempted IRQ fetch/cache/base-cycle charge is source-profile-only; modern default delivery remains uncharged. Source opcode eligibility and measured source behavior remain intact.
- Explicit critical-section/guest-syscall exception models return the guest vector through the flat dispatcher and reject unsupported nested entry; direct default SYS01/02 behavior remains unchanged.
- Renderer mode/reset callbacks again observe completed register transitions. Depth row reset occurs only on an actual depth change.
- Host-provider fixture uses current BOOT17/18 while retaining MSNP-v1 section import, atomic rejection and provider roundtrip checks. This does not reinstate rejected old complete BOOT/SPK2 wire formats.
- Studio licence is the unchanged canonical Git blob; its existing required hash is retained and LF checkout is specified.
Focused failed-case diagnosis precedes the required complete combined GCC pair. No modern runtime/replay qualification follows from these edits alone.

The next measured subset found the inherited RFE same-thread redirect and outside-window unwind were omitted, leaving nested host frames and continuing the interrupted block on a restored task stack. Restore those exact context-selection guards and real scheduler resume owners, alongside modern callback-context cleanup. No synthetic register/context result replaces the production restore.
Renderer upload spans again include sliced writes; GP1 reset clears the actual source queue and notifies the renderer before resetting movie row history. Fixture-only seams describe modern presentation context with no render-thread replay/depth surface; their original ownership/order assertions remain.
The setup-host compiler probe uses dynamically allocated inherited PATH and restores it after the child, as in the retained safe implementation. CLI toolchain refusals preserve their authored explanatory sentence.

SIO reconciliation restores the actual debug-route poll owner and accepted DualShock 45h bytes without replacing JogCon-specific commands. A refused card extension flush returns the existing serial failure byte. The DTR malformed-byte regression now addresses its same session field inside the preserved replay prefix, before the new modern extension; it does not corrupt a valid modern device selector or replace its expected rejection.
Both disc probes leave their single JSON document after diagnostic messages when a caller combines stdout/stderr. Multi-disc CHD preparation again checks/builds the actual reader before probing or changing configuration.

Renderer callbacks were missing from the composed facade/backend table, so the prior call alone could not synchronize ownership. The real optional facade callback and both GL backend bindings are restored; the render-thread binding acquires its existing synchronized context before the transition. Existing optional-backend/no-context assertions remain.
Modern default SIO retains explicit pre-pad-mode, pre-JogCon and pre-rumble tail readers; omitted fields receive their documented power-on state. Source-profile codecs still require their exact current length. Current-format validation and field range checks remain, and legacy reads do not claim BOOT/SPK2 migration support.

The setup-host synthetic controls exposed additional inherited safety operations omitted by the composition. Restore their actual read-only checks, rejection-aware lookup, interrupted-install recovery, non-destructive health refusal, pointer replacement and rename-before-prune behavior as one batch. Preserve modern non-Windows native-tool policy and Studio project-local cache roots; do not restore the older automatic POSIX portable-pack fallback. All tests operate on authored sandbox packs, not the installed host toolchain.

Measured controller contract conflict: the retained e841 synthetic Pin F identity fixture matches E78B3101B495BF34; the composed default first differs at bus byte57, received byte for digital43h: FFh before,41h now. The modern authored digital-command regression intentionally requires41h. Preserve the modern default and expose the historical plain-digital hi-Z behavior only under PSX_INPUT_ROUTE_PAD_COMMAND_MODEL=pin-f. Unknown nonempty values refuse. Historical golden fixtures select that model explicitly; their exact expected hashes remain unchanged, while modern digital/JogCon/neGcon controls retain their default environment. This is an explicit versioned compatibility scope, not proof of unchanged default behavior or transferred c03 replay qualification. BOOT identity includes the selected command model and a real serializer regression refuses a cross-model load before mutation. TAS config identity already includes the full resolved PSX environment.

CD merge repair: selected source read/pause helpers share their resolver with direct controller callers; default ReadN/ReadS starts the motor once, and Seek completion/GetlocL/GetlocP distinguish paused delivery from a completed seek. Source GetStat observes the retained stopped head instead of spinning it up. Distinct2.4.2/2.7/2.10 tray model selectors remain unchanged.
The composed serializer omitted pending/active CD gain registers. CDF5 appends both eight-bit2x2 matrices, with preflight marker/length checks before mutation. Default legacy CDF2/3/4 forms remain readable with their absent registers initialized to the documented identity matrix; optional source-context states require their current full prefix and SCD2 identity. No cross-restore from the immutable replay pin or old source states is claimed.
The reviewed pure host-inspection boundary is also applied to preliminary IRQ/scheduler code and tested by the same six normal/replay/negative controls. Private origin observers remain; they obtain TCB diagnostics through pure peeks.

The next default-drive repair restores pre-transition Seek ACKs, computes travel before moving the cursor, and starts reads with the selected seek/stream phase. GetlocL starts without a fabricated header; Play and ReadTOC clear completed-seek provenance. Existing synthetic cases, not hardware timing claims, measure these paths.

CDF6 adds the common selected-model identity and the paused/standby timing byte after the gain matrices. A plain explicit-seek selection has no separate clock/audio/head appendix; optional SCD2 retains those actual state owners and its two tray-notification bytes. The common and optional phase must agree before any restore mutation. Legacy default CDF2/3/4/5 remain readable; those formats lack a paused/standby distinction, so that bit can only be inferred from their motor/reading state. Old optional contexts and cross-model identities are rejected, not described as compatible. The receipt-corruption test still corrupts the actual CDF4 Boolean, now before both newer extension tails; the SCD2 size assertion includes the real two tray-notification bytes.
The next controller batch keeps modern default explicit-seek latency, acknowledges Read from its pre-seek state with motor-on admission, clears SEEK at delivery and Pause, and retains the resident read's genuine received-sector/cursor ownership. Asynchronous data-ready presentation reaches INTC with its visible flag; command-store responses retain their existing delay. Missing draw-order fixture commands now actually invoke the production admission checks instead of falling out of main.

CDDA failure diagnosis did not support a new production-audio regression: e841 and the composed controller both produced484200bytes with SHAa3567803fc612ff570ee018c841c54085b70af17a7c5bd1c0cef152adf510e52 for data-play-mode-0, whereas the declared oracle transcript hash isb10ee0db1607c478be33f76bf7dd2159df671b0c3781c78cd93a1ae52ebd97bd. The e841 contract driver tested the four audio-only transcripts, not this additional data-track set. Corpus PSX-CD-013 and PS1G-116 explicitly document this earlier wrong turn: the synthetic data sectors need their MSF/mode header, since those bytes are audibly carried across the two-sector pipeline into audio. Restore the authored fixture correction from91a4d79a (data LBA<90, bytes12..14 MSF and byte15 Mode1), not an emulator model or new expected hash. Original replay-pin qualification remains unchanged; the expanded current unit suite must prove the corrected fixture separately.
The CD failed-case runner initially selected executable targets only from CTest command[0], missing CDDA executables passed as arguments to Python. Those prior CDDA failures therefore do not execute the new synthetic-header correction. The runner now scans all executable arguments in the build directory and measures this serial unit-only job's own descendant commit peak with the existing Windows job helper, rather than applying the generated-BIOS peak. The Pause malformed-state regression now corrupts the actual CDF4 receipt field and CDF3/CDF4 markers; current extension sizes are explicit. The resident Read test captures pre-command admission status and separately checks the scheduled seek/read phase, preserving ACK and state assertions rather than comparing a pre-seek response to post-seek state.
Root independent read7-14047 caught the flags8-only legacy early return bypassing CDF6 model identity. The model-length guard now precedes that return. Focused controls retain default legacy acceptance and require selected flags8/no-appendix legacy refusal with a byte-exact unchanged state. The restored CDDA data-header fixture now matches all nine unchanged transcript hashes cold and checkpointed; remaining fixture scope commands for data track1 and advertised57 tracks/named track2 were absent from the old merged driver. They are restored with explicit three-track backing bounds; source Play uses its already validated named track without scanning unrelated track entries. No track10+ PCM/autopause claim follows.

GPU/MDEC fixture reconciliation keeps unselected GP0 dispatch immediate and tests the existing explicitly selected bounded FIFO machinery, with a default-negative control. The FIFO tests now seed the actual selector/config state their queue assertions require, and corrupt the real queue-count field instead of an obsolete tail offset. GP1 command-buffer reset cancels that selected queue at its state owner. This is unit machinery coverage, not a full source-route initialization claim. Source-bus tests explicitly use the no-scaling clock reciprocal65536 and fraction0, matching the real clock owner; no runtime data definitions are synthesized by the link helper.
Two inherited MDEC tests conflicted: output-lifetime required Busy for every unread word, while mdec_busy_drain and the production contract require Busy clear once the final32-word FIFO load remains. Preserve that existing boundary and check it for every output word in the output-lifetime test, retaining every DMA-ready, exact drain and pixel-packing assertion. Production MDEC semantics are not changed by this fixture correction; no new hardware claim follows.
The next device batch corrects actual default decoder gaps (also present in f969): four-bit mono output must pack two quantized pixels per byte; a trailing FE00 alignment word is not another block. The authored packing and hash-bound MDEC fixtures retain their expectations, and selected source-MDEC behavior is unchanged. The GPU admission fixture now uses the chosen GAME_START wire tag rather than reserved BOOTFLOW. Source-bus callback/clock-generation data are explicitly typed and the link helper refuses those data seams instead of turning them into executable stubs. DMA stop publishes its next packet cursor; a paused packet is replaced on a new cursor or block-mode restart, while same-list resumes preserve the admitted tail. Actual CD/DMA word mismatches now report their first word and FIFO/cycle state for diagnosis; no model offset is guessed.
Current device diagnostics distinguish stale products from runtime failures: the MDEC clean fixture DLL was not rebuilt by the executable-only subset selector. Target selection now includes the actual shared-library artifact. The CD/DMA fixture must seed and observe the selected source FIFO, not modern RB_, when PSX_CD_DMA_MODEL enables that owner; expected words/IRQ/CHCR/rewind assertions are unchanged. The GPU loader now rejects credit above SOURCE_GPU_T_CREDIT_LIMIT before mutation, with the inclusive cap as its positive stream. Finite source-bus units explicitly have no input endpoint and declare the real clock-generation data they use.

The current SPU clock tail is 20 bytes. Cutting four bytes off that payload
is a truncated current record, not a supported pre-clock migration. The old
positive pre-clock restore case is replaced explicitly with rejection plus
unchanged serialized state/output checks. Current-format phase restoration
still tests the first sample boundary after guest execution resumes. This is
another stated compatibility limit, not proof of old snapshot migration.
SPU initialization anchors the guest clock immediately so the first elapsed
sample also reaches the disabled capture ring; it does not wait for the first
register access to choose that epoch.

The GPU prefix now appends an explicit GPR1 marker and independent last-read
bus word after its original 288 bytes. Its current prefix is 296 bytes; source
queue/raster extensions follow it. Earlier candidate GPU sections are refused,
not migrated. The codec preserves the independent idle-info and last-data
latches; AV hashing removes only the producer-owned presentation fields at
276..287 and retains GPR1 and all source/raster state. These unit contracts do
not establish cross-product restore or cache-state replay qualification.

Current repair batch restores requested heartbeat dumps and a complete two-window spin confirmation while retaining profile-diversity filtering. Netplay and replay use the counted loader hold; P2 applied-state observations run at the next input boundary. Rollback fixture now owns and restores CPU bytes instead of refusing every capture. OSD text shares the 38-column window bound. These changed cases are awaiting execution; no complete-suite claim.

The modern prepared runtime menu uses PsxMenuHost provider callbacks and roster-only tray operations, replacing the old standard-widget block and unrestricted file picker. Structural guards now bind those actual owners, queued main-thread edits, live/restart resolution policy and netplay refusal; direct cdrom_replace_disc remains open-before-swap. This does not claim interactive UI qualification.

The GL textured modulation factor now uses the 8-bit neutral value 128 exactly (normalized vertex colors need 255/128, not 2). The retained mask scene measured f9f9f9 before versus native 7fff with neutral modulation. Existing expected white values and mask checks remain unchanged; complete rendering regression is pending. This does not establish full hardware color/rounding parity.

Pending GPU source DMA captures are supported by the composed current wire. The two old capture-negative tests called the writer with NULL, which is not a valid output buffer; they now require successful validation/load/byte-identical rewrite of allocated pending snapshots. Their unrelated bounded-profile rejection cases remain unchanged. Host setup now preserves full recorded disc sets at relaunch and exclusively seeds missing executable-adjacent settings without replacing existing or blocked temporary files. POSIX CLI native-tool preference and host-specific pack checks are restored from the first-party replay base.

The complete runtime collection on1e612da5 compiled successfully and measured
614 passed,12 failed,3 disabled and1 skipped (630 registered). The next repair
batch preserves those raw failures rather than calling passing subsets a
complete check. Renderer-only fixtures use the existing progressive sentinel
-1; zero means a real skipped row parity. Two shared GL runners link that
fixture owner, the extracted reset declares its new last-data latch, and the
GP0 history fixture links real video timing. No rendering expectation changes.

Setup-host forwarding retains explicit headless/replay/no-launcher intent and
seeds only absent product settings with the existing exclusive-copy helper.
The POSIX CLI itself is unchanged from the standalone base: native tools take
precedence, with an already-local validated pack fallback when unavailable.
Its old AST-only test lacked the current dependencies; the replacement tests
native precedence, absent/invalid/valid fallback and no actual PATH/cache
mutation using explicit collaborators. This is expanded current-contract
coverage, not proof that POSIX never activates any local pack.

The first recompiler build exposed the missing legacy boot-key migration owner.
Format2 metadata and old launcher-echo rejection are restored from the replay
base; newly written settings retain deliberate boot choices. Modern built-in
mods continue to own resolution, so the boot migration fixture uses the still
supported window width as its unrelated-setting control and also checks that
the retired supersampling key is dropped. Its boot-choice/legacy assertions
remain intact; this does not restore the removed player scaling fields.

The F5/F6 give-back fixture now executes the real inline main-RAM word load,
with real retail RAM geometry, instead of a removed standalone timing helper.
All recorded vectors remain unchanged. Renderer/interpreter opcode fixtures
link the real I-cache owner and explicit no-device collaborators; unexpected
IRQ/BREAK/device delivery aborts. Their instruction semantics and positive/
negative culling checks remain. CD bridge callbacks refer to the same authored
data-only sector/SubQ view. DMA checkpoint fixtures already include the real
linked-list implementation, so its redundant second translation unit is
removed rather than replacing the implementation. No product stub is added.

The first executable recompiler subset retained15/18passes. Its paused-SubQ
fixture found a real selector omission: the moved GetlocP wrapper admitted
source-read/CDDA selectors but excluded the independently selected drive
profile. That profile's existing idle-head update already computed the right
sector; only admission fell through to the modern last-delivery response.
GetlocP now admits that drive selector locally; all20 measured positions,
clock boundaries, default-profile negative and invalid-head fallback remain.
No position or timing offset is added. Other commands keep their selectors.

Interpreter DIV/DIVU deadlines now name the existing PSX_DIV_LATENCY constant
instead of spelling its identical37-cycle value twice. The timing fixture
still checks the recorded latencies and both emitters; its structural check
now binds the shared fact/stream bound owners and rejects duplicate local DIV
estimates, rather than demanding the old duplicated bound implementation.

### Recompiler collection after runtime qualification (November 6)

The complete recompiler build on 9092d177 passed, but its first complete 396-case collection had 312 passes, 76 failures, four disabled cases and four skips. This is not a complete-pair qualification. The earlier runtime pass remains bound to 883ad29d.

The next batch restores the documented set-file loader behavior from the replay base without removing modern EXE auto-detection or single-game validation. Ordinary LUI output now uses an unsigned C operand, preventing signed left-shift overflow at immediate values 0x8000 and above. Its existing six-boundary and zero-register regression remains unchanged. The source-page test registration now passes its real supported arguments; the BIOS emitter inventory excludes only MinGW's generated linked manifest resource object, retaining all emitter source/header checks. The PGO fixture loses a duplicate keyword, not a check.

The raster and GPU service round-trip fixtures use values inside the real readers' semantic domains; the production validators are unchanged. Distinct unrestricted payload words and record identities still expose order/instance mistakes, while booleans and enums cannot all carry arbitrary distinct high-byte values. Length, cycle/fraction corruption, writer-boundary and bytewise round-trip checks remain. Decoder fixtures explicitly select no mod and an unscaled clock rather than treating missing data symbols as abort-function bytes. The precise scheduler escape fixture counts the new snapshot-host reset collaborator as well as its existing escape assertions. These changes are not claimed passing until the focused run records its outcome.


The next CLI batch restores the first-party replay-base implementations that the composed commands still referenced: set routing, split-prepass reporting, product-shape verification, diagnostic-only builds, report-only diagnostic collection, licence staging and protected product folders during intermediate pruning. The modern Generate disc identity and static-AOT path remain. Diagnostic-only mtime handling preserves both normal and diagnostic trees. The set-disc updater regains its distinct multi-program route while keeping modern relative paths for single-program projects. No actual game generation/build/replay is dispatched by this repair work; its fixtures use synthetic inputs and mock product build consumers. The Windows helper directory guard normalizes trailing separators so a spelling variant cannot overwrite its own running helper. Its existing refusal/unchanged-tree tests remain unchanged.

The remaining CLI dependency batch restores `PSX_EXIT_AFTER_MS` in the real vblank path, outside debug guards. This was present in the replay base: product-shaped PGO training can exit normally and flush profiles without adding the debug server's different control flow. The old host-flag fixture is updated to assert both instrumented and final debug-OFF shapes, and compiles the actual extracted timer at O0/O2 against synthetic SDL ticks to prove disabled/negative and exact-deadline clean-exit behavior. Its mocked configure now emits the real requested cache fields, so production cache-shape validation is exercised rather than mocked away. Explicit `--cmake-extra` flags retain last-value precedence and are validated as requested. Available module import-only licence staging no longer exits when optional TOML packages are absent; actual overlay compilation still refuses without its TOML parser. Repeatable `--set-disc` options are restored on Generate/Rebuild. No game executable is built or played by these focused fixtures.

The CPU batch restores one CPUState-owned value pipeline for ordinary and selected-profile execution, CFG emission and full-function emission. The composition had routed default dirty interpretation to a separate BIOS-only process-global pipeline, while generated code used local same-block pair temporaries. Those cannot carry a pending value across calls, labels, dispatcher returns or savestates. CPUState and its 592-byte field-wise wire already carry the value/rt/age, so no structure or wire version is changed here. Modern mod callbacks, immediate variants, memory-address hooks, static analysis bounds, source-bus/native-span timing, segment dispatch and relocation remain. Host returns preserve pending values; real exception entry commits them. LWL/LWR merges use the pending value without exposing it prematurely. Indirect targets are latched before their slots even when a pending load arrived from another unit. Guest SYSCALL emission again publishes its executing virtual PC and respects a transferred exception. Existing authored CPU goldens and generated-code assertions are unchanged.

Boot image CRC metadata is restored alongside the modern separate dispatch emitter. Its bytes are captured before static analysis trimming and guarded instruction patches. This does not discard segment variants or their range/dispatch checks. No full game collection is rebuilt. Regression checks compile only framework/test targets and synthetic authored inputs; final source qualification still requires the complete same-head pair.

The next emitter-fixture batch restores the existing replay-base `full_function_emitter_test` export modes (authored load-value, alias/admission, orphan-target and syscall fixtures) and its CPU-owned pipeline assertions. The composition had retained only the modern no-argument fallback test; callers requesting an export mode received no files. Both modern mod-dispatch precedence and native-span tests remain in the restored fixture. Local same-block temporary-name checks in the mod-immediate test now bind the real CPU-owned pending slot: all variants/default still emit their actual memory read and preserve the pre-load GPR. No input vectors are changed. Existing L1 programs keep every golden word, including exception and precise controls; their precise fetch links real RAM geometry, and explicitly inactive frontend predicates preserve vanilla branch decisions rather than returning arbitrary decisions.

ROOT independently read the third fixture abort failures at 14:03. The COP0 fixture now defines inactive observer data and still asserts the actual device deadline, CAUSE and PGXP sample at the read before retiring the loaded value through two successor instructions. The scheduler fixture checks IE-disabled interrupt admission at both exact dispatch PCs and counts the calls; it does not fabricate interrupt delivery or clock progress. Full same-head source checks are still pending.

On 8be9cbdb the clean and precise authored L1 matrices passed unchanged, as did mod-immediate selection and the restored orphan-target fixture. Remaining real omissions are repaired in the next batch: full-function exact-instruction resume keys/labels and cold-entry fetches, plus the scheduler's precise-mode reset after longjmp. Resume keys use the modern segment-variant mapping. Native-span accounting is split at admitted interior entries, preserving the original path total rather than skipping or double-charging the entry; the unchanged four-instruction fixture explicitly checks 3+1+2=6 across those entries. Reference block timing is kept separate. The ordinary L1 native driver now links the same real RAM geometry as the precise driver. COP0/refused-text fixtures remove duplicate replay-state definitions because the real interpreter owns that variable.

The device fixture repairs keep strict compiler diagnostics enabled: SPU removes an unused parser cursor and makes a nonnegative voice comparison unsigned; source-bus generated mode removes a duplicate clock definition; CD read-order and DMA-hold fixtures bind real RAM geometry/unscaled clock collaborators. No render pass is selected, and an unexpected render-MMIO policy call aborts. The MDEC invalid-state test now corrupts the actual error word; its former final-byte location is signed chroma history. It retains the cross-model rejection, adds valid chroma-history acceptance and asserts invalid-load atomicity. No cycle/event expected value is changed.

### Branch operands and separate CD checkpoint ownership
The CFG emitter latches a conditional branch before its link write as well as its delay slot. The existing L1 vector with BLTZAL using the link register must retain its expected result. The SPU checkpoint regression now uses the existing separate voice/clock and BS_SEC_SPUCD owners; the composed format intentionally normalizes queued CD samples rather than retaining ring cursor positions or diagnostic counters. No wire change or legacy compatibility claim is made. Malformed CD counts and truncated sections must reject without mutating the queue. Typed fixture observations preserve COP0 deadlines, scheduler resume checks and exact interpreter hand-back publications. Focused and final same-head checks are still required.


The composed overlay ABI is now 36: modern v35 hooks remain in order and the existing pre-instruction observer callbacks are appended before the source-only appendix. The loader and real DLL shim forward live observer admission/flush semantics; older layouts reject. REGIMM links use the explicit runtime PC, so RAM-copied BIOS branch links do not retain ROM PCs. Fetch-model fixtures retain real cache timing and bind their existing clock to observer code; no event or cache expectation is removed. Checks remain pending on this source.


The overlay hash is restored as build-owned output for both the emitter and runtime, with exact emitter-adjacent lookup in the shard tool and staged-SDK fallback. Stale-binary refusal remains mandatory. The ABI fixture again executes its original dedicated accept/reject/replacement scenario alongside the modern segment scenarios. Duplicate legacy card-store filters were removed: only the existing ROM-identity-gated runtime-PC filters remain, disabled for the source bus profile as intended. The syscall predecessor metadata translation now names its runtime PC too. These are pending changed-case checks, not final qualification.


Host integration restores authored early netplay idle/scripted input through the modern virtual-host and transform pipeline; neGcon twist uses its independent one-axis deadzone without replacing remapped or radial stick policies. User settings record actual explicit BIOS choices and legacy-echo notices; netplay settles from kit defaults before explicit test overrides and modern mod-profile policy. Card paths use the existing install-relative writer/normalizing reader. Existing source guards now locate the complete modern capture chain, not an obsolete 700-byte excerpt. Focused checks and final both-tree qualification remain required; no sockets/game/input runtime qualification follows from source guards.


CFG gap targets again partition each other's bodies instead of sharing the original gap end. Long zero-word runs compact their followers while preserving per-word source-bus boundaries, cache/observer calls, load-value retirement and co-simulation; annotations, labels and selected mod hooks stop compaction. Expanded cached followers also retain observer boundaries. No regression vector is changed. The settings_format schema section is restored from its authored original. Focused codegen checks and final same-head pair remain pending.


The zero-run/gap focused batch on6121 passed both original regressions and both modern patch/immediate controls; the generated source-bus fixture reached a new observer seam. Its typed inactive host-observer binding now asserts no observer is installed while real device scheduling remains active. The tools CTest registration again adds every discovered test to the zero-case/skipped-case protection list. The isolated icon resource target explicitly clears inherited include/definition/options. The obsolete clear_generated.sh SOURCE helper is retired to preserve modern bundled generated inputs; this executes no data cleanup. All cleanup of lane data remains deferred.


The selective PGO fixture remains a real baseline/generate/use cycle with original guards and companion checks. Its nested compiler jobs are serial to respect the current host allocation. The existing PSX_RUNTIME_HOT_PGO_TEST_SCRATCH_ROOT option is bound to a short issue-named local directory for MinGW's measured profile-path limit; no profile checks or path guards are disabled.


After ROOT's existing Rule13 read (2026-11-06 15:09 relay), default MMIO reads defer only their completion cost until after the register sample. Default RAM/fast paths, timing-only requests, disabled load-delay behavior and all source-profile sampling paths retain their owners. The CD regression retains every existing event/width/segment assertion and adds actual disabled-delay and half-rate CPU-scaling controls. Overlay preflight records real ABI refusal counters and a persistent explanation on both platforms, without deleting/replacing any path. The resident fixture requires exactly two parent roots and two exact-demand fragment owners rather than assuming two total artifacts. Final qualification still pending.


The resident-overlay regression reached actual missing production APIs: modified-text load bounds, per-page invalidation backoff and their diagnostics were declared but not implemented. The authored replay-base owners are now composed with modern segment-qualified exact entries, range validation, atomic publication and candidate limits. They never mark a data page executable; a CPU-modified text page can load only its segment's exact cached entry, with bounded loads and take-outs. Existing units remain eligible after the load bound, while page backoff leaves rewritten text to interpretation. RAM geometry in the fixture is real; inactive render/mod/cache-isolation collaborators are typed and unexpected device paths abort. The ABI guard observes persistent counters and refusal metadata instead of an unavailable transient log line. Focused regression and final full-pair proof are pending.



The final repair batch retains modern regional/original-disc selection while
restoring matching serial verdicts in both the launcher and scripted starts.
LAN joins carry and verify the build and sanitized game identity before seating
a guest. Startup refusal reports retain the actual reason, redact peer addresses,
and distinguish a canceled launcher from a failed start. Hidden-window and
scripted modes keep their no-dialog behavior. Multi-program starts keep the
shared-card lock. Keyboard-as-pad, rewind/menu, and configurable GunCon reads
are neutral outside the active window; controller input and modern host menus
keep their separate owners.

Default load pacing again uses the bounded sustained-read gate; XA/CD-DA,
netplay, self-check and scripted input routes do not accidentally engage it.
Segment-aware overlay dispatch counts each miss exactly once through its
returned reason. A state blob's resume PC is checked before application; modern
checked loads and quiet rollback still protect post-apply validation failures.
Threaded and direct GL presents share the single swap owner, including sticky
vsync recovery and unchanged telemetry. These are framework tests, not observed
hardware or whole-game qualification.

The BIOS fingerprint now follows the real BIOS target and recursive local
includes using one CR-normalized CMake implementation on every platform.
Native Windows root paths are normalized before macro expansion. Pinned images
remain emitter-verified; unpinned recipes include their byte-exact binary image in the fingerprint.
The old hand-list mode remains explicitly incomplete historical evidence.
All five current BIOS outputs were actually generated from profile-verified
private inputs before recording their fingerprints. Input images stay private.
The complete serial GCC pair remains required; earlier focused passes do not
qualify later source heads or this full composition.

The unified dirty-RAM interpreter retires an aged delayed register value before
calling an instruction mod hook. The immediate successor still observes the
old value; IRQ preemption remains before both retirement and hook dispatch.
Ordinary and source-device execution share this owner. Immediate substitution
still follows the original-word hook and precedes decode. The charged COP0
fixture checks hook-visible values as well as its existing device deadline and
PGXP assertions. This contract does not qualify games or cross-version saves.

## Private 0.1.4 preview batch

F11 is the first fullscreen default, with Alt+Enter and Ctrl+F retained.
F12 records player replays. Explicit unbinds and saved modified bindings
retain their existing semantics. Diagnostic route markers use Shift+F11/F12.

Runtime disc identity and roster comparison share serial normalization for
ordinary four-letter names and five-letter boot names such as SLUSP012.06.
The paired builder normalizes both the roster and log-reader fallback and
requires PSX_DISC_BOOT_SERIAL_VERSION=2 for five-letter boot names. An old
runtime with that new nonstandard roster is refused before build delivery.
Ordinary old-pin builds retain their path.

Admitted input routes preserve SIO presentation across desktop refresh while
host controller handles refresh. RTI2 delivery returns before local physical
and debug presentation policy. Focused fixtures include the production refresh
body and the deliberate old-ownership defect. Visible replay/input and DW7
mid-game disc swap remain pending. MK4 Auto has no accepted repair in this
batch; the forced Digital-lock control does not establish Auto support.

## Private 0.1.5 preview batch

The ordinary SPU decoder requires both SPUCNT unit-enable and IRQ-enable bits
before an IRQ request. Its pitch path uses the preceding hardware voice's
current post-envelope sample before channel volume; private sample-bank voices
remain outside hardware PMON. A high pitch retains integer block overshoot
separately from fractional phase. No snapshot layout/version is changed.

The optional source-profile pitch, voice and refill helpers remain unchanged.
Focused GCC controls cover IRQ enable, ordinary modulation, block crossing,
private sample banks, source-profile rows and source snapshot/reverb behavior.
These results do not qualify listening, hardware accuracy or affected game
replays. Use a fresh same-version state for the short manual audio checks.

### Private Windows 0.1.6

Quick-save uses F5 and quick-load uses Shift+F5 by default. Both bindings are
editable and persist through the launcher. A separate quick slot preserves all
twelve numbered slots and the existing disc/content scopes. Duplicate hotkeys
are highlighted; a simultaneous quick-save/load match refuses the operation.
The existing title, netplay and safe-resume restrictions still apply.

The title option `select_shortcuts = false` disables only the fallback Select
combinations. Explicit saved bindings and direct shortcut fields retain priority.
Captured mouse wheel events use type 5 in the existing mouse event structure;
uncaptured, disallowed, zero, invalid and out-of-range events are rejected.
No new mouse event structure or launcher ABI version is introduced.

The controls tests are engineering fixtures. Fresh same-version state tests do
not promise compatibility with older state files. A real consuming wheel/Select
profile and visible quick-save/load acceptance remain manual checks.

### Private Windows 0.1.7

Restore the retained 513/521 host report accounting: power-on totals remain in
their existing fields. New preload and after-game-entry fields separate BIOS
work from game work. Before entry the after-entry share is zero; differences
saturate at zero if a counter is restored below its snapshot. Per-address rows
move at entry without changing guest memory or breaking table probe chains.
State restoration closes preload before restoring a started-game latch.

Restore the 105 BIOS-phase diagnostic line before entry moves these rows, or
at exit when no game entry occurred. This is telemetry only. It does not repair
slow BIOS execution or establish a BIOS performance improvement.
