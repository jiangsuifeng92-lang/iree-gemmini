#ifndef G_HOST_DIAGNOSTICS_H
#define G_HOST_DIAGNOSTICS_H
#include "device_abi.h"
static inline void g_putchar(char c) {
  *(volatile uint32_t *)(uintptr_t)G_HOST_CONSOLE = (uint8_t)c;
}
static inline void g_puts(const char *s) {
  while (*s) g_putchar(*s++);
}
static inline void g_put_u32(uint32_t value) {
  char digits[10]; unsigned count = 0;
  do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
  while (count) g_putchar(digits[--count]);
}
static inline void g_put_i32(int32_t value) {
  if (value < 0) { g_putchar('-'); g_put_u32(0u - (uint32_t)value); }
  else g_put_u32(value);
}
static inline void g_put_u64(uint64_t value) {
  char digits[20]; unsigned count = 0;
  do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
  while (count) g_putchar(digits[--count]);
}
static inline uint64_t g_host_read_counter(uint32_t low_addr,
                                           uint32_t high_addr) {
  volatile uint32_t *low = (volatile uint32_t *)(uintptr_t)low_addr;
  volatile uint32_t *high = (volatile uint32_t *)(uintptr_t)high_addr;
  uint32_t high_before, high_after, low_value;
  do {
    high_before = *high;
    low_value = *low;
    high_after = *high;
  } while (high_before != high_after);
  return ((uint64_t)high_before << 32) | low_value;
}
static inline void g_report_arm_profile(const char *marker) {
  uint64_t soc_cycles = g_host_read_counter(G_HOST_SOC_CYCLES_LO,
                                           G_HOST_SOC_CYCLES_HI);
  uint64_t active_cycles = g_host_read_counter(G_HOST_ACTIVE_CYCLES_LO,
                                               G_HOST_ACTIVE_CYCLES_HI);
  g_puts("[ARM PERF] marker="); g_puts(marker);
  g_puts(" soc_cycles="); g_put_u64(soc_cycles);
  g_puts(" active_cycles="); g_put_u64(active_cycles);
  g_putchar('\n');
}
#endif
