#include "device_abi.h"

/* Transport bring-up only. The planned ARM IREE VM/HAL is not linked yet. */

#ifndef G_POLL_LIMIT
#define G_POLL_LIMIT UINT32_C(1000000)
#endif

static volatile g_mailbox_t *const mailbox =
    (volatile g_mailbox_t *)(uintptr_t)G_MAILBOX_BASE;
static volatile g_descriptor_t *const descriptor =
    (volatile g_descriptor_t *)(uintptr_t)G_DESCRIPTOR_BASE;

static void report(uint32_t result) {
  *(volatile uint32_t *)(uintptr_t)G_HOST_REPORT = result;
}

static int submit(uint32_t operation) {
  /* One command in flight. Publish the sequence after the descriptor. */
  uint32_t seq = mailbox->complete_seq + 1;
  descriptor->operation = operation;
  g_barrier();
  mailbox->request_seq = seq;
  for (uint32_t i = 0; i < G_POLL_LIMIT; ++i) {
    if (mailbox->complete_seq == seq) {
      g_barrier();
      return mailbox->status == G_STATUS_OK;
    }
  }
  return 0;
}

int host_main(void) {
  uint32_t i;
  for (i = 0; i < G_POLL_LIMIT; ++i)
    if (mailbox->ready && mailbox->magic == G_ABI_MAGIC &&
        mailbox->abi_version == G_ABI_VERSION) break;
  if (i == G_POLL_LIMIT) { report(2); return 1; }
  for (i = 0; i < sizeof(g_descriptor_t) / sizeof(uint32_t); ++i)
    ((volatile uint32_t *)descriptor)[i] = 0;
  if (!submit(G_OP_PING)) { report(2); return 1; }

  volatile int8_t *a = (volatile int8_t *)(uintptr_t)G_DEMO_A;
  volatile int8_t *b = (volatile int8_t *)(uintptr_t)G_DEMO_B;
  volatile int32_t *c = (volatile int32_t *)(uintptr_t)G_DEMO_C;
  for (i = 0; i < G_DEMO_DIM * G_DEMO_DIM; ++i) {
    a[i] = (int8_t)((i & 15) - 8);
    b[i] = (i / G_DEMO_DIM == i % G_DEMO_DIM) ? 1 : 0;
    c[i] = INT32_C(0x55555555);
  }
  descriptor->input_addr = G_DEMO_A;
  descriptor->input_bytes = G_DEMO_DIM * G_DEMO_DIM;
  descriptor->weight_addr = G_DEMO_B;
  descriptor->weight_bytes = G_DEMO_DIM * G_DEMO_DIM;
  descriptor->output_addr = G_DEMO_C;
  descriptor->output_bytes = G_DEMO_DIM * G_DEMO_DIM * 4;
  descriptor->m = descriptor->n = descriptor->k = G_DEMO_DIM;
  if (!submit(G_OP_MATMUL_I8)) { report(2); return 1; }
  for (i = 0; i < G_DEMO_DIM * G_DEMO_DIM; ++i)
    if (c[i] != a[i]) { report(2); return 1; }
  report(1);
  return 0;
}
