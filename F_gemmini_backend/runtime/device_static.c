// local-sync device with the static library loader, plus an import provider.
//
// Same as the step-D device except for one thing: the
// generated library carries an import table.
//
// Why it does is a property of IREE's executable linking. With
// --gemmini-link-executables=true (required, because each executable would
// otherwise overwrite the same .o path) LLVMCPU merges the per-dispatch
// modules and turns the call to `gemmini_matmul_i8` into a *dynamic* import
// rather than a link-time symbol reference. The resulting library therefore
// reports one required import, and a null import provider fails at load with
// "no import provider registered for resolving required executable imports".
//
// The import's signature is the bare-pointer ABI the lowering pass emitted --
// (i8*, i64, i64, i8*, i64, i64, i32*, i64, i64, i64, i64, i64) -- which is
// exactly the C function in kernels/gemmini_matmul.c. So the provider just
// hands back that pointer.

#include <stddef.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "iree/hal/drivers/local_sync/sync_device.h"
#include "iree/hal/local/executable_loader.h"
#include "iree/hal/local/loaders/static_library_loader.h"

#include "cnn_lib.h"  // cnn_linked_library_query()

// Defined in kernels/gemmini_matmul.c.
extern int gemmini_matmul_i8(const int8_t*, size_t, size_t, const int8_t*,
                             size_t, size_t, int32_t*, size_t, size_t, size_t,
                             size_t, size_t);

extern int gemmini_conv2d_i8(const int8_t*, size_t, size_t, size_t,
                             const int8_t*, size_t, size_t, int32_t*, size_t,
                             size_t, size_t, size_t, size_t, size_t, size_t,
                             size_t, size_t, size_t, size_t);

static iree_status_t gemmini_import_resolve(
    void* self, iree_host_size_t count, const char* const* symbol_names,
    void** out_fn_ptrs, void** out_fn_contexts,
    iree_hal_executable_import_resolution_t* out_resolution) {
  (void)self;
  // out_resolution is optional: the runtime passes NULL here and the
  // reference implementation guards the store the same way.
  if (out_resolution) *out_resolution = 0;
  for (iree_host_size_t i = 0; i < count; ++i) {
    if (strcmp(symbol_names[i], "gemmini_matmul_i8") == 0) {
      out_fn_ptrs[i] = (void*)(uintptr_t)&gemmini_matmul_i8;
      out_fn_contexts[i] = NULL;
    } else if (strcmp(symbol_names[i], "gemmini_conv2d_i8") == 0) {
      out_fn_ptrs[i] = (void*)(uintptr_t)&gemmini_conv2d_i8;
      out_fn_contexts[i] = NULL;
    } else {
      return iree_make_status(IREE_STATUS_NOT_FOUND,
                              "gemmini: unresolved executable import '%s'",
                              symbol_names[i]);
    }
  }
  return iree_ok_status();
}

iree_status_t d_create_device(iree_allocator_t host_allocator,
                              iree_hal_device_t** out_device) {
  iree_hal_sync_device_params_t params;
  iree_hal_sync_device_params_initialize(&params);

  const iree_hal_executable_library_query_fn_t libraries[] = {
      cnn_linked_library_query,
  };

  iree_hal_executable_import_provider_t import_provider = {
      /*self=*/NULL,
      /*resolve=*/gemmini_import_resolve,
  };

  iree_hal_executable_loader_t* loader = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_static_library_loader_create(
      IREE_ARRAYSIZE(libraries), libraries, import_provider, host_allocator,
      &loader));

  iree_string_view_t identifier = iree_make_cstring_view("local-sync");

  iree_hal_allocator_t* device_allocator = NULL;
  iree_status_t status = iree_hal_allocator_create_heap(
      identifier, host_allocator, host_allocator, &device_allocator);

  if (iree_status_is_ok(status)) {
    status = iree_hal_sync_device_create(identifier, &params,
                                         /*loader_count=*/1, &loader,
                                         device_allocator, host_allocator,
                                         out_device);
  }

  iree_hal_allocator_release(device_allocator);
  iree_hal_executable_loader_release(loader);
  return status;
}
