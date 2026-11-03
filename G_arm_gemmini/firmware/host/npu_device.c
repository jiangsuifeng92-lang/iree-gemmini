/* Arm HAL bridge. local-sync owns command buffers and semaphore signaling;
 * issue_call sends a single-workgroup request or batches a whole grid for Rocket.
 * There are deliberately no RISC-V code pointers or device native structs on Arm. */
#include "npu_device.h"
#include "queue_abi.h"
#include "transport.h"
#include "diagnostics.h"
#include "iree/hal/drivers/local_sync/sync_device.h"
#include "iree/hal/local/executable_loader.h"
#include "iree/hal/local/local_executable.h"
#include "iree/hal/local/executable_library_util.h"
#include <string.h>

static g_transport_t transport;
static uint32_t query_completed;
static uint32_t dispatch_completed;
static uint32_t dispatch_workgroups_completed;

uint32_t g_npu_query_count(void) { return query_completed; }
uint32_t g_npu_dispatch_count(void) { return dispatch_completed; }

void g_npu_profile_finish(void) {
  volatile g_queue_t *q = (volatile g_queue_t *)(uintptr_t)G_QUEUE_BASE;
  q->reserved[G_QUEUE_PROFILE_DONE_INDEX] = G_QUEUE_PROFILE_DONE;
  g_barrier();
  while (q->reserved[G_QUEUE_PROFILE_ACK_INDEX] != G_QUEUE_PROFILE_ACK)
    g_barrier();
}

static void report_query_progress(void) {
  g_puts("ARM NPU: QUERY completed=");
  g_put_u32(query_completed);
  g_putchar('\n');
}

static void report_query_start(uint32_t ordinal) {
  g_puts("ARM NPU: QUERY request ordinal=");
  g_put_u32(ordinal);
  g_putchar('\n');
}

static void report_dispatch_progress(void) {
  if (dispatch_completed != 1 && (dispatch_completed & 63u) != 0) return;
  g_puts("ARM NPU: DISPATCH requests=");
  g_put_u32(dispatch_completed);
  g_puts(" workgroups=");
  g_put_u32(dispatch_workgroups_completed);
  g_putchar('\n');
}

static iree_status_t rpc(g_queue_request_t *r, g_queue_completion_t *c) {
  uint32_t status = g_transport_call(&transport, r, c);
  if (!status) return iree_ok_status();
  return iree_make_status(status == G_QUEUE_TIMEOUT ? IREE_STATUS_DEADLINE_EXCEEDED
                                                  : IREE_STATUS_INTERNAL,
                          "Rocket request failed: %u", status);
}

typedef struct {
  iree_hal_local_executable_t base;
  uint32_t library_id, executable_constant_count;
  uint32_t executable_constants[G_MAX_CONSTANTS];
  /* Host-only reflection metadata; ptrs/imports are never populated. */
  iree_hal_executable_library_v0_t reflection;
  iree_hal_executable_dispatch_attrs_v0_t *attrs;
  const char **names;
  char *name_storage;
  uint32_t skip_workgroups;
} remote_executable_t;

static void executable_destroy(iree_hal_executable_t *base) {
  remote_executable_t *e = (remote_executable_t *)base;
  iree_allocator_t a = e->base.host_allocator;
  iree_hal_local_executable_deinitialize(&e->base);
  iree_allocator_free(a, e->name_storage);
  iree_allocator_free(a, e->names);
  iree_allocator_free(a, e->attrs);
  iree_allocator_free(a, e);
}
static iree_host_size_t export_count(iree_hal_executable_t *base) {
  return ((remote_executable_t *)base)->reflection.exports.count;
}
static iree_status_t export_info(iree_hal_executable_t *base,
    iree_hal_executable_export_ordinal_t ordinal,
    iree_hal_executable_export_info_t *info) {
  return iree_hal_executable_library_export_info(
      &((remote_executable_t *)base)->reflection, ordinal, info);
}
static iree_status_t export_parameters(iree_hal_executable_t *base,
    iree_hal_executable_export_ordinal_t ordinal, iree_host_size_t capacity,
    iree_hal_executable_export_parameter_t *parameters) {
  return iree_hal_executable_library_export_parameters(
      &((remote_executable_t *)base)->reflection, ordinal, capacity, parameters);
}
static iree_status_t lookup_export(iree_hal_executable_t *base,
    iree_string_view_t name, iree_hal_executable_export_ordinal_t *ordinal) {
  return iree_hal_executable_library_lookup_export_by_name(
      &((remote_executable_t *)base)->reflection, name, ordinal);
}
static iree_status_t issue_call(iree_hal_local_executable_t *base,
    iree_host_size_t ordinal,
    const iree_hal_executable_dispatch_state_v0_t *d,
    const iree_hal_executable_workgroup_state_v0_t *w, uint32_t worker_id) {
  (void)worker_id;
  remote_executable_t *e = (remote_executable_t *)base;
  if (ordinal >= e->reflection.exports.count ||
      d->binding_count > G_MAX_BINDINGS || d->constant_count > G_MAX_CONSTANTS)
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE);
  if (e->skip_workgroups) {
    --e->skip_workgroups;
    return iree_ok_status();
  }
  uint64_t workgroup_total = (uint64_t)d->workgroup_count_x *
      d->workgroup_count_y * d->workgroup_count_z;
  bool batch = workgroup_total > 1 &&
      workgroup_total <= G_MAX_BATCH_WORKGROUPS &&
      !w->workgroup_id_x && !w->workgroup_id_y && !w->workgroup_id_z;
  g_queue_request_t r;
  r.operation = batch ? G_QUEUE_DISPATCH_BATCH : G_OP_DISPATCH;
  r.sequence = 0;
  r.library_id = e->library_id;
  r.ordinal = ordinal;
  g_queue_dispatch_payload_t *wire = g_queue_dispatch_payload(&r);
  wire->workgroup_size[0] = d->workgroup_size_x;
  wire->workgroup_size[1] = d->workgroup_size_y;
  wire->workgroup_size[2] = d->workgroup_size_z;
  wire->workgroup_count[0] = d->workgroup_count_x;
  wire->workgroup_count[1] = d->workgroup_count_y;
  wire->workgroup_count[2] = d->workgroup_count_z;
  wire->workgroup_id[0] = w->workgroup_id_x;
  wire->workgroup_id[1] = w->workgroup_id_y;
  wire->workgroup_id[2] = w->workgroup_id_z;
  wire->constant_count = d->constant_count;
  wire->binding_count = d->binding_count;
  wire->executable_constant_count = e->executable_constant_count;
  if (d->constant_count)
    memcpy(g_queue_dispatch_constants(wire), d->constants,
           d->constant_count * sizeof(uint32_t));
  if (e->executable_constant_count)
    memcpy(g_queue_dispatch_executable_constants(wire), e->executable_constants,
           wire->executable_constant_count * sizeof(uint32_t));
  g_queue_binding_t *bindings = g_queue_dispatch_bindings(wire);
  for (uint32_t i = 0; i < d->binding_count; ++i) {
    uintptr_t address = (uintptr_t)d->binding_ptrs[i];
    size_t length = d->binding_lengths[i];
    /* IREE command-buffer templates may leave an unused binding empty. */
    if (!address && !length) {
      bindings[i].address = 0;
      bindings[i].bytes = 0;
      continue;
    }
    if (address > UINT32_MAX || length > UINT32_MAX ||
        !g_range_in_dispatch((uint32_t)address, (uint32_t)length))
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
          "dispatch ordinal=%u binding=%u address=0x%08x bytes=%u outside shared DDR",
          (uint32_t)ordinal, i, (uint32_t)address, (uint32_t)length);
    bindings[i].address = address;
    bindings[i].bytes = length;
  }
  g_queue_completion_t c;
  iree_status_t status = rpc(&r, &c);
  if (iree_status_is_ok(status)) {
    ++dispatch_completed;
    dispatch_workgroups_completed += batch ? (uint32_t)workgroup_total : 1u;
    if (batch) e->skip_workgroups = (uint32_t)workgroup_total - 1u;
    report_dispatch_progress();
  }
  return status;
}
static const iree_hal_local_executable_vtable_t executable_vtable = {
  .base = {.destroy = executable_destroy, .export_count = export_count,
           .export_info = export_info, .export_parameters = export_parameters,
           .lookup_export_by_name = lookup_export},
  .issue_call = issue_call,
};

typedef struct {
  iree_hal_executable_loader_t base;
  iree_allocator_t allocator;
} remote_loader_t;
static void loader_destroy(iree_hal_executable_loader_t *base) {
  remote_loader_t *l = (remote_loader_t *)base;
  iree_allocator_free(l->allocator, l);
}
static bool loader_support(iree_hal_executable_loader_t *base,
    iree_hal_executable_caching_mode_t mode, iree_string_view_t format) {
  (void)base; (void)mode;
  return iree_string_view_equal(format, iree_make_cstring_view("static"));
}
static iree_status_t loader_infer(iree_hal_executable_loader_t *base,
    iree_hal_executable_caching_mode_t mode, iree_const_byte_span_t data,
    iree_host_size_t capacity, char *format, iree_host_size_t *size) {
  (void)base; (void)mode;
  if (capacity < 7 || !data.data_length || data.data_length > G_NAME_BYTES ||
      data.data[data.data_length - 1] != 0)
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT);
  memcpy(format, "static", 7);
  *size = data.data_length;
  return iree_ok_status();
}
static iree_status_t loader_load(iree_hal_executable_loader_t *base,
    const iree_hal_executable_params_t *p, iree_host_size_t workers,
    iree_hal_executable_t **out) {
  (void)workers;
  *out = NULL;
  if (!loader_support(base, p->caching_mode, p->executable_format) ||
      !p->executable_data.data_length ||
      p->executable_data.data_length > G_NAME_BYTES ||
      p->executable_data.data[p->executable_data.data_length - 1] != 0 ||
      p->constant_count > G_MAX_CONSTANTS)
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT);
  g_queue_request_t r;
  r.operation = G_QUEUE_QUERY_BATCH;
  r.sequence = 0;
  r.library_id = 0;
  r.ordinal = 0;
  memset(r.payload, 0, G_NAME_BYTES);
  memcpy(r.payload, p->executable_data.data, p->executable_data.data_length);
  g_queue_query_batch_payload_t *batch_payload =
      (g_queue_query_batch_payload_t *)(r.payload + G_NAME_BYTES);
  batch_payload->table_address = G_QUERY_TABLE_BASE;
  batch_payload->table_bytes = G_QUERY_TABLE_BYTES;
  g_queue_completion_t c;
  report_query_start(0);
  IREE_RETURN_IF_ERROR(rpc(&r, &c));
  query_completed += c.export_count;
  report_query_progress();
  if (!c.export_count || c.export_count > G_QUERY_MAX_EXPORTS ||
      c.executable_constant_count != p->constant_count)
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION);
  iree_allocator_t a = ((remote_loader_t *)base)->allocator;
  remote_executable_t *e = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(a, sizeof(*e), (void **)&e));
  iree_hal_local_executable_initialize(&executable_vtable, a, &e->base);
  e->skip_workgroups = 0;
  e->library_id = c.library_id;
  e->executable_constant_count = p->constant_count;
  if (p->constant_count) memcpy(e->executable_constants, p->constants,
                               p->constant_count * sizeof(uint32_t));
  e->reflection.exports.count = c.export_count;
  iree_status_t status = iree_allocator_malloc_array(
      a, c.export_count, sizeof(*e->attrs), (void **)&e->attrs);
  if (iree_status_is_ok(status)) status = iree_allocator_malloc_array(
      a, c.export_count, sizeof(*e->names), (void **)&e->names);
  if (iree_status_is_ok(status)) status = iree_allocator_malloc_array(
      a, c.export_count, G_NAME_BYTES, (void **)&e->name_storage);
  e->reflection.exports.attrs = e->attrs;
  e->base.dispatch_attrs = e->attrs;
  size_t query_table_offset = 0;
  bool has_export_names = false;
  for (uint32_t i = 0; i < e->reflection.exports.count && iree_status_is_ok(status); ++i) {
    const volatile uint8_t *shared_exports =
        (const volatile uint8_t *)(uintptr_t)G_QUERY_TABLE_BASE;
    g_queue_query_export_t export_info;
    g_copy_from_shared(&export_info, shared_exports + query_table_offset,
                       offsetof(g_queue_query_export_t, export_name));
    if (export_info.constant_count > G_MAX_CONSTANTS ||
        export_info.binding_count > G_MAX_BINDINGS ||
        export_info.export_name_bytes > G_NAME_BYTES ||
        (export_info.export_name_bytes & 3u)) {
      status = iree_make_status(IREE_STATUS_OUT_OF_RANGE); break;
    }
    size_t record_bytes = offsetof(g_queue_query_export_t, export_name) +
                          export_info.export_name_bytes;
    if (query_table_offset > G_QUERY_TABLE_BYTES ||
        record_bytes > G_QUERY_TABLE_BYTES - query_table_offset) {
      status = iree_make_status(IREE_STATUS_OUT_OF_RANGE); break;
    }
    g_copy_from_shared(export_info.export_name,
        shared_exports + query_table_offset +
            offsetof(g_queue_query_export_t, export_name),
        export_info.export_name_bytes);
    if (export_info.export_name_bytes &&
        !memchr(export_info.export_name, 0, export_info.export_name_bytes)) {
      status = iree_make_status(IREE_STATUS_INVALID_ARGUMENT); break;
    }
    query_table_offset += record_bytes;
    e->attrs[i] = (iree_hal_executable_dispatch_attrs_v0_t){
      .flags = export_info.flags, .constant_count = export_info.constant_count,
      .binding_count = export_info.binding_count,
      .workgroup_size_x = export_info.workgroup_size[0],
      .workgroup_size_y = export_info.workgroup_size[1],
      .workgroup_size_z = export_info.workgroup_size[2],
      /* Local memory lives on Rocket; Arm allocates no workgroup scratch. */
      .local_memory_pages = 0, .parameter_count = 0,
    };
    if (export_info.export_name_bytes) {
      e->names[i] = e->name_storage + i * G_NAME_BYTES;
      memset((char *)e->names[i], 0, G_NAME_BYTES);
      memcpy((char *)e->names[i], export_info.export_name,
             export_info.export_name_bytes);
      has_export_names = true;
    } else {
      e->names[i] = NULL;
    }
  }
  e->reflection.exports.names = has_export_names ? e->names : NULL;
  if (!iree_status_is_ok(status)) { executable_destroy((iree_hal_executable_t *)e); return status; }
  *out = (iree_hal_executable_t *)e;
  return iree_ok_status();
}
static const iree_hal_executable_loader_vtable_t loader_vtable = {
  .destroy = loader_destroy, .infer_format = loader_infer,
  .query_support = loader_support, .try_load = loader_load,
};

/* Shared buffer storage with 64-byte alignment and first-fit reclamation.
 * Only the single Arm thread manipulates this allocator. Metadata stays in
 * host-private DDR; all tensor data, including staging, stays in shared DDR. */
typedef struct { uint32_t span, used; uint32_t reserved[14]; } io_block_t;
#define G_IO_ALLOC_SIZE (G_IO_SIZE - G_QUERY_TABLE_BYTES)
static int io_initialized;
static void *io_allocate(uint32_t bytes) {
  if (bytes > G_IO_ALLOC_SIZE - 128) return NULL;
  uint32_t need = ((bytes ? bytes : 1) + 63u) & ~63u;
  if (!io_initialized) {
    *(io_block_t *)(uintptr_t)G_IO_BASE = (io_block_t){.span = G_IO_ALLOC_SIZE};
    io_initialized = 1;
  }
  for (uint32_t offset = 0; offset < G_IO_ALLOC_SIZE;) {
    io_block_t *b = (io_block_t *)(uintptr_t)(G_IO_BASE + offset);
    if (!b->used && b->span >= need + sizeof(*b)) {
      uint32_t consumed = need + sizeof(*b);
      if (b->span >= consumed + 128) {
        io_block_t *next = (io_block_t *)((uint8_t *)b + consumed);
        *next = (io_block_t){.span = b->span - consumed};
        b->span = consumed;
      }
      b->used = 1;
      return b + 1;
    }
    offset += b->span;
  }
  return NULL;
}
static void io_free(void *ptr) {
  if (!ptr) return;
  ((io_block_t *)ptr - 1)->used = 0;
  for (uint32_t offset = 0; offset < G_IO_ALLOC_SIZE;) {
    io_block_t *b = (io_block_t *)(uintptr_t)(G_IO_BASE + offset);
    while (!b->used && offset + b->span < G_IO_ALLOC_SIZE) {
      io_block_t *next = (io_block_t *)((uint8_t *)b + b->span);
      if (next->used) break;
      b->span += next->span;
    }
    offset += b->span;
  }
}
static iree_status_t io_ctl(void *self, iree_allocator_command_t command,
                           const void *params, void **ptr) {
  (void)self;
  /* After a timeout retain outstanding allocations until system reset. */
  if (transport.poisoned) return iree_make_status(IREE_STATUS_ABORTED);
  if (command == IREE_ALLOCATOR_COMMAND_FREE) { io_free(*ptr); *ptr = NULL; return iree_ok_status(); }
  const iree_allocator_alloc_params_t *p = params;
  if (command != IREE_ALLOCATOR_COMMAND_MALLOC &&
      command != IREE_ALLOCATOR_COMMAND_CALLOC &&
      command != IREE_ALLOCATOR_COMMAND_REALLOC)
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED);
  if (p->byte_length > UINT32_MAX) return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED);
  void *new_ptr = io_allocate(p->byte_length);
  if (!new_ptr) return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED);
  if (command == IREE_ALLOCATOR_COMMAND_CALLOC) memset(new_ptr, 0, p->byte_length);
  if (command == IREE_ALLOCATOR_COMMAND_REALLOC && *ptr) {
    size_t old_size = ((io_block_t *)*ptr - 1)->span - sizeof(io_block_t);
    memcpy(new_ptr, *ptr, old_size < p->byte_length ? old_size : p->byte_length);
    io_free(*ptr);
  }
  *ptr = new_ptr;
  return iree_ok_status();
}

iree_status_t g_npu_device_create(iree_allocator_t a, iree_hal_device_t **out) {
  *out = NULL;
  /* One device per firmware instance; queue is owned by this Arm thread. */
  uint32_t rc = g_transport_open(&transport);
  if (rc) return iree_make_status(IREE_STATUS_UNAVAILABLE, "NPU handshake: %u", rc);
  g_puts("ARM NPU: allocate loader\n");
  remote_loader_t *l = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(a, sizeof(*l), (void **)&l));
  l->allocator = a;
  iree_hal_executable_import_provider_t imports = {0};
  iree_hal_executable_loader_initialize(&loader_vtable, imports, &l->base);
  iree_hal_allocator_t *buffers = NULL;
  iree_allocator_t data = {.self = NULL, .ctl = io_ctl};
  /* F's current LocalDevice target matches "local*" in VMFB initialization.
   * Keep that prefix while replacing its executable loader with remote RPC. */
  iree_string_view_t identifier = iree_make_cstring_view("local-arm-gemmini");
  iree_status_t status = iree_hal_allocator_create_heap(identifier, data, a, &buffers);
  g_puts("ARM NPU: buffer allocator initialized\n");
  iree_hal_sync_device_params_t params;
  iree_hal_sync_device_params_initialize(&params);
  iree_hal_executable_loader_t *loader = &l->base;
  if (iree_status_is_ok(status)) status = iree_hal_sync_device_create(
      identifier, &params, 1, &loader, buffers, a, out);
  g_puts("ARM NPU: HAL device initialized\n");
  iree_hal_allocator_release(buffers);
  iree_hal_executable_loader_release(loader);
  return status;
}
