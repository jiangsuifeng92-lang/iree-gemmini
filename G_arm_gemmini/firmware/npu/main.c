#include "device_abi.h"
#ifdef G_ENABLE_IREE_DISPATCH
#include "dispatch.h"
#include <stdio.h>
static uint32_t dispatch_count;
static uint64_t dispatch_workgroup_count;
static uint32_t query_count;
static uint32_t query_export_count;
static uint64_t query_slot_cycles, query_handler_cycles;
static uint64_t dispatch_slot_cycles, dispatch_handler_cycles;
static uint64_t rocket_queue_ready_cycle;

static inline uint64_t g_read_cycle(void) {
  uint64_t value;
  __asm__ volatile("rdcycle %0" : "=r"(value) :: "memory");
  return value;
}

/* 在 Arm 完成所有同步请求后输出 Rocket 侧汇总计数与周期数。 */
static void report_profile(volatile g_queue_t *q) {
  printf("[ROCKET PERF] query_count=%u query_exports=%u slot_cycles=%llu handler_cycles=%llu\n",
      query_count, query_export_count, (unsigned long long)query_slot_cycles,
      (unsigned long long)query_handler_cycles);
  printf("[ROCKET PERF] dispatch_count=%u workgroups=%llu slot_cycles=%llu handler_cycles=%llu\n",
      dispatch_count, (unsigned long long)dispatch_workgroup_count,
      (unsigned long long)dispatch_slot_cycles,
      (unsigned long long)dispatch_handler_cycles);
  printf("[ROCKET PERF] cycles_since_queue_ready=%llu\n",
      (unsigned long long)(g_read_cycle() - rocket_queue_ready_cycle));
  printf("[GEMMINI PERF] calls=%u call_latency_cycles=%llu\n",
      g_dispatch_profile_gemmini_calls(),
      (unsigned long long)g_dispatch_profile_gemmini_cycles());
  g_barrier();
  q->reserved[G_QUEUE_PROFILE_ACK_INDEX] = G_QUEUE_PROFILE_ACK;
}

/*
 * Queue ABI v4 服务路径：取出 Arm 写入的一个请求槽，交给 dispatch ABI
 * 执行，再向完成环写回同一 sequence 和状态。一个 DISPATCH_BATCH 请求
 * 可以包含多个 workgroup；Rocket 在本次 g_dispatch_execute 中逐个运行，
 * 所以完成环仍只产生一条回执。
 */
static void drain_queue(void) {
  /* 访问共享 SRAM 中的 queue ABI v4 环形队列。 */
  volatile g_queue_t *q = (volatile g_queue_t *)(uintptr_t)G_QUEUE_BASE;
  /* Clear before checking tail: a later Arm submission cannot lose its wakeup.
   * Firmware polls; global interrupts remain disabled, with no WFI race. */
  *(volatile uint32_t *)(uintptr_t)G_DOORBELL_ADDR = 0;
  g_barrier();
  while (q->submit_head != q->submit_tail) {
    uint32_t head = q->submit_head, tail = q->complete_tail;
    if ((uint32_t)(tail - q->complete_head) >= G_QUEUE_CAPACITY) return;
    g_barrier();
    g_queue_request_t r;
    volatile g_queue_request_t *slot = &q->submit[head % G_QUEUE_CAPACITY];
    uint64_t slot_start = g_read_cycle();
    g_copy_from_shared(&r, slot, g_queue_request_bytes_volatile(slot));
    g_queue_completion_t c;
    c.sequence = r.sequence;
    c.status = G_STATUS_OK;
    c.export_name_bytes = 0;
    /* QUERY_BATCH 查库目录；DISPATCH_BATCH 执行请求携带的整个 workgroup 网格。 */
    uint64_t handler_start = g_read_cycle();
    c.status = g_dispatch_execute(&r, &c);
    uint64_t handler_cycles = g_read_cycle() - handler_start;
    if (g_queue_operation_is_dispatch(r.operation)) {
      ++dispatch_count;
      if (r.operation == G_QUEUE_DISPATCH_BATCH) {
        const g_queue_dispatch_payload_t *d =
            g_queue_dispatch_payload_const(&r);
        dispatch_workgroup_count += (uint64_t)d->workgroup_count[0] *
            d->workgroup_count[1] * d->workgroup_count[2];
      } else {
        ++dispatch_workgroup_count;
      }
      if (dispatch_count <= 3 || c.status)
        printf("NPU dispatch %u: library=%u ordinal=%u status=%u workgroups=%llu\n",
               dispatch_count, r.library_id, r.ordinal, c.status,
               (unsigned long long)dispatch_workgroup_count);
    }
    g_barrier();
    size_t completion_bytes = c.status
        ? g_queue_completion_fixed_bytes(r.operation)
        : g_queue_completion_bytes(r.operation, &c);
    /* 先写完整回执，再发布 complete_tail；Arm 由此知道新完成槽已就绪。 */
    g_copy_to_shared(&q->complete[tail % G_QUEUE_CAPACITY], &c,
                     completion_bytes);
    g_barrier();
    q->complete_tail = tail + 1;
    g_barrier();
    q->submit_head = head + 1;
    uint64_t slot_cycles = g_read_cycle() - slot_start;
    if (r.operation == G_QUEUE_QUERY ||
        r.operation == G_QUEUE_QUERY_BATCH) {
      ++query_count;
      query_export_count += r.operation == G_QUEUE_QUERY_BATCH
          ? c.export_count : 1u;
      query_handler_cycles += handler_cycles;
      query_slot_cycles += slot_cycles;
    } else if (g_queue_operation_is_dispatch(r.operation)) {
      dispatch_handler_cycles += handler_cycles;
      dispatch_slot_cycles += slot_cycles;
    }
  }
}
#endif

/* F 提供的 Gemmini C kernel；构建时链接进 Rocket 的 RISC-V 固件。 */
extern int gemmini_matmul_i8(const int8_t *, size_t, size_t,
                            const int8_t *, size_t, size_t,
                            int32_t *, size_t, size_t, size_t, size_t, size_t);

static uint32_t execute(const g_descriptor_t *d) {
  if (d->operation == G_OP_PING) return G_STATUS_OK;
  /*
   * 旧 mailbox ABI v1 的独立 matmul 示例。MNIST/IREE dispatch 走上方
   * queue ABI v4 和 g_dispatch_execute，不走这个旧 descriptor 分支。
   */
  if (d->operation != G_OP_MATMUL_I8) return G_STATUS_UNSUPPORTED;
  if (!d->m || d->m > 4096 || !d->n || d->n > 1024 ||
      !d->k || d->k > 1152) return G_STATUS_BAD_SHAPE;
  uint32_t a_bytes = d->m * d->k;
  uint32_t b_bytes = d->k * d->n;
  uint32_t c_bytes = d->m * d->n * 4;
  if (d->input_bytes < a_bytes || d->weight_bytes < b_bytes ||
      d->output_bytes < c_bytes || (d->output_addr & 3) ||
      !g_range_in_io(d->input_addr, a_bytes) ||
      !g_range_in_io(d->weight_addr, b_bytes) ||
      !g_range_in_io(d->output_addr, c_bytes)) return G_STATUS_BAD_ADDRESS;
  int rc = gemmini_matmul_i8(
      (const int8_t *)(uintptr_t)d->input_addr, 0, d->k,
      (const int8_t *)(uintptr_t)d->weight_addr, 0, d->n,
      (int32_t *)(uintptr_t)d->output_addr, 0, d->n, d->m, d->n, d->k);
  return rc == 0 ? G_STATUS_OK : G_STATUS_KERNEL_ERROR;
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  /* 初始化旧 mailbox ABI；保留给 ping / 早期 matmul transport 示例。 */
  volatile g_mailbox_t *mb = (volatile g_mailbox_t *)(uintptr_t)G_MAILBOX_BASE;
  volatile g_descriptor_t *shared =
      (volatile g_descriptor_t *)(uintptr_t)G_DESCRIPTOR_BASE;
  /* Cold boot only; warm reset/session recovery needs an epoch handshake. */
  mb->ready = 0;
  mb->magic = G_ABI_MAGIC;
  mb->abi_version = G_ABI_VERSION;
  mb->epoch = 1;
  mb->request_seq = mb->complete_seq = 0;
  mb->status = G_STATUS_OK;
  g_barrier();
  mb->ready = 1;
#ifdef G_ENABLE_IREE_DISPATCH
  /* 初始化提交环/完成环，并登记已静态链接到 npu.elf 的设备库。 */
  volatile g_queue_t *q = (volatile g_queue_t *)(uintptr_t)G_QUEUE_BASE;
  q->ready = 0;
  q->magic = G_ABI_MAGIC;
  q->abi_version = G_QUEUE_VERSION;
  q->epoch = 1;
  q->submit_head = q->submit_tail = q->complete_head = q->complete_tail = 0;
  for (unsigned i = 0; i < 8; ++i) q->reserved[i] = 0;
  uint32_t init_status = g_dispatch_initialize();
  if (init_status != G_STATUS_OK) {
    printf("NPU library initialization failed: %u\n", init_status);
    mb->status = G_STATUS_UNSUPPORTED;
    for (;;) {} /* Arm handshake times out and reports failure. */
  }
  g_barrier();
  q->ready = 1;
  rocket_queue_ready_cycle = g_read_cycle();
  printf("NPU ready: Rocket dispatch executor, queue ABI %u\n", G_QUEUE_VERSION);
#endif
  /*
   * Rocket 常驻服务循环：有队列请求就执行并回执；profile 标志置位后
   * 输出汇总。循环末尾的 mailbox 检查仅服务旧 ABI 请求。
   */
  for (;;) {
#ifdef G_ENABLE_IREE_DISPATCH
    if (*(volatile uint32_t *)(uintptr_t)G_DOORBELL_ADDR ||
        q->submit_head != q->submit_tail) drain_queue();
    if (q->reserved[G_QUEUE_PROFILE_DONE_INDEX] == G_QUEUE_PROFILE_DONE &&
        q->reserved[G_QUEUE_PROFILE_ACK_INDEX] != G_QUEUE_PROFILE_ACK)
      report_profile(q);
#endif
    uint32_t seq = mb->request_seq;
    if (seq == mb->complete_seq) continue;
    g_barrier();
    g_descriptor_t local;
    for (size_t i = 0; i < sizeof(local) / sizeof(uint32_t); ++i)
      ((uint32_t *)&local)[i] = ((volatile uint32_t *)shared)[i];
    mb->status = G_STATUS_BUSY;
    uint32_t status = execute(&local);
    g_barrier();
    mb->status = status;
    g_barrier();
    mb->complete_seq = seq;
  }
}
