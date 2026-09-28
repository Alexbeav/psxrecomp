/* Per-file SHA-256 with a persistent cache (PS1B-191).
 *
 * A replay's identity includes the disc digest, which means hashing the whole
 * image. The cache file keeps one line per hashed file:
 *   <size>\t<mtime>\t<spot>\t<sha256 hex>\t<path>
 * where <spot> hashes the first and last 64 KiB. A line is reused only when
 * path, size, mtime and spot all match, so a changed or replaced image (even a
 * same-size rewrite that kept its mtime, unless it only changed bytes in the
 * middle) is hashed again. Lines are appended; stale lines never match. */
#ifndef DISC_DIGEST_CACHE_H
#define DISC_DIGEST_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The cache file; NULL or "" hashes every time without a cache. */
void disc_digest_cache_set_path(const char *cache_file);
/* 1 and the digest in out, or 0 when the file cannot be read. */
int disc_digest_cache_sha256(const char *path, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif
