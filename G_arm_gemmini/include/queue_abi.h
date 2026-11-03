#ifndef G_QUEUE_ABI_H
#define G_QUEUE_ABI_H
#include "device_abi.h"

/* ABI v4: query and dispatch requests share a header but use different
 * payloads. Dispatch arrays are packed to their active counts, and dispatch
 * completions carry only sequence/status. Query replies carry reflection. */
#define G_QUEUE_BASE (G_MAILBOX_BASE + 0x1000u)
#define G_QUEUE_VERSION 4u
#define G_QUEUE_CAPACITY 8u
#define G_MAX_BINDINGS 64u
#define G_MAX_CONSTANTS 64u
#define G_NAME_BYTES 128u
#define G_QUERY_TABLE_BASE (G_IO_BASE + G_IO_SIZE - UINT32_C(0x00040000))
#define G_QUERY_TABLE_BYTES UINT32_C(0x00040000)
#define G_QUERY_MAX_EXPORTS 1024u
#define G_MAX_BATCH_WORKGROUPS 65536u
#define G_DOORBELL_ADDR 0x02000000u /* CLINT MSIP, Rocket hart 0 */
#define G_QUEUE_QUERY 5u
#define G_QUEUE_QUERY_BATCH 6u
#define G_QUEUE_DISPATCH_BATCH 8u
#define G_QUEUE_TIMEOUT 7u
#define G_QUEUE_PROTOCOL_ERROR 8u
#define G_QUEUE_PROFILE_DONE 0x50524644u /* PRFD */
#define G_QUEUE_PROFILE_ACK 0x50524641u  /* PRFA */
#define G_QUEUE_PROFILE_DONE_INDEX 0u
#define G_QUEUE_PROFILE_ACK_INDEX 1u

typedef struct {
  uint32_t workgroup_size[3], workgroup_count[3], workgroup_id[3];
  uint32_t constant_count, binding_count, executable_constant_count;
} g_queue_dispatch_payload_t;

typedef struct {
  uint32_t address, bytes;
} g_queue_binding_t;

/* Host and Rocket share this fixed-width export reflection record. */
typedef struct {
  uint32_t flags, constant_count, binding_count;
  uint32_t workgroup_size[3];
  uint32_t export_name_bytes;
  char export_name[G_NAME_BYTES];
} g_queue_query_export_t;

typedef struct {
  uint32_t table_address, table_bytes;
} g_queue_query_batch_payload_t;

#define G_QUEUE_DISPATCH_FIXED_BYTES ((uint32_t)sizeof(g_queue_dispatch_payload_t))
#define G_QUEUE_DISPATCH_MAX_BYTES (G_QUEUE_DISPATCH_FIXED_BYTES + \
    4u * (G_MAX_CONSTANTS + G_MAX_CONSTANTS) + 8u * G_MAX_BINDINGS)
#define G_QUEUE_PAYLOAD_BYTES \
    (G_QUEUE_DISPATCH_MAX_BYTES > G_NAME_BYTES \
        ? G_QUEUE_DISPATCH_MAX_BYTES : G_NAME_BYTES)

typedef struct {
  uint32_t sequence, operation, library_id, ordinal;
  uint8_t payload[G_QUEUE_PAYLOAD_BYTES];
} g_queue_request_t;

typedef struct {
  uint32_t sequence, status;
  uint32_t library_id, export_count;
  uint32_t flags, constant_count, binding_count, executable_constant_count;
  uint32_t workgroup_size[3], export_name_bytes;
  char export_name[G_NAME_BYTES];
} g_queue_completion_t;

typedef struct {
  uint32_t magic, abi_version, ready, epoch;
  uint32_t submit_head, submit_tail, complete_head, complete_tail;
  uint32_t reserved[8];
  g_queue_request_t submit[G_QUEUE_CAPACITY];
  g_queue_completion_t complete[G_QUEUE_CAPACITY];
} g_queue_t;

static inline g_queue_dispatch_payload_t *g_queue_dispatch_payload(
    g_queue_request_t *request) {
  return (g_queue_dispatch_payload_t *)request->payload;
}
static inline const g_queue_dispatch_payload_t *g_queue_dispatch_payload_const(
    const g_queue_request_t *request) {
  return (const g_queue_dispatch_payload_t *)request->payload;
}
static inline uint32_t *g_queue_dispatch_constants(
    g_queue_dispatch_payload_t *dispatch) {
  return (uint32_t *)(dispatch + 1);
}
static inline const uint32_t *g_queue_dispatch_constants_const(
    const g_queue_dispatch_payload_t *dispatch) {
  return (const uint32_t *)(dispatch + 1);
}
static inline uint32_t *g_queue_dispatch_executable_constants(
    g_queue_dispatch_payload_t *dispatch) {
  return g_queue_dispatch_constants(dispatch) + dispatch->constant_count;
}
static inline const uint32_t *g_queue_dispatch_executable_constants_const(
    const g_queue_dispatch_payload_t *dispatch) {
  return g_queue_dispatch_constants_const(dispatch) + dispatch->constant_count;
}
static inline g_queue_binding_t *g_queue_dispatch_bindings(
    g_queue_dispatch_payload_t *dispatch) {
  return (g_queue_binding_t *)(g_queue_dispatch_executable_constants(dispatch) +
                               dispatch->executable_constant_count);
}
static inline const g_queue_binding_t *g_queue_dispatch_bindings_const(
    const g_queue_dispatch_payload_t *dispatch) {
  return (const g_queue_binding_t *)(
      g_queue_dispatch_executable_constants_const(dispatch) +
      dispatch->executable_constant_count);
}
static inline int g_queue_operation_is_dispatch(uint32_t operation) {
  return operation == G_OP_DISPATCH ||
         operation == G_QUEUE_DISPATCH_BATCH;
}

static inline size_t g_queue_request_bytes(const g_queue_request_t *request) {
  if (request->operation == G_QUEUE_QUERY) {
    for (size_t i = 0; i < G_NAME_BYTES; ++i) {
      if (!request->payload[i])
        return offsetof(g_queue_request_t, payload) + ((i + 4u) & ~(size_t)3u);
    }
    return sizeof(*request);
  }
  if (request->operation == G_QUEUE_QUERY_BATCH)
    return offsetof(g_queue_request_t, payload) + G_NAME_BYTES +
           sizeof(g_queue_query_batch_payload_t);
  if (!g_queue_operation_is_dispatch(request->operation))
    return sizeof(*request);
  const g_queue_dispatch_payload_t *d = g_queue_dispatch_payload_const(request);
  if (d->constant_count > G_MAX_CONSTANTS ||
      d->executable_constant_count > G_MAX_CONSTANTS ||
      d->binding_count > G_MAX_BINDINGS) return sizeof(*request);
  return offsetof(g_queue_request_t, payload) + G_QUEUE_DISPATCH_FIXED_BYTES +
      4u * (d->constant_count + d->executable_constant_count) +
      8u * d->binding_count;
}

/* The Rocket reads the count prefix from volatile SRAM before deciding how
 * many packed payload bytes to copy into its local request. Invalid counts
 * select the full bounded slot so dispatch validation can reject them. */
static inline size_t g_queue_request_bytes_volatile(
    const volatile g_queue_request_t *request) {
  if (request->operation == G_QUEUE_QUERY) {
    for (size_t i = 0; i < G_NAME_BYTES; ++i) {
      if (!request->payload[i])
        return offsetof(g_queue_request_t, payload) + ((i + 4u) & ~(size_t)3u);
    }
    return sizeof(*request);
  }
  if (request->operation == G_QUEUE_QUERY_BATCH)
    return offsetof(g_queue_request_t, payload) + G_NAME_BYTES +
           sizeof(g_queue_query_batch_payload_t);
  if (!g_queue_operation_is_dispatch(request->operation))
    return sizeof(*request);
  const volatile g_queue_dispatch_payload_t *d =
      (const volatile g_queue_dispatch_payload_t *)request->payload;
  if (d->constant_count > G_MAX_CONSTANTS ||
      d->executable_constant_count > G_MAX_CONSTANTS ||
      d->binding_count > G_MAX_BINDINGS) return sizeof(*request);
  return offsetof(g_queue_request_t, payload) + G_QUEUE_DISPATCH_FIXED_BYTES +
      4u * (d->constant_count + d->executable_constant_count) +
      8u * d->binding_count;
}

static inline size_t g_queue_completion_fixed_bytes(uint32_t operation) {
  return operation == G_QUEUE_QUERY || operation == G_QUEUE_QUERY_BATCH
      ? offsetof(g_queue_completion_t, export_name)
      : offsetof(g_queue_completion_t, library_id);
}
static inline size_t g_queue_completion_bytes(uint32_t operation,
                                               const g_queue_completion_t *c) {
  if (operation != G_QUEUE_QUERY) return g_queue_completion_fixed_bytes(operation);
  if (c->export_name_bytes > G_NAME_BYTES || (c->export_name_bytes & 3u))
    return sizeof(*c);
  return offsetof(g_queue_completion_t, export_name) + c->export_name_bytes;
}

#ifdef __cplusplus
#define G_QUEUE_ASSERT static_assert
#else
#define G_QUEUE_ASSERT _Static_assert
#endif
G_QUEUE_ASSERT(sizeof(g_queue_dispatch_payload_t) == 48,
               "dispatch prefix ABI size");
G_QUEUE_ASSERT(sizeof(g_queue_binding_t) == 8, "binding ABI size");
G_QUEUE_ASSERT(offsetof(g_queue_query_export_t, export_name) == 28,
               "query export prefix ABI size");
G_QUEUE_ASSERT(sizeof(g_queue_query_batch_payload_t) == 8,
               "query batch ABI size");
G_QUEUE_ASSERT(sizeof(g_queue_request_t) == 1088, "request slot ABI size");
G_QUEUE_ASSERT(sizeof(g_queue_completion_t) == 176, "completion ABI size");
G_QUEUE_ASSERT(offsetof(g_queue_request_t, payload) == 16,
               "request header ABI size");
G_QUEUE_ASSERT(offsetof(g_queue_completion_t, export_name) == 48,
               "query completion prefix ABI size");
G_QUEUE_ASSERT(offsetof(g_queue_t, submit) == 64, "queue header ABI size");
G_QUEUE_ASSERT(sizeof(g_queue_t) <= 0xf000, "queue exceeds shared SRAM");
#undef G_QUEUE_ASSERT

static inline void g_copy_from_shared(void *dst, const volatile void *src,
                                      size_t bytes) {
  uint32_t *d = (uint32_t *)dst;
  const volatile uint32_t *s = (const volatile uint32_t *)src;
  for (size_t i = 0; i < bytes / 4; ++i) d[i] = s[i];
}
static inline void g_copy_to_shared(volatile void *dst, const void *src,
                                    size_t bytes) {
  volatile uint32_t *d = (volatile uint32_t *)dst;
  const uint32_t *s = (const uint32_t *)src;
  for (size_t i = 0; i < bytes / 4; ++i) d[i] = s[i];
}
#endif
