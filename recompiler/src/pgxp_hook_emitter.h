#ifndef PGXP_HOOK_EMITTER_H
#define PGXP_HOOK_EMITTER_H

#include <cstdint>
#include <string>

namespace PSXRecomp {

/* PGXP dataflow-shadowing hook emission (ENHANCEMENTS.md G1.10; runtime
 * pgxp_hooks.h). Shared by BOTH emitters — CodeGenerator (game) and
 * StrictTranslator (BIOS) — so the hook grammar can never drift between them.
 *
 * Wraps the translated statement `code` with a PGXP_*() macro invocation for
 * the instruction classes that move sub-pixel projection provenance. Source
 * operands the statement may clobber are captured into locals BEFORE it;
 * everything else is read after it. In the base build the macros preprocess
 * to ((void)0) and the optimizer erases the dead captures, so base objects
 * are unchanged; only a -DPSX_PGXP=1 TU pays.
 *
 * NOT applied to LWC2/SWC2 — those emissions capture the raw loaded/stored
 * word themselves (a masked GTE register write must validate against the
 * word as loaded, not the register's masked value).
 *
 * Deliberately unhooked: AND/XOR/NOR/SLT-family and the exotic immediates —
 * they only ever DESTROY precision, and the engine's validate-on-read drops
 * their stale shadows without help. */
void append_pgxp_hooks(uint32_t instr, std::string& code);

} // namespace PSXRecomp

#endif /* PGXP_HOOK_EMITTER_H */
