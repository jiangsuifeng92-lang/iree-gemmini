#ifndef G_NPU_DISPATCH_H
#define G_NPU_DISPATCH_H
#include "queue_abi.h"
uint32_t g_dispatch_initialize(void);
uint32_t g_dispatch_execute(const g_queue_request_t *request,
                            g_queue_completion_t *completion);
uint64_t g_dispatch_profile_gemmini_cycles(void);
uint32_t g_dispatch_profile_gemmini_calls(void);
#endif
