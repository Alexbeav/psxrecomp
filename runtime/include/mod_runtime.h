#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <filesystem>
#include <string>
#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

namespace PSXRecompV4 {

bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            uint32_t game_entry_pc,
                            const std::filesystem::path& exe_path = {},
                            std::string* error = nullptr);
bool mod_runtime_commit(const std::filesystem::path& disc_path = {},
                        std::string* error = nullptr);
/* Drop the in-session mod plan for a netplay launch without rewriting the
 * user's persisted offline selection on disk. Netplay is always vanilla for
 * now (no synced mod plans). */
bool mod_runtime_clear_for_netplay(std::string* error = nullptr);
const std::string& mod_runtime_fingerprint();
/* How many times this process decoded and hashed a whole disc image for the
 * mod plan. It stays 0 unless an enabled package names a disc_sha256 or
 * carries a derived disc (PS1B-340). */
unsigned mod_runtime_disc_digest_computations_for_tests();
/* The enabled mods a replay must match (PS1B-191); "none" when there are none. */
std::string mod_runtime_replay_fingerprint();
const std::filesystem::path& mod_runtime_effective_disc_path();

#if defined(RECOMP_LAUNCHER)
const ::RecompLauncherCModProvider* mod_runtime_launcher_provider();
#endif

} // namespace PSXRecompV4
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Called before a guest dispatch. Applies the complete main-EXE plan
 * transactionally on the first dispatch to the configured entry point. */
void mod_runtime_on_dispatch(uint32_t target);
/* A full-machine savestate restores guest RAM after the initial entry-point
 * application. Reapply the already-validated main-EXE plan so the current
 * enabled mod selection remains authoritative after the restore. */
void mod_runtime_on_savestate_loaded(void);
/* Invokes activation callbacks for the committed plan. Call after the final
 * launcher commit and before renderer/window initialization. */
void mod_runtime_activate_plugins(void);
void mod_runtime_on_vblank(void);
void mod_runtime_patch_disc_sector(uint32_t lba, int raw_sector,
                                   uint8_t* bytes, uint32_t size);
void mod_runtime_enable_disc_patches(void);

#ifdef __cplusplus
}
#endif
