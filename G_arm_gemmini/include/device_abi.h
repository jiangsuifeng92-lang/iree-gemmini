#ifndef G_ARM_GEMMINI_DEVICE_ABI_H
#define G_ARM_GEMMINI_DEVICE_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Physical addresses, little endian, no native pointers or size_t on the wire. */
#define G_ABI_MAGIC UINT32_C(0x474e5055)
#define G_ABI_VERSION UINT32_C(1)
#define G_MAILBOX_BASE UINT32_C(0x08000000)
#define G_DESCRIPTOR_BASE (G_MAILBOX_BASE + UINT32_C(0x100))
#define G_DDR_BASE UINT32_C(0x80000000)
#define G_DDR_SIZE UINT32_C(0x10000000)
#define G_HOST_DDR_BASE UINT32_C(0x88000000)
#define G_MODEL_BASE UINT32_C(0x89000000)
#define G_MODEL_SIZE UINT32_C(0x01000000)
#define G_IO_BASE UINT32_C(0x8a000000)
#define G_IO_SIZE UINT32_C(0x01000000)
#define G_HOST_REPORT UINT32_C(0x10000000) /* host-local simulation diagnostic */
#define G_HOST_CONSOLE UINT32_C(0x10000004) /* host-local simulation character sink */
#define G_HOST_SOC_CYCLES_LO UINT32_C(0x10000008)
#define G_HOST_SOC_CYCLES_HI UINT32_C(0x1000000c)
#define G_HOST_ACTIVE_CYCLES_LO UINT32_C(0x10000010)
#define G_HOST_ACTIVE_CYCLES_HI UINT32_C(0x10000014)
#define G_DEMO_A (G_IO_BASE + UINT32_C(0x0000))
#define G_DEMO_B (G_IO_BASE + UINT32_C(0x1000))
#define G_DEMO_C (G_IO_BASE + UINT32_C(0x2000))
#define G_DEMO_DIM UINT32_C(16)

enum g_operation {
  G_OP_PING = 1, G_OP_MATMUL_I8 = 2,
  G_OP_RUN_MODEL = 3, /* legacy reserved value, unsupported */
  G_OP_DISPATCH = 4   /* implemented through queue_abi.h, not v1 mailbox */
};
enum g_status {
  G_STATUS_OK = 0, G_STATUS_BUSY = 1, G_STATUS_BAD_ABI = 2,
  G_STATUS_BAD_ADDRESS = 3, G_STATUS_BAD_SHAPE = 4,
  G_STATUS_UNSUPPORTED = 5, G_STATUS_KERNEL_ERROR = 6
};

typedef struct {
  uint32_t magic, abi_version, ready, epoch;
  uint32_t request_seq, complete_seq, status, reserved[9];
} g_mailbox_t;

typedef struct {
  uint32_t operation, model_id, model_addr, model_bytes;
  uint32_t input_addr, input_bytes, weight_addr, weight_bytes;
  uint32_t output_addr, output_bytes, m, n, k;
  uint32_t reserved[19];
} g_descriptor_t;

#ifdef __cplusplus
#define G_STATIC_ASSERT static_assert
#else
#define G_STATIC_ASSERT _Static_assert
#endif
G_STATIC_ASSERT(sizeof(g_mailbox_t) == 64, "mailbox ABI size");
G_STATIC_ASSERT(sizeof(g_descriptor_t) == 128, "descriptor ABI size");
G_STATIC_ASSERT(offsetof(g_mailbox_t, request_seq) == 16, "publish offset");
#undef G_STATIC_ASSERT

static inline void g_barrier(void) {
#if defined(__riscv)
  __asm__ volatile("fence iorw, iorw" ::: "memory");
#elif defined(__arm__)
  /* ARMv4 core has no cache or store buffer; its bridge waits for AXI B/R.
     This compiler barrier is valid only for this platform configuration. */
  __asm__ volatile("" ::: "memory");
#else
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static inline int g_range_in_io(uint32_t address, uint32_t bytes) {
  return address >= G_IO_BASE && address < G_IO_BASE + G_IO_SIZE &&
         bytes <= G_IO_SIZE && address - G_IO_BASE <= G_IO_SIZE - bytes;
}

static inline int g_range_in_dispatch(uint32_t address, uint32_t bytes) {
  /* Static VMFB constants may be imported without copying by IREE. */
  return g_range_in_io(address, bytes) ||
      (address >= G_MODEL_BASE && address < G_MODEL_BASE + G_MODEL_SIZE &&
       bytes <= G_MODEL_SIZE && address - G_MODEL_BASE <= G_MODEL_SIZE - bytes);
}

#endif
