/* Disc digest cache (PS1B-191): a file's SHA-256 is reused from the cache
 * file while its path, size and mtime match, and recomputed when they do not.
 * Keeps a replay's disc identity from re-reading a whole image (often on a
 * NAS) at every game start. */
#include "disc_digest_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static void hexs(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; ++i) sprintf(out + 2 * i, "%02x", d[i]);
}

int main(int argc, char **argv) {
    char dir[512], disc[600], cache[600], text[4096], h[65];
    uint8_t d[32];
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : ".");
    snprintf(disc, sizeof disc, "%s/digest_cache_disc.bin", dir);
    snprintf(cache, sizeof cache, "%s/digest_cache.tsv", dir);
    remove(cache);
    write_file(disc, "abc");

    disc_digest_cache_set_path(cache);
    CHECK(disc_digest_cache_sha256(disc, d), "hash a file");
    hexs(d, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of abc");

    /* The cache file now names the file; forge its digest to prove reuse. */
    FILE *f = fopen(cache, "rb");
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    char *hit = strstr(text, "ba7816bf");
    CHECK(hit != NULL, "cache line written");
    if (hit) memset(hit, '0', 64);
    write_file(cache, text);
    CHECK(disc_digest_cache_sha256(disc, d), "hash again");
    hexs(d, h);
    CHECK(!strcmp(h, "0000000000000000000000000000000000000000000000000000000000000000"),
          "a matching path, size and mtime reuse the cached digest");

    /* A changed file (different size) is hashed again. */
    write_file(disc, "abcd");
    CHECK(disc_digest_cache_sha256(disc, d), "hash the changed file");
    hexs(d, h);
    CHECK(!strcmp(h, "88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589"),
          "a changed file is hashed, not taken from the cache");

    /* Without a cache path the file is always hashed. */
    disc_digest_cache_set_path("");
    write_file(disc, "abc");
    CHECK(disc_digest_cache_sha256(disc, d), "hash without cache");
    hexs(d, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "no cache: real digest");
    CHECK(!disc_digest_cache_sha256("does/not/exist.bin", d), "a missing file fails");
    remove(disc);
    remove(cache);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: disc digest cache");
    return 0;
}
