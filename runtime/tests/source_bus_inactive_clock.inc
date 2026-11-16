/* These scheduler fixtures declare every GPU source controller inactive.
 * They continue testing their real clock/freeze/event assertions. */
uint32_t source_gpu_runtime_cycles_to_event(void){return UINT32_MAX;}
uint32_t gpu_queue_cycles_to_event(void){return UINT32_MAX;}
void source_gpu_runtime_advance(void){}
void gpu_queue_service(void){}
