#include "npu_device.h"
#include "device_abi.h"
#include "diagnostics.h"
#include "iree/base/api.h"
#include "iree/modules/hal/module.h"
#include "iree/vm/api.h"
#include "iree/vm/bytecode/module.h"
#include "model_vmfb.h"
#include <string.h>
#ifdef G_MODEL_MNIST
#include "cnn_io.h"
#endif

/*
 * Arm 端应用入口：这里用 IREE VM API 驱动模型流程。
 * IREE runtime 的通用实现来自单独链接的 runtime 库；模型程序本身
 * 是嵌入的 VMFB；与 Rocket / Gemmini 的硬件交互由 npu_device.c 实现。
 */
/* Inspect these symbols in the Arm ELF/DDR for application diagnostics. */
volatile uint32_t g_host_status;
volatile uint32_t g_host_stage;
uint32_t g_host_output[16];

/* 将 IREE 格式化的错误状态逐字节送到本项目的 UART 输出。 */
static bool status_output(iree_string_view_t chunk, void *user_data) {
  (void)user_data;
  for (size_t i = 0; i < chunk.size; ++i) g_putchar(chunk.data[i]);
  return true;
}

#ifdef G_HAL_MAPPING_DIAGNOSTIC
/* 可选的 HAL buffer 映射自检；普通 MNIST 推理构建不会走这条分支。 */
static void report_mapping(const char *name,
                           const iree_hal_buffer_mapping_t *mapping) {
  g_puts("HAL MAP DIAG "); g_puts(name);
  g_puts(" data=");
  g_put_u32((uint32_t)(uintptr_t)mapping->contents.data);
  g_puts(" length="); g_put_u32((uint32_t)mapping->contents.data_length);
  g_putchar('\n');
}

static iree_status_t run_mapping_diagnostic(iree_hal_device_t *device) {
  const iree_hal_dim_t shape[] = {1, 28, 28, 1};
  iree_hal_buffer_t *buffer = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_allocator_allocate_buffer(
      iree_hal_device_allocator(device), (iree_hal_buffer_params_t){
        .usage = IREE_HAL_BUFFER_USAGE_TRANSFER |
                 IREE_HAL_BUFFER_USAGE_DISPATCH_STORAGE |
                 IREE_HAL_BUFFER_USAGE_MAPPING |
                 IREE_HAL_BUFFER_USAGE_MAPPING_PERSISTENT,
        .type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL |
                IREE_HAL_MEMORY_TYPE_HOST_VISIBLE},
      784, &buffer));

  iree_status_t status = iree_ok_status();
  iree_hal_buffer_mapping_t mapping = {{0}};
  status = iree_hal_buffer_map_range(buffer, IREE_HAL_MAPPING_MODE_SCOPED,
      IREE_HAL_MEMORY_ACCESS_WRITE, 0, 784, &mapping);
  if (iree_status_is_ok(status)) {
    report_mapping("scoped", &mapping);
    memset(mapping.contents.data, 0x5a, 784);
    status = iree_hal_buffer_unmap_range(&mapping);
  }

  iree_hal_buffer_view_t *view = NULL;
  if (iree_status_is_ok(status)) {
    status = iree_hal_buffer_view_create(buffer, IREE_ARRAYSIZE(shape), shape,
        IREE_HAL_ELEMENT_TYPE_INT_8, IREE_HAL_ENCODING_TYPE_DENSE_ROW_MAJOR,
        iree_allocator_system(), &view);
  }
  iree_hal_buffer_release(buffer);
  buffer = NULL;

  if (iree_status_is_ok(status)) {
    buffer = iree_hal_buffer_view_buffer(view);
    status = iree_hal_buffer_map_range(buffer, IREE_HAL_MAPPING_MODE_PERSISTENT,
        IREE_HAL_MEMORY_ACCESS_READ, 0, 784, &mapping);
    if (iree_status_is_ok(status)) {
      report_mapping("persistent", &mapping);
      if ((uint32_t)(uintptr_t)mapping.contents.data < G_IO_BASE ||
          (uint32_t)(uintptr_t)mapping.contents.data >= G_IO_BASE + G_IO_SIZE ||
          mapping.contents.data_length != 784) {
        status = iree_make_status(IREE_STATUS_DATA_LOSS,
            "HAL mapping diagnostic returned an invalid span");
      }
      iree_status_t unmap_status = iree_hal_buffer_unmap_range(&mapping);
      status = iree_status_join(status, unmap_status);
    }
  }

  if (buffer) iree_hal_buffer_release(buffer);
  if (view) iree_hal_buffer_view_release(view);
  if (iree_status_is_ok(status)) g_puts("HAL MAP DIAG PASS\n");
  return status;
}
#endif

/*
 * 为 IREE 输入创建 HAL buffer/view，并把样本复制到 host-visible 的共享 DDR。
 * MNIST 构建使用 1x28x28x1 的 int8 图像；matmul 演示使用 64x64 数据。
 */
static iree_status_t make_input(iree_hal_device_t *device, unsigned argument,
                               iree_hal_buffer_view_t **out) {
#ifdef G_MODEL_MNIST
  (void)argument;
  const iree_hal_dim_t shape[] = {1, 28, 28, 1};
  const size_t bytes = cnn_input_count;
#else
  const iree_hal_dim_t shape[] = {64, 64};
  const size_t bytes = 64 * 64;
#endif
  iree_hal_buffer_t *buffer = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_allocator_allocate_buffer(
      iree_hal_device_allocator(device), (iree_hal_buffer_params_t){
        .usage = IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_DISPATCH_STORAGE |
                 IREE_HAL_BUFFER_USAGE_MAPPING,
        .type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL | IREE_HAL_MEMORY_TYPE_HOST_VISIBLE},
      bytes, &buffer));
  iree_hal_buffer_mapping_t mapping;
  iree_status_t status = iree_hal_buffer_map_range(buffer, IREE_HAL_MAPPING_MODE_SCOPED,
      IREE_HAL_MEMORY_ACCESS_WRITE, 0, bytes, &mapping);
  if (iree_status_is_ok(status)) {
#ifdef G_MODEL_MNIST
    memcpy(mapping.contents.data, cnn_input, bytes);
#else
    for (unsigned i = 0; i < bytes; ++i)
      mapping.contents.data[i] = argument ? (i / 64 == i % 64) : (int8_t)((i % 16) - 8);
#endif
    status = iree_hal_buffer_unmap_range(&mapping);
  }
  if (iree_status_is_ok(status)) status = iree_hal_buffer_view_create(
      buffer, IREE_ARRAYSIZE(shape), shape, IREE_HAL_ELEMENT_TYPE_INT_8,
      IREE_HAL_ENCODING_TYPE_DENSE_ROW_MAJOR, iree_allocator_system(), out);
  iree_hal_buffer_release(buffer);
  return status;
}

/*
 * IREE Runtime 的应用层执行顺序都由本函数发起：准备 VM 与 HAL、载入
 * VMFB、建 context、调用 module.main、再读回结果。这里是 Arm C 程序，
 * 不是由 IREE 编译器生成的模型代码。
 */
static iree_status_t run_model(void) {
  /* stage 1：建立 IREE VM 的基础运行环境，并登记 HAL 相关类型。 */
  g_host_stage = 1;
  g_puts("ARM IREE: initialize VM\n");
  g_report_arm_profile("arm_iree_start");
  iree_allocator_t a = iree_allocator_system();
  iree_vm_instance_t *instance = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_instance_create(IREE_VM_TYPE_CAPACITY_DEFAULT, a, &instance));
  IREE_RETURN_IF_ERROR(iree_hal_module_register_all_types(instance));
  /* stage 2：创建 G 的 HAL device；它等待 Rocket 队列就绪并接通共享内存协议。 */
  g_host_stage = 2;
  g_puts("ARM IREE: connect NPU\n");
  iree_hal_device_t *device = NULL;
  IREE_RETURN_IF_ERROR(g_npu_device_create(a, &device));
#ifdef G_HAL_MAPPING_DIAGNOSTIC
  g_puts("ARM IREE: run HAL mapping diagnostic\n");
  iree_status_t diagnostic_status = run_mapping_diagnostic(device);
  iree_hal_device_release(device);
  return diagnostic_status;
#endif
  /*
   * 把 HAL device 放入 IREE device group，再建立 VM 可调用的 HAL module。
   * VM bytecode 通过这个 module 使用 buffer、executable 和 command buffer API。
   */
  iree_hal_device_group_t *group = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_device_group_create_from_device(device, a, &group));
  iree_vm_module_t *hal = NULL, *model = NULL;
  g_puts("ARM IREE: create HAL module\n");
  IREE_RETURN_IF_ERROR(iree_hal_module_create(instance,
      iree_hal_module_device_policy_default(), group, IREE_HAL_MODULE_FLAG_SYNCHRONOUS,
      iree_hal_module_debug_sink_null(), a, &hal));
  iree_hal_device_group_release(group);
  /*
   * VMFB 字节已经由 prepare_model.py 生成 C 数组并链接进 host.elf。
   * 这里创建 VM bytecode module；只是在解析模型程序，不是在此处做推理。
   */
  g_puts("ARM IREE: load model VMFB bytes=");
  g_put_u32((uint32_t)model_vmfb_size);
  g_putchar('\n');
  g_report_arm_profile("vmfb_load_start");
  IREE_RETURN_IF_ERROR(iree_vm_bytecode_module_create(instance,
      IREE_VM_BYTECODE_MODULE_FLAG_NONE,
      iree_make_const_byte_span(model_vmfb_data, model_vmfb_size),
      iree_allocator_null(), a, &model));
  g_puts("ARM IREE: model VMFB loaded\n");
  g_report_arm_profile("vmfb_loaded");
  iree_vm_module_t *modules[] = {hal, model};
  /*
   * 将 HAL module 与模型 module 链成 VM context。static executable loader
   * 在这一步向 Rocket 发一条 QUERY_BATCH：查询库名并指定共享 DDR 表地址。
   * Rocket 返回 library_id，并把 108 条 export 反射记录写进该表。注意，
   * g_npu_query_count() 累加的是返回的 export 条目数，所以显示 108；
   * 队列里实际只有这一条查询请求。模型执行时的 DISPATCH 请求另行发送。
   */
  g_host_stage = 3;
  g_puts("ARM IREE: initialize model context\n");
  g_report_arm_profile("context_init_start");
  iree_vm_context_t *context = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_context_create_with_modules(instance,
      IREE_VM_CONTEXT_FLAG_NONE, IREE_ARRAYSIZE(modules), modules, a, &context));
  g_puts("ARM NPU: QUERY phase complete count=");
  g_put_u32(g_npu_query_count());
  g_putchar('\n');
  g_report_arm_profile("query_complete");
  /* context 已保留模块引用；释放创建时的临时引用。 */
  iree_vm_module_release(hal);
  iree_vm_module_release(model);

  /* 找到 VMFB 暴露的模型入口 module.main，并准备它的输入/输出列表。 */
  iree_vm_function_t function;
  IREE_RETURN_IF_ERROR(iree_vm_context_resolve_function(
      context, iree_make_cstring_view("module.main"), &function));
  iree_vm_list_t *inputs = NULL, *outputs = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_list_create(iree_vm_make_undefined_type_def(), 2, a, &inputs));
  IREE_RETURN_IF_ERROR(iree_vm_list_create(iree_vm_make_undefined_type_def(), 1, a, &outputs));
#ifdef G_MODEL_MNIST
  const unsigned input_count = 1;
#else
  const unsigned input_count = 2;
#endif
  /* 为入口参数构造 IREE buffer view；底层 tensor 数据放在共享 DDR。 */
  for (unsigned i = 0; i < input_count; ++i) {
    iree_hal_buffer_view_t *view = NULL;
    IREE_RETURN_IF_ERROR(make_input(device, i, &view));
    iree_vm_ref_t ref = iree_hal_buffer_view_move_ref(view);
    IREE_RETURN_IF_ERROR(iree_vm_list_push_ref_move(inputs, &ref));
  }
  /*
   * stage 4：同步执行 VMFB 的 module.main。VM 在 Arm 上管理模型流程；
   * 遇到设备计算时，经 HAL executable 的 issue_call 把 dispatch 交给 Rocket。
   */
  g_host_stage = 4;
  g_puts("ARM IREE: invoke module.main\n");
  g_report_arm_profile("module_main_start");
  /* VMFB 的模型流程由 Arm 上的 IREE VM 执行；设备计算经 HAL 交给 Rocket。 */
  IREE_RETURN_IF_ERROR(iree_vm_invoke(context, function, IREE_VM_INVOCATION_FLAG_NONE,
                                    NULL, inputs, outputs, a));
  /* stage 5：模型返回后，从结果 buffer 读回 Arm 可见的输出区域。 */
  g_host_stage = 5;
  g_puts("ARM IREE: read output\n");
  g_report_arm_profile("output_read_start");
  iree_hal_buffer_view_t *result = iree_vm_list_get_buffer_view_assign(outputs, 0);
  if (!result) return iree_make_status(IREE_STATUS_INTERNAL);
  size_t size = iree_hal_buffer_view_byte_length(result);
  if (size > sizeof(g_host_output)) size = sizeof(g_host_output);
  IREE_RETURN_IF_ERROR(iree_hal_buffer_map_read(iree_hal_buffer_view_buffer(result),
                                              0, g_host_output, size));
  /* 推理结束，释放 VM 列表、context、HAL device 和 VM instance。 */
  iree_vm_list_release(outputs);
  iree_vm_list_release(inputs);
  iree_vm_context_release(context);
  iree_hal_device_release(device);
  iree_vm_instance_release(instance);
  return iree_ok_status();
}
#if defined(G_MODEL_MNIST) && !defined(G_HAL_MAPPING_DIAGNOSTIC)
/* 在 Arm 上把 10 个输出与导出阶段生成的参考分数逐项比较。 */
static int check_mnist(void) {
  float output[10];
  memcpy(output, g_host_output, sizeof(output));
  float max_error = 0;
  unsigned predicted = 0, expected = 0;
  for (unsigned i = 0; i < 10; ++i) {
    if (output[i] > output[predicted]) predicted = i;
    if (cnn_expected[i] > cnn_expected[expected]) expected = i;
    float error = output[i] - cnn_expected[i];
    if (error < 0) error = -error;
    /* 同时拒绝 NaN 和超过参考误差范围的分数。 */
    if (!(error <= 0.002f)) return 0;
    if (error > max_error) max_error = error;
    g_puts("MNIST score["); g_put_u32(i); g_puts("] x1e6=");
    g_put_i32((int32_t)(output[i] * 1000000.0f)); g_putchar('\n');
  }
  g_puts("MNIST predicted="); g_put_u32(predicted);
  g_puts(" expected="); g_put_u32(expected);
  g_puts(" max_error_x1e6="); g_put_u32((uint32_t)(max_error * 1000000.0f));
  g_putchar('\n');
  return predicted == expected;
}
#endif
/*
 * start.S 清零 BSS 后调用 host_main；这是 Arm ELF 的 C 入口。
 * 本函数负责 IREE 执行后的 PASS/FAIL、诊断状态和仿真报告寄存器。
 */
int host_main(void) {
  /* Failure is terminal. In particular, do not reuse outstanding DMA buffers
   * after a timeout. A new session requires both cores to cold reset. */
  iree_status_t status = run_model();
  g_host_status = iree_status_code(status);
  if (g_host_status) {
    g_puts("ARM IREE FAIL stage="); g_put_u32(g_host_stage);
    g_puts(" status="); g_put_u32(g_host_status); g_puts(" message=");
    iree_string_view_t message = iree_status_message(status);
    for (size_t i = 0; i < message.size; ++i) g_putchar(message.data[i]);
    g_putchar('\n');
    g_puts("ARM NPU counts: QUERY="); g_put_u32(g_npu_query_count());
    g_puts(" DISPATCH="); g_put_u32(g_npu_dispatch_count());
    g_putchar('\n');
    iree_status_format_to(status, status_output, NULL);
    g_putchar('\n');
  }
  iree_status_ignore(status);
#if defined(G_MODEL_MNIST) && !defined(G_HAL_MAPPING_DIAGNOSTIC)
  if (!g_host_status && !check_mnist()) {
    g_host_status = IREE_STATUS_DATA_LOSS;
    g_puts("MNIST FAIL: output differs from reference\n");
  }
  if (!g_host_status) {
    g_npu_profile_finish();
    g_puts("MNIST PASS (Arm IREE / Rocket / Gemmini)\n");
    g_report_arm_profile("mnist_pass");
  }
#endif
  *(volatile uint32_t *)(uintptr_t)G_HOST_REPORT = g_host_status ? 2 : 1;
  return g_host_status != 0;
}
