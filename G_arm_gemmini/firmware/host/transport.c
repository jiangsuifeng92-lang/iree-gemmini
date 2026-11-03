#include "transport.h"
#include "diagnostics.h"
#include <string.h>
#ifndef G_QUEUE_POLL_LIMIT
#define G_QUEUE_POLL_LIMIT 100000000u
#endif

uint32_t g_transport_open(g_transport_t *t) {
  t->queue = (volatile g_queue_t *)(uintptr_t)G_QUEUE_BASE;
  t->poisoned = 0;
  t->next_sequence = 0;
  g_puts("ARM NPU: wait for queue ready\n");
  for (uint32_t i = 0; i < G_QUEUE_POLL_LIMIT; ++i) {
    if (!t->queue->ready) continue;
    g_puts("ARM NPU: queue ready, magic="); g_put_u32(t->queue->magic);
    g_puts(" version="); g_put_u32(t->queue->abi_version); g_putchar('\n');
    g_barrier();
    if (t->queue->magic != G_ABI_MAGIC ||
        t->queue->abi_version != G_QUEUE_VERSION) return G_STATUS_BAD_ABI;
    t->epoch = t->queue->epoch;
    if (t->queue->submit_tail != t->queue->submit_head ||
        t->queue->complete_tail != t->queue->complete_head)
      return G_QUEUE_PROTOCOL_ERROR;
    return G_STATUS_OK;
  }
  return G_QUEUE_TIMEOUT;
}

uint32_t g_transport_call(g_transport_t *t, g_queue_request_t *r,
                         g_queue_completion_t *c) {
  volatile g_queue_t *q = t->queue;
  if (t->poisoned || !q->ready || q->epoch != t->epoch)
    return G_QUEUE_PROTOCOL_ERROR;
  uint32_t tail = q->submit_tail;
  if ((uint32_t)(tail - q->submit_head) >= G_QUEUE_CAPACITY)
    return G_STATUS_BUSY;
  r->sequence = ++t->next_sequence;
  g_copy_to_shared(&q->submit[tail % G_QUEUE_CAPACITY], r,
                   g_queue_request_bytes(r));
  g_barrier();
  q->submit_tail = tail + 1;
  g_barrier();
  *(volatile uint32_t *)(uintptr_t)G_DOORBELL_ADDR = 1;
  for (uint32_t i = 0; i < G_QUEUE_POLL_LIMIT; ++i) {
    if (!q->ready || q->epoch != t->epoch) break;
    uint32_t head = q->complete_head;
    if (head == q->complete_tail) continue;
    g_barrier();
    volatile g_queue_completion_t *slot = &q->complete[head % G_QUEUE_CAPACITY];
    c->sequence = 0;
    c->status = 0;
    g_copy_from_shared(c, slot, offsetof(g_queue_completion_t, library_id));
    if (c->sequence == r->sequence && !c->status &&
        (r->operation == G_QUEUE_QUERY ||
         r->operation == G_QUEUE_QUERY_BATCH)) {
      g_copy_from_shared(c, slot, offsetof(g_queue_completion_t, export_name));
      if (r->operation == G_QUEUE_QUERY) {
        if (c->export_name_bytes > G_NAME_BYTES ||
            (c->export_name_bytes & 3u)) {
          q->complete_head = head + 1;
          t->poisoned = 1;
          return G_QUEUE_PROTOCOL_ERROR;
        }
        g_copy_from_shared(c, slot,
                           g_queue_completion_bytes(r->operation, c));
      }
    }
    g_barrier();
    q->complete_head = head + 1;
    if (c->sequence != r->sequence) break;
    return c->status;
  }
  /* Do not reuse buffers/queue after timeout: device may still be executing. */
  t->poisoned = 1;
  return G_QUEUE_TIMEOUT;
}
