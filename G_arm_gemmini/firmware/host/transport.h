#ifndef G_HOST_TRANSPORT_H
#define G_HOST_TRANSPORT_H
#include "queue_abi.h"
typedef struct {
  volatile g_queue_t *queue;
  uint32_t epoch, next_sequence, poisoned;
} g_transport_t;
uint32_t g_transport_open(g_transport_t *transport);
uint32_t g_transport_call(g_transport_t *transport, g_queue_request_t *request,
                         g_queue_completion_t *completion);
#endif
