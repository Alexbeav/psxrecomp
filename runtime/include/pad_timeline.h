#ifndef PSX_PAD_TIMELINE_H
#define PSX_PAD_TIMELINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Source-owned, title-neutral recording of the final SIO-visible pad state.
 * The file contains input only: no guest RAM, media, or generated code. */
int  pad_timeline_configure(const char *record_path, const char *replay_path,
                            char *error_out, unsigned error_out_size);
int  pad_timeline_is_replay(void);
void pad_timeline_capture(uint64_t guest_frame);
int  pad_timeline_apply(uint64_t guest_frame);
void pad_timeline_close(void);

#ifdef __cplusplus
}
#endif

#endif
