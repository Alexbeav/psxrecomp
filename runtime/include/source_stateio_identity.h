#ifndef PSX_SOURCE_STATEIO_IDENTITY_H
#define PSX_SOURCE_STATEIO_IDENTITY_H

/* v7 checkpoint identity dimensions. A state saved by one executable, under one
 * configuration, over one route means nothing in another, so all three are part
 * of the manifest and all three are gated at resume.
 *
 * The configuration is the WHOLE resolved PSX_* environment minus an explicit
 * allow-list (see source_tas_stateio.h), not a hand-written flag list: a list
 * from memory is how #8 was missed, and a newly added model flag would slip
 * through it. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
int source_stateio_file_sha256(const char *path, char out[65]);
/* Both computed once per process: the executable and PSX_INPUT_ROUTE_FILE cannot
 * change during a run, and hashing them is the slow part of a capture. */
int source_stateio_exe_sha256(char out[65]);
int source_stateio_route_sha256(char out[65]);
/* "content:<sha256>:<bytes>" of an input file. The config digest binds the
 * content-bound PSX_* variables (source_tas_stateio.h) with it, not by path, so a
 * state resumes wherever the same file lies. */
int source_stateio_content_binding(const char *path, char *out, size_t cap);
int source_stateio_config_digest_hex(char out[65]);
/* The path-bound form, as states saved before content binding carry it.
 * Explicit LEGACY_CLOCK_PATH + LEGACY_CLOCK_SHA256 reconstruct only that old
 * path after verifying current tape bytes; native CD snapshot validation remains
 * required. All other configuration identity values remain exact. */
int source_stateio_config_digest_legacy_hex(char out[65]);
/* 1 when a stored manifest digest matches the current configuration: the
 * content-bound form, or (setting *legacy) the legacy path-bound form with the
 * same paths given again, or the explicit verified clock relocation. 0 otherwise. */
/* Reconstruct only an absent SPU key-off model under an explicit compatible
 * native transition. path_bound retains the existing clock-path/SHA checks.
 * accepts() reports 0=current, 1=legacy clock, 2=model/content, 3=model/clock. */
int source_stateio_config_digest_spu_keyoff_legacy_hex(char out[65], int path_bound);
int source_stateio_config_digest_accepts(const char *stored, int *legacy);
#ifdef __cplusplus
}
#endif

#endif
