// malloc/free for bare metal.
//
// IREE's runtime is configured with IREE_ALLOCATOR_SYSTEM=libc, so every
// allocation inside the runtime ends up calling malloc/calloc/realloc/free from
// the C library. On Linux those come from glibc. On a bare-metal target there
// is no libc to provide them (we link -nostdlib), so this file does.
//
// It is a bump allocator: allocations move a cursor forward and free() does
// nothing. That is deliberate.
//
//   * The workload is "start up, run the model, exit". There is no long-lived
//     process to leak in, and the runtime's own arena allocations are counted
//     in tens of KiB.
//   * A real allocator would be ~300 lines of free-list code, and getting it
//     wrong corrupts the heap in ways that look like compiler bugs.
//
// If a later step needs many repeated invocations (e.g. token-by-token decode
// in a loop), replace this with a proper allocator or reset the cursor between
// runs via baremetal_heap_reset().
//
// Each block carries a 16-byte header so that realloc() knows how much to copy
// and every returned pointer stays 16-byte aligned (IREE asks for aligned
// storage in a few places).

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef BAREMETAL_HEAP_SIZE
#define BAREMETAL_HEAP_SIZE (64u * 1024u * 1024u)
#endif

static uint8_t g_heap[BAREMETAL_HEAP_SIZE] __attribute__((aligned(16)));
static size_t g_heap_used = 0;

typedef struct {
  size_t size;     // usable bytes following the header
  size_t reserved; // keeps the next block 16-byte aligned
} block_header_t;

#define HEADER_SIZE (sizeof(block_header_t))

void* malloc(size_t size) {
  if (size == 0) size = 1;
  // Round up to 16 bytes so the header plus payload stays aligned.
  size_t aligned = (size + 15u) & ~(size_t)15u;
  if (g_heap_used + HEADER_SIZE + aligned > BAREMETAL_HEAP_SIZE) {
    return NULL;
  }
  block_header_t* header = (block_header_t*)(g_heap + g_heap_used);
  header->size = size;
  header->reserved = 0;
  void* payload = g_heap + g_heap_used + HEADER_SIZE;
  g_heap_used += HEADER_SIZE + aligned;
  return payload;
}

void free(void* ptr) {
  // Bump allocator: nothing to reclaim. Kept as a real function because IREE
  // calls it and because a future replacement will need the hook.
  (void)ptr;
}

void* calloc(size_t nmemb, size_t size) {
  size_t total = nmemb * size;
  if (size != 0 && total / size != nmemb) return NULL; // overflow
  void* ptr = malloc(total);
  if (ptr) memset(ptr, 0, total);
  return ptr;
}

void* realloc(void* ptr, size_t size) {
  if (ptr == NULL) return malloc(size);
  if (size == 0) {
    free(ptr);
    return NULL;
  }
  block_header_t* header = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);
  if (header->size >= size) return ptr; // already big enough

  void* new_ptr = malloc(size);
  if (!new_ptr) return NULL;
  memcpy(new_ptr, ptr, header->size);
  free(ptr);
  return new_ptr;
}

// Not required by IREE today, but newlib-adjacent code sometimes references it.
void* aligned_alloc(size_t alignment, size_t size) {
  if (alignment <= 16) return malloc(size);
  // The bump allocator only guarantees 16; over-allocate and shift.
  uint8_t* raw = (uint8_t*)malloc(size + alignment + HEADER_SIZE);
  if (!raw) return NULL;
  uintptr_t aligned = ((uintptr_t)raw + HEADER_SIZE + alignment - 1) &
                      ~(uintptr_t)(alignment - 1);
  return (void*)aligned;
}

// How much of the heap has been handed out, for the report at the end.
size_t baremetal_heap_used(void) { return g_heap_used; }
size_t baremetal_heap_size(void) { return BAREMETAL_HEAP_SIZE; }
