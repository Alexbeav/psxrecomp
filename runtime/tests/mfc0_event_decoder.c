/* Production interpreter decoder with per-instruction cycle charging on, for
 * test_mfc0_event_sample.c (PS1B-248). */
#define PSX_ENABLE_BLOCK_CYCLES 1
#include "load_delay_decoder.c"
