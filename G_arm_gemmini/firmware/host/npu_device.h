#ifndef G_NPU_DEVICE_H
#define G_NPU_DEVICE_H
#include "iree/hal/api.h"
iree_status_t g_npu_device_create(iree_allocator_t allocator,
                                iree_hal_device_t **out_device);
uint32_t g_npu_query_count(void);
uint32_t g_npu_dispatch_count(void);
void g_npu_profile_finish(void);
#endif
