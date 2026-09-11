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
#ifdef __cplusplus
extern "C" {
#endif
int source_stateio_file_sha256(const char *path, char out[65]);
int source_stateio_exe_sha256(char out[65]);
int source_stateio_config_digest_hex(char out[17]);
#ifdef __cplusplus
}
#endif

#endif
