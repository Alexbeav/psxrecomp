/* ws_branch_site_cases.h: the branch conditions the recompiler emits for the
 * [widescreen.cull] branch-site lists (PS1B-520).
 *
 * One table, two tests:
 *   - recompiler/tests/recompiler_patch_test.cpp checks that
 *     CodeGenerator::generate_branch_condition emits exactly COND (and the
 *     comment TAG) for WORD at a site on LISTS;
 *   - runtime/tests/test_interp_ws_branch_sites.py compiles COND as the
 *     generated code compiles it and checks that the dirty-RAM interpreter
 *     decides WORD at a site on LISTS the same way.
 * So the interpreter is compared with the text the generator really emits, not
 * with a second copy of the rule.
 *
 * X(NAME, LISTS, WORD, COND, TAG)
 *   LISTS: the lists the site is on (bit k = PSX_WS_BRANCH_* kind k of gpu.h).
 *   WORD:  the branch, rs = $t0 (gpr 8), rt = $t1 (gpr 9) for beq/bne, +2.
 *   COND:  the emitted condition, token for token.
 *   TAG:   the emitted comment after COND; "" when the lists do not apply and
 *          the raw condition is emitted. */
#ifndef WS_BRANCH_SITE_CASES_H
#define WS_BRANCH_SITE_CASES_H

#define WS_BS_BLTZ        1u /* bltz_sites */
#define WS_BS_KEEP        2u /* branch_keep_sites */
#define WS_BS_NCLIP_KEEP  4u /* nclip_keep_sites */
#define WS_BS_NCLIP_EXACT 8u /* nclip_exact_sites */

#define WS_BRANCH_SITE_CASES(X)                                                    \
    /* Each list on its own. */                                                    \
    X(bltz_on_bltz_sites, WS_BS_BLTZ, 0x05000002u,                                 \
      psx_ws_cull_bltz(cpu->gpr[8]), "ws cull (left edge)")                        \
    X(bltz_on_nclip_keep, WS_BS_NCLIP_KEEP, 0x05000002u,                           \
      psx_ws_x_margin() > 0 ? 0 : ((int32_t)cpu->gpr[8] < 0), "ws nclip keep")     \
    X(bltz_on_nclip_exact, WS_BS_NCLIP_EXACT, 0x05000002u,                         \
      psx_ws_x_margin() > 0 ? gte_nclip_precise_bltz((int32_t)cpu->gpr[8]) : ((int32_t)cpu->gpr[8] < 0), \
      "ws exact nclip")                                                            \
    X(bltz_on_branch_keep, WS_BS_KEEP, 0x05000002u,                                \
      psx_ws_x_margin() > 0 ? 0 : ((int32_t)cpu->gpr[8] < 0), "ws branch keep")    \
    X(bgez_on_branch_keep, WS_BS_KEEP, 0x05010002u,                                \
      psx_ws_x_margin() > 0 ? 0 : ((int32_t)cpu->gpr[8] >= 0), "ws branch keep")   \
    X(bltzal_on_branch_keep, WS_BS_KEEP, 0x05100002u,                              \
      psx_ws_x_margin() > 0 ? 0 : ((int32_t)cpu->gpr[8] < 0), "ws branch keep")    \
    X(bgezal_on_branch_keep, WS_BS_KEEP, 0x05110002u,                              \
      psx_ws_x_margin() > 0 ? 0 : ((int32_t)cpu->gpr[8] >= 0), "ws branch keep")   \
    X(beq_on_branch_keep, WS_BS_KEEP, 0x11090002u,                                 \
      psx_ws_x_margin() > 0 ? 0 : (cpu->gpr[8] == cpu->gpr[9]), "ws branch keep")  \
    X(bne_on_branch_keep, WS_BS_KEEP, 0x15090002u,                                 \
      psx_ws_x_margin() > 0 ? 0 : (cpu->gpr[8] != cpu->gpr[9]), "ws branch keep")  \
    X(blez_on_branch_keep, WS_BS_KEEP, 0x19000002u,                                \
      psx_ws_x_margin() > 0 ? 0 : ((int32_t)cpu->gpr[8] <= 0), "ws branch keep")   \
    X(bgtz_on_branch_keep, WS_BS_KEEP, 0x1D000002u,                                \
      psx_ws_x_margin() > 0 ? 0 : ((int32_t)cpu->gpr[8] > 0), "ws branch keep")    \
    /* A site on two lists: the order generate_branch_condition tests them. */     \
    X(bltz_on_bltz_and_keep, WS_BS_BLTZ | WS_BS_KEEP, 0x05000002u,                 \
      psx_ws_cull_bltz(cpu->gpr[8]), "ws cull (left edge)")                        \
    X(bltz_on_nclip_keep_and_exact, WS_BS_NCLIP_KEEP | WS_BS_NCLIP_EXACT, 0x05000002u, \
      psx_ws_x_margin() > 0 ? 0 : ((int32_t)cpu->gpr[8] < 0), "ws nclip keep")     \
    X(bltz_on_nclip_exact_and_keep, WS_BS_NCLIP_EXACT | WS_BS_KEEP, 0x05000002u,   \
      psx_ws_x_margin() > 0 ? gte_nclip_precise_bltz((int32_t)cpu->gpr[8]) : ((int32_t)cpu->gpr[8] < 0), \
      "ws exact nclip")                                                            \
    /* The three bltz lists apply to bltz (rt 0) only. */                          \
    X(bltzal_on_bltz_sites, WS_BS_BLTZ, 0x05100002u,                               \
      (int32_t)cpu->gpr[8] < 0, "")                                                \
    X(bgez_on_nclip_keep, WS_BS_NCLIP_KEEP, 0x05010002u,                           \
      (int32_t)cpu->gpr[8] >= 0, "")                                               \
    X(beq_on_nclip_exact, WS_BS_NCLIP_EXACT, 0x11090002u,                          \
      cpu->gpr[8] == cpu->gpr[9], "")                                              \
    /* No list. */                                                                 \
    X(bltz_unlisted, 0u, 0x05000002u, (int32_t)cpu->gpr[8] < 0, "")

#endif
