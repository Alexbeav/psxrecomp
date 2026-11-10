#ifndef PSX_CDROM_RANDOM_TAPE_H
#define PSX_CDROM_RANDOM_TAPE_H
/* Generic bounded word source for explicit device-clock experiments.
 * This reader contains no generator algorithm, seed, title or movie data. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx_sha256.h"

typedef struct CdRandomTape {
    uint8_t *bytes;
    uint32_t count, cursor;
    uint8_t sha256[32];
} CdRandomTape;

static uint32_t cd_tape_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

static int cd_tape_load(CdRandomTape *tape, const char *path) {
    static const uint8_t magic[16] = "PSX-CD-RNG1";
    uint8_t header[20];
    CdRandomTape next = {0};
    FILE *file = fopen(path,"rb");
    if (!file) return 0;
    if (fread(header,1,sizeof(header),file)!=sizeof(header) || memcmp(header,magic,16)) {
        fclose(file); return 0;
    }
    next.count=cd_tape_le32(header+16);
    if (!next.count || next.count>1048576u) { fclose(file); return 0; }
    size_t size=(size_t)next.count*4u;
    next.bytes=(uint8_t *)malloc(size);
    if (!next.bytes) { fclose(file); return 0; }
    if (fread(next.bytes,1,size,file)!=size || fgetc(file)!=EOF || ferror(file)) {
        free(next.bytes); fclose(file); return 0;
    }
    fclose(file);
    psx_sha256_ctx hash;
    psx_sha256_init(&hash);
    psx_sha256_update(&hash,header,sizeof(header));
    psx_sha256_update(&hash,next.bytes,size);
    psx_sha256_final(&hash,next.sha256);
    free(tape->bytes);
    *tape=next;
    return 1;
}

static int cd_tape_bounded(CdRandomTape *tape, uint32_t maximum, uint32_t *value) {
    uint32_t mask=maximum;
    for (unsigned shift=1;shift<32;shift*=2) mask |= mask>>shift;
    while (tape->cursor<tape->count) {
        uint32_t candidate=cd_tape_le32(tape->bytes+(size_t)tape->cursor++*4u)&mask;
        if (candidate<=maximum) { *value=candidate; return 1; }
    }
    return 0; /* Exhaustion never wraps or supplies a made-up value. */
}

/* A longer resource may restore an old checkpoint only when every old word
 * and its original length-bearing header have exactly the saved identity. */
static int cd_tape_identity_matches(const CdRandomTape *tape,
                                    const uint8_t identity[32],
                                    uint32_t count, uint32_t cursor) {
    if (!count || count>tape->count || cursor>count || !tape->bytes) return 0;
    if (count==tape->count) return !memcmp(identity,tape->sha256,32);
    uint8_t header[20]="PSX-CD-RNG1", digest[32];
    for(unsigned i=0;i<4;i++)header[16+i]=(uint8_t)(count>>(8*i));
    psx_sha256_ctx hash;
    psx_sha256_init(&hash);
    psx_sha256_update(&hash,header,sizeof(header));
    psx_sha256_update(&hash,tape->bytes,(size_t)count*4u);
    psx_sha256_final(&hash,digest);
    return !memcmp(identity,digest,32);
}

static int cd_tape_extend(CdRandomTape *tape, const char *path) {
    CdRandomTape next={0};
    if(!cd_tape_load(&next,path))return 0;
    if(next.count<=tape->count ||
       !cd_tape_identity_matches(&next,tape->sha256,tape->count,tape->cursor)) {
        free(next.bytes);return 0;
    }
    next.cursor=tape->cursor;
    free(tape->bytes);*tape=next;return 1;
}

static int cd_tape_restore(CdRandomTape *tape, const uint8_t identity[32],
                           uint32_t count, uint32_t cursor) {
    if (!cd_tape_identity_matches(tape,identity,count,cursor)) return 0;
    tape->cursor=cursor;
    return 1;
}

static void cd_tape_reset(CdRandomTape *tape) { tape->cursor=0; }
static void cd_tape_free(CdRandomTape *tape) {
    free(tape->bytes); memset(tape,0,sizeof(*tape));
}
#endif
