/* Rocket device executor. No VMFB interpretation, IREE VM or model scheduling.
 * Native RISC-V objects are linked independently from the Arm firmware. */
#include "dispatch.h"
#include "iree/base/api.h"
#include "iree/hal/local/executable_environment.h"
#include "iree/hal/local/executable_library_util.h"
#include "model_registry.h" /* generated registry of native query functions */
#include <string.h>

extern int gemmini_matmul_i8(const int8_t *, size_t, size_t,
    const int8_t *, size_t, size_t, int32_t *, size_t, size_t,
    size_t, size_t, size_t);
extern int gemmini_conv2d_i8(const int8_t *, size_t, size_t, size_t,
    const int8_t *, size_t, size_t, int32_t *, size_t, size_t,
    size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t);

static const iree_hal_executable_library_v0_t *libraries[G_LIBRARY_COUNT];
static iree_hal_executable_environment_v0_t environments[G_LIBRARY_COUNT];
/* Rocket-local workgroup scratch, unrelated to Arm virtual/native pointers. */
#define G_LOCAL_MEMORY_BYTES (256u * 1024u)
static uint8_t local_memory[G_LOCAL_MEMORY_BYTES] __attribute__((aligned(64)));
static uint64_t gemmini_call_cycles;
static uint32_t gemmini_call_count;

static inline uint64_t g_read_cycle(void) {
  uint64_t value;
  __asm__ volatile("rdcycle %0" : "=r"(value) :: "memory");
  return value;
}

static int gemmini_matmul_i8_profile(const int8_t *a, size_t a_off,
    size_t a_rs, const int8_t *b, size_t b_off, size_t b_rs, int32_t *c,
    size_t c_off, size_t c_rs, size_t m, size_t n, size_t k) {
  uint64_t start = g_read_cycle();
  int result = gemmini_matmul_i8(a, a_off, a_rs, b, b_off, b_rs,
                                 c, c_off, c_rs, m, n, k);
  gemmini_call_cycles += g_read_cycle() - start;
  ++gemmini_call_count;
  return result;
}

static int gemmini_conv2d_i8_profile(const int8_t *input, size_t in_off,
    size_t in_rs, size_t in_ws, const int8_t *weights, size_t w_off,
    size_t w_rs, int32_t *output, size_t out_off, size_t out_rs,
    size_t h, size_t w, size_t channels, size_t oh, size_t ow,
    size_t out_channels, size_t kh, size_t kw, size_t stride) {
  uint64_t start = g_read_cycle();
  int result = gemmini_conv2d_i8(input, in_off, in_rs, in_ws,
      weights, w_off, w_rs, output, out_off, out_rs, h, w, channels,
      oh, ow, out_channels, kh, kw, stride);
  gemmini_call_cycles += g_read_cycle() - start;
  ++gemmini_call_count;
  return result;
}

uint64_t g_dispatch_profile_gemmini_cycles(void) { return gemmini_call_cycles; }
uint32_t g_dispatch_profile_gemmini_calls(void) { return gemmini_call_count; }

static int import_thunk(iree_hal_executable_import_v0_t fn, void *params,
                        void *context, void *reserved) {
  return fn(params, context, reserved);
}
static iree_status_t resolve_imports(void *self, iree_host_size_t count,
    const char *const *names, void **functions, void **contexts,
    iree_hal_executable_import_resolution_t *resolution) {
  (void)self;
  if (resolution) *resolution = 0;
  for (iree_host_size_t i = 0; i < count; ++i) {
    if (!strcmp(names[i], "gemmini_matmul_i8"))
      functions[i] = (void *)(uintptr_t)&gemmini_matmul_i8_profile;
    else if (!strcmp(names[i], "gemmini_conv2d_i8"))
      functions[i] = (void *)(uintptr_t)&gemmini_conv2d_i8_profile;
    else return iree_make_status(IREE_STATUS_NOT_FOUND, "unresolved NPU import: %s", names[i]);
    contexts[i] = NULL;
  }
  return iree_ok_status();
}
uint32_t g_dispatch_initialize(void) {
  iree_allocator_t a = iree_allocator_system();
  for (uint32_t i = 0; i < G_LIBRARY_COUNT; ++i) {
    iree_hal_executable_environment_initialize(a, &environments[i]);
    const iree_hal_executable_library_header_t *const *header =
        g_library_queries[i](IREE_HAL_EXECUTABLE_LIBRARY_VERSION_LATEST, &environments[i]);
    if (!header || (*header)->version != IREE_HAL_EXECUTABLE_LIBRARY_VERSION_LATEST)
      return G_STATUS_BAD_ABI;
    libraries[i] = (const iree_hal_executable_library_v0_t *)header;
    if (!libraries[i]->exports.attrs ||
        libraries[i]->constants.count > G_MAX_CONSTANTS) return G_STATUS_UNSUPPORTED;
    iree_status_t status = iree_hal_executable_library_initialize_imports(
        &environments[i], (iree_hal_executable_import_provider_t){
          .self = NULL, .resolve = resolve_imports},
        &libraries[i]->imports, import_thunk, a);
    if (!iree_status_is_ok(status)) { iree_status_ignore(status); return G_STATUS_UNSUPPORTED; }
  }
  return G_STATUS_OK;
}
static uint32_t query(const g_queue_request_t *r, g_queue_completion_t *c) {
  if (!memchr(r->payload, 0, G_NAME_BYTES)) return G_STATUS_BAD_ABI;
  for (uint32_t i = 0; i < G_LIBRARY_COUNT; ++i) {
    const iree_hal_executable_library_v0_t *l = libraries[i];
    if (strcmp((const char *)r->payload, l->header->name)) continue;
    if (r->ordinal >= l->exports.count) return G_STATUS_BAD_ABI;
    const iree_hal_executable_dispatch_attrs_v0_t *a = &l->exports.attrs[r->ordinal];
    c->library_id = i + 1; /* zero is never a valid registry handle */
    c->export_count = l->exports.count;
    c->executable_constant_count = l->constants.count;
    c->flags = a->flags;
    c->constant_count = a->constant_count;
    c->binding_count = a->binding_count;
    c->workgroup_size[0] = a->workgroup_size_x;
    c->workgroup_size[1] = a->workgroup_size_y;
    c->workgroup_size[2] = a->workgroup_size_z;
    const char *name = l->exports.names ? l->exports.names[r->ordinal] : NULL;
    if (name) {
      size_t size = strlen(name);
      if (size >= G_NAME_BYTES) return G_STATUS_UNSUPPORTED;
      memcpy(c->export_name, name, size + 1);
      c->export_name_bytes = (uint32_t)((size + 1 + 3) & ~(size_t)3);
    }
    return G_STATUS_OK;
  }
  printf("NPU QUERY unsupported library '%s'; registered:",
         (const char *)r->payload);
  for (uint32_t i = 0; i < G_LIBRARY_COUNT; ++i)
    printf(" '%s'", libraries[i]->header->name);
  putchar('\n');
  return G_STATUS_UNSUPPORTED;
}
static uint32_t query_batch(const g_queue_request_t *r,
                            g_queue_completion_t *c) {
  if (!memchr(r->payload, 0, G_NAME_BYTES)) return G_STATUS_BAD_ABI;
  const g_queue_query_batch_payload_t *request =
      (const g_queue_query_batch_payload_t *)(r->payload + G_NAME_BYTES);
  if (request->table_address != G_QUERY_TABLE_BASE ||
      request->table_bytes > G_QUERY_TABLE_BYTES ||
      !g_range_in_io(request->table_address, request->table_bytes))
    return G_STATUS_BAD_ADDRESS;
  for (uint32_t library_index = 0; library_index < G_LIBRARY_COUNT;
       ++library_index) {
    const iree_hal_executable_library_v0_t *library = libraries[library_index];
    if (strcmp((const char *)r->payload, library->header->name)) continue;
    if (!library->exports.count || library->exports.count > G_QUERY_MAX_EXPORTS ||
        library->exports.count > request->table_bytes /
            (offsetof(g_queue_query_export_t, export_name) + G_NAME_BYTES))
      return G_STATUS_BAD_SHAPE;
    c->library_id = library_index + 1;
    c->export_count = library->exports.count;
    c->executable_constant_count = library->constants.count;
    volatile uint8_t *table =
        (volatile uint8_t *)(uintptr_t)request->table_address;
    size_t table_offset = 0;
    for (uint32_t i = 0; i < library->exports.count; ++i) {
      const iree_hal_executable_dispatch_attrs_v0_t *attrs =
          &library->exports.attrs[i];
      const char *name = library->exports.names ? library->exports.names[i] : NULL;
      g_queue_query_export_t record;
      record.flags = attrs->flags;
      record.constant_count = attrs->constant_count;
      record.binding_count = attrs->binding_count;
      record.workgroup_size[0] = attrs->workgroup_size_x;
      record.workgroup_size[1] = attrs->workgroup_size_y;
      record.workgroup_size[2] = attrs->workgroup_size_z;
      size_t name_bytes = name ? strlen(name) + 1 : 0;
      if (name_bytes > G_NAME_BYTES) return G_STATUS_UNSUPPORTED;
      record.export_name_bytes = name
          ? (uint32_t)((name_bytes + 3u) & ~(size_t)3u) : 0;
      memset(record.export_name, 0, sizeof(record.export_name));
      if (name_bytes) memcpy(record.export_name, name, name_bytes);
      size_t record_bytes = offsetof(g_queue_query_export_t, export_name) +
                            record.export_name_bytes;
      if (table_offset > request->table_bytes ||
          record_bytes > request->table_bytes - table_offset)
        return G_STATUS_BAD_SHAPE;
      g_copy_to_shared(table + table_offset, &record, record_bytes);
      table_offset += record_bytes;
    }
    return G_STATUS_OK;
  }
  return G_STATUS_UNSUPPORTED;
}

uint32_t g_dispatch_execute(const g_queue_request_t *r, g_queue_completion_t *c) {
  if (r->operation == G_QUEUE_QUERY) return query(r, c);
  if (r->operation == G_QUEUE_QUERY_BATCH) return query_batch(r, c);
  if (r->operation == G_OP_PING) return G_STATUS_OK;
  if (!g_queue_operation_is_dispatch(r->operation)) {
    printf("NPU unsupported queue operation=%u sequence=%u ordinal=%u\n",
           r->operation, r->sequence, r->ordinal);
    return G_STATUS_UNSUPPORTED;
  }
  const g_queue_dispatch_payload_t *wire =
      g_queue_dispatch_payload_const(r);
  if (!r->library_id || r->library_id > G_LIBRARY_COUNT) return G_STATUS_BAD_ABI;
  uint32_t index = r->library_id - 1;
  const iree_hal_executable_library_v0_t *l = libraries[index];
  if (r->ordinal >= l->exports.count || wire->constant_count > G_MAX_CONSTANTS ||
      wire->binding_count > G_MAX_BINDINGS ||
      wire->executable_constant_count != l->constants.count)
    return G_STATUS_BAD_ABI;
  const iree_hal_executable_dispatch_attrs_v0_t *a = &l->exports.attrs[r->ordinal];
  if (wire->constant_count < a->constant_count ||
      wire->binding_count != a->binding_count)
    return G_STATUS_BAD_ABI;
  size_t scratch_bytes = (size_t)a->local_memory_pages *
                         IREE_HAL_EXECUTABLE_WORKGROUP_LOCAL_MEMORY_PAGE_SIZE;
  if (scratch_bytes > sizeof(local_memory)) return G_STATUS_UNSUPPORTED;
  for (unsigned i = 0; i < 3; ++i)
    if (!wire->workgroup_size[i] || !wire->workgroup_count[i] ||
        wire->workgroup_id[i] >= wire->workgroup_count[i])
      return G_STATUS_BAD_SHAPE;
  if (wire->workgroup_size[2] > UINT16_MAX ||
      wire->workgroup_count[2] > UINT16_MAX ||
      wire->workgroup_id[2] > UINT16_MAX) return G_STATUS_BAD_SHAPE;
  const uint32_t *constants = g_queue_dispatch_constants_const(wire);
  const uint32_t *executable_constants =
      g_queue_dispatch_executable_constants_const(wire);
  const g_queue_binding_t *bindings = g_queue_dispatch_bindings_const(wire);
  void *pointers[G_MAX_BINDINGS];
  size_t lengths[G_MAX_BINDINGS];
  for (uint32_t i = 0; i < wire->binding_count; ++i) {
    /* Empty template bindings are legal when neither side dereferences them. */
    if (!bindings[i].address && !bindings[i].bytes) {
      pointers[i] = NULL;
      lengths[i] = 0;
      continue;
    }
    if (!g_range_in_dispatch(bindings[i].address, bindings[i].bytes))
      return G_STATUS_BAD_ADDRESS;
    pointers[i] = (void *)(uintptr_t)bindings[i].address;
    lengths[i] = bindings[i].bytes;
  }
  iree_hal_executable_environment_v0_t env = environments[index];
  env.constants = executable_constants;
  iree_hal_executable_dispatch_state_v0_t d = {
    .workgroup_size_x = wire->workgroup_size[0],
    .workgroup_size_y = wire->workgroup_size[1],
    .workgroup_size_z = wire->workgroup_size[2],
    .constant_count = wire->constant_count,
    .workgroup_count_x = wire->workgroup_count[0],
    .workgroup_count_y = wire->workgroup_count[1],
    .workgroup_count_z = wire->workgroup_count[2],
    .binding_count = wire->binding_count,
    .max_concurrency = 1, .constants = constants,
    .binding_ptrs = pointers, .binding_lengths = lengths,
  };
  iree_hal_executable_workgroup_state_v0_t w = {
    .workgroup_id_x = wire->workgroup_id[0],
    .workgroup_id_y = wire->workgroup_id[1],
    .workgroup_id_z = wire->workgroup_id[2], .processor_id = 0,
    .local_memory = local_memory, .local_memory_size = scratch_bytes,
  };
  if (r->operation == G_QUEUE_DISPATCH_BATCH) {
    if (wire->workgroup_id[0] || wire->workgroup_id[1] ||
        wire->workgroup_id[2]) return G_STATUS_BAD_SHAPE;
    for (uint32_t z = 0; z < wire->workgroup_count[2]; ++z) {
      w.workgroup_id_z = z;
      for (uint32_t y = 0; y < wire->workgroup_count[1]; ++y) {
        w.workgroup_id_y = y;
        for (uint32_t x = 0; x < wire->workgroup_count[0]; ++x) {
          w.workgroup_id_x = x;
          if (l->exports.ptrs[r->ordinal](&env, &d, &w))
            return G_STATUS_KERNEL_ERROR;
        }
      }
    }
    return G_STATUS_OK;
  }
  int result = l->exports.ptrs[r->ordinal](&env, &d, &w);
  return result ? G_STATUS_KERNEL_ERROR : G_STATUS_OK;
}
