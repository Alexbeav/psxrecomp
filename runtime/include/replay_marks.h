#ifndef PSX_REPLAY_MARKS_H
#define PSX_REPLAY_MARKS_H
/* Host annotations only. Frame N is the completed boundary before input N+1.
 * A sidecar never changes the replay, guest input or recorded checkpoints. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define REPLAY_MARKS_MAX 256u
#define REPLAY_MARK_LABEL_MAX 64u
#define REPLAY_MARKS_BYTES_MAX 65536u
typedef struct {
    uint32_t frame;
    char label[REPLAY_MARK_LABEL_MAX + 1];
    int estimated;
    int reached, in_sync; /* playback observation: -1 = exact state unmeasured */
} ReplayMark;
typedef struct {
    unsigned count;
    ReplayMark items[REPLAY_MARKS_MAX];
} ReplayMarks;
int replay_marks_path(const char *replay, char *out, size_t cap);
/* Missing metadata is an empty list. All other faults return a fixed reason.
 * Present metadata is bounded, labels unique, and frames within total. */
const char *replay_marks_read(const char *replay, uint32_t total, ReplayMarks *out);
const char *replay_marks_write(const char *replay, const ReplayMarks *marks);
int replay_marks_add(ReplayMarks *marks, uint32_t frame, const char *label, int estimated);
/* Exact label, or label+unsigned frame offset; no extrapolation past END. */
int replay_marks_resolve(const ReplayMarks *marks, const char *selector,
                         uint32_t total, uint32_t *frame);
void replay_marks_json_string(FILE *f, const char *s);
#endif
