// Run the MNIST CNN (compiled to a .vmfb) on bare metal and check the answer
// against ONNX Runtime.
//
// Same shape of program as ../src/main.c, with two differences:
//   * the I/O is float32 rather than int8, and the reference values come from
//     ONNX Runtime (embedded in cnn_io.h by the exporter), so "PASS" means
//     "agrees with the framework this model was trained/exported for";
//   * there is no printf("%f") on bare metal -- support/syscalls.c only
//     implements the integer conversions -- so diagnostics are printed as
//     scaled integers.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "iree/modules/hal/module.h"
#include "iree/vm/api.h"
#include "iree/vm/bytecode/module.h"

#include "cnn_vmfb.h"
#include "cnn_io.h"

// The float model and the int8 model differ only in what the function takes.
#ifndef CNN_INT8
#define CNN_INT8 0
#endif
#if CNN_INT8
typedef int8_t input_elem_t;
#define IREE_INPUT_ELEM_TYPE IREE_HAL_ELEMENT_TYPE_INT_8
#define TOLERANCE 2e-3f   // looser: quantization is lossy by construction
#else
typedef float input_elem_t;
#define IREE_INPUT_ELEM_TYPE IREE_HAL_ELEMENT_TYPE_FLOAT_32
#define TOLERANCE 2e-4f
#endif

// Defined in ../src/device_local_sync.c.
iree_status_t d_create_device(iree_allocator_t host_allocator,
                             iree_hal_device_t** out_device);

// Input is NHWC because the exporter rewrote the graph that way; the values
// are the same 1x1x28x28 image ONNX Runtime was given.
static const iree_hal_dim_t kInputShape[4] = {1, 28, 28, 1};

static float g_output[10];

static void report(const char* what, iree_status_t status) {
  iree_string_view_t message = iree_status_message(status);
  char buf[256];
  size_t n = message.size < sizeof(buf) - 1 ? message.size : sizeof(buf) - 1;
  memcpy(buf, message.data, n);
  buf[n] = '\0';
  printf("FAIL %s (code %d): %s\n", what, (int)iree_status_code(status), buf);
}

static iree_status_t make_input_view(iree_hal_device_t* device,
                                     iree_hal_buffer_view_t** out_view) {
  const iree_device_size_t byte_length = cnn_input_count * sizeof(input_elem_t);
  iree_hal_buffer_t* buffer = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_allocator_allocate_buffer(
      iree_hal_device_allocator(device),
      (iree_hal_buffer_params_t){
          .usage = IREE_HAL_BUFFER_USAGE_TRANSFER |
                   IREE_HAL_BUFFER_USAGE_DISPATCH_STORAGE |
                   IREE_HAL_BUFFER_USAGE_MAPPING,
          .type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL |
                  IREE_HAL_MEMORY_TYPE_HOST_VISIBLE,
      },
      byte_length, &buffer));
  iree_status_t status = iree_hal_buffer_map_write(buffer, 0, cnn_input,
                                                   byte_length);
  if (iree_status_is_ok(status)) {
    status = iree_hal_buffer_view_create(
        buffer, IREE_ARRAYSIZE(kInputShape), kInputShape,
        IREE_INPUT_ELEM_TYPE, IREE_HAL_ENCODING_TYPE_DENSE_ROW_MAJOR,
        iree_allocator_system(), out_view);
  }
  iree_hal_buffer_release(buffer);
  return status;
}

// On any error this returns immediately and leaks; see ../src/main.c for why
// that is an acceptable trade-off for a run-once program.
static iree_status_t run(iree_allocator_t host_allocator) {
  iree_vm_instance_t* instance = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_instance_create(
      IREE_VM_TYPE_CAPACITY_DEFAULT, host_allocator, &instance));
  IREE_RETURN_IF_ERROR(iree_hal_module_register_all_types(instance));

  iree_hal_device_t* device = NULL;
  IREE_RETURN_IF_ERROR(d_create_device(host_allocator, &device));

  iree_hal_device_group_t* device_group = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_device_group_create_from_device(
      device, host_allocator, &device_group));

  iree_vm_module_t* hal_module = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_module_create(
      instance, iree_hal_module_device_policy_default(), device_group,
      IREE_HAL_MODULE_FLAG_SYNCHRONOUS, iree_hal_module_debug_sink_null(),
      host_allocator, &hal_module));
  iree_hal_device_group_release(device_group);

  iree_vm_module_t* bytecode_module = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_bytecode_module_create(
      instance, IREE_VM_BYTECODE_MODULE_FLAG_NONE,
      iree_make_const_byte_span(cnn_vmfb_data, cnn_vmfb_size),
      iree_allocator_null(), host_allocator, &bytecode_module));

  iree_vm_context_t* context = NULL;
  iree_vm_module_t* modules[] = {hal_module, bytecode_module};
  IREE_RETURN_IF_ERROR(iree_vm_context_create_with_modules(
      instance, IREE_VM_CONTEXT_FLAG_NONE, IREE_ARRAYSIZE(modules), modules,
      host_allocator, &context));
  iree_vm_module_release(hal_module);
  iree_vm_module_release(bytecode_module);

  iree_vm_function_t main_function;
  IREE_RETURN_IF_ERROR(iree_vm_context_resolve_function(
      context, iree_make_cstring_view("module.main"), &main_function));

  iree_hal_buffer_view_t* arg0 = NULL;
  IREE_RETURN_IF_ERROR(make_input_view(device, &arg0));

  iree_vm_list_t* inputs = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_list_create(
      iree_vm_make_undefined_type_def(), 1, host_allocator, &inputs));
  iree_vm_ref_t arg0_ref = iree_hal_buffer_view_move_ref(arg0);
  IREE_RETURN_IF_ERROR(iree_vm_list_push_ref_move(inputs, &arg0_ref));

  iree_vm_list_t* outputs = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_list_create(
      iree_vm_make_undefined_type_def(), 1, host_allocator, &outputs));

  unsigned long t0 = 0, t1 = 0;
#if defined(__riscv) || defined(__riscv__)
  __asm__ volatile("rdcycle %0" : "=r"(t0));
#endif
  IREE_RETURN_IF_ERROR(iree_vm_invoke(
      context, main_function, IREE_VM_INVOCATION_FLAG_NONE,
      /*policy=*/NULL, inputs, outputs, host_allocator));
#if defined(__riscv) || defined(__riscv__)
  __asm__ volatile("rdcycle %0" : "=r"(t1));
#endif
  if (t1) printf("cycles: %lu\n", t1 - t0);

  iree_hal_buffer_view_t* ret = iree_vm_list_get_buffer_view_assign(outputs, 0);
  if (!ret) {
    IREE_RETURN_IF_ERROR(
        iree_make_status(IREE_STATUS_NOT_FOUND, "no return buffer view"));
  }
  IREE_RETURN_IF_ERROR(iree_hal_buffer_map_read(
      iree_hal_buffer_view_buffer(ret), 0, g_output, sizeof(g_output)));

  iree_vm_list_release(inputs);
  iree_vm_list_release(outputs);
  iree_vm_context_release(context);
  iree_hal_device_release(device);
  iree_vm_instance_release(instance);
  return iree_ok_status();
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  printf("cnn mnist: conv5x5(1->10) pool conv5x5(10->20) pool fc(320->50) fc(50->10)\n");
  printf("vmfb size: %d bytes\n", (int)cnn_vmfb_size);

  iree_status_t status = run(iree_allocator_system());
  if (!iree_status_is_ok(status)) {
    report("runtime", status);
    iree_status_free(status);
    return 1;
  }

  // Compare against ONNX Runtime element by element.
  float max_err = 0.0f;
  int bad = 0;
  for (size_t i = 0; i < cnn_output_count; ++i) {
    float d = g_output[i] - cnn_expected[i];
    if (d < 0) d = -d;
    if (d > max_err) max_err = d;
    if (d > TOLERANCE) ++bad;
  }

  int argmax = 0;
  for (int i = 1; i < 10; ++i) {
    if (g_output[i] > g_output[argmax]) argmax = i;
  }
  int ref_argmax = 0;
  for (int i = 1; i < 10; ++i) {
    if (cnn_expected[i] > cnn_expected[ref_argmax]) ref_argmax = i;
  }

  // No %f on bare metal: print the error scaled by 1e6.
  printf("max abs error: %d x1e-6\n", (int)(max_err * 1e6f));
  printf("argmax: iree=%d reference=%d\n", argmax, ref_argmax);

  if (bad || argmax != ref_argmax) {
    printf("iree:");
    for (size_t i = 0; i < cnn_output_count; ++i)
      printf(" %d", (int)(g_output[i] * 1000.0f));
    printf("\nref :");
    for (size_t i = 0; i < cnn_output_count; ++i)
      printf(" %d", (int)(cnn_expected[i] * 1000.0f));
    printf("\n");
    printf("FAIL: %d / %d outputs outside tolerance\n", bad,
           (int)cnn_output_count);
    return 1;
  }
#if CNN_INT8
  printf("PASS cnn mnist int8 (matches the numpy int8 reference)\n");
#else
  printf("PASS cnn mnist (matches onnxruntime)\n");
#endif
  return 0;
}
