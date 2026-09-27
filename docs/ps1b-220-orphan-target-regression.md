# Orphan indirect-jump declaration regression

The full-function emitter declared an indirect jump target twice when the delay slot lay beyond the function walk. The normal pending-terminator loop already covers orphan JR/JALR instructions. Removing the second declaration loop preserves target snapshots and execution paths.

`orphan_jump_target_codegen` exports six authored cases through the production emitter and compiles their emitted C at O0 and O2. An adjacent function entry bounds each orphan walk before its backed delay slot; complete-slot controls retain the slot in their walk. No function range is widened. The exporter reports zero skipped or interpreted functions.

The test calls the actual generated entry and checks that a slot which overwrites the jump register does not change the published target. JALR also checks the link register, including a slot which reads it. Complete-slot JR/JALR and an orphan direct J are controls. The direct J must have no indirect-target declaration. Assertions are required; NDEBUG causes compilation to fail.

The existing source_fixture_link helper supplies aborting unrelated link symbols. Explicit debug and PGXP hooks are inert; the no-IRQ hook checks the published destination. No fallback executes. The generated destination lies outside this finite backing fixture: its published PC is checked, but destination dispatch is not exercised. This is generated-body architectural coverage, not full-runtime selector, IRQ, timing, retail BIOS, or pending-load qualification.

Run the registered test after building full_function_emitter_test:

    ctest --test-dir <build> -R '^orphan_jump_target_codegen$' --output-on-failure

The Python test can retain authored output with --output-dir. No retail or BIOS bytes are included. Private fail-before and execution receipts bind the original and corrected emitter identities; a changed emitter requires a new generated-cache identity before any separately authorized preflight.
