#ifndef PSX_INPUT_INSTRUCTION_HISTOGRAM_H
#define PSX_INPUT_INSTRUCTION_HISTOGRAM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern int g_input_instruction_histogram_active;
extern void (*g_input_instruction_histogram_callback)(uint32_t pc);
void input_instruction_histogram_sample(uint32_t pc);
#ifdef __cplusplus
}
#endif
#endif
