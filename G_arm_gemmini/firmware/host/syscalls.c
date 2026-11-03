/* Newlib support, no OS or semihosting. Heap is Arm-private DDR. */
#include "device_abi.h"
#include "diagnostics.h"
#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
extern char __heap_start[], __heap_end[];
static char *heap_cursor;
void *_sbrk(ptrdiff_t increment) {
  if (!heap_cursor) heap_cursor = __heap_start;
  if (increment < 0 || (size_t)increment > (size_t)(__heap_end - heap_cursor)) {
    errno = ENOMEM; return (void *)-1;
  }
  char *old = heap_cursor;
  heap_cursor += increment;
  return old;
}
int _write(int fd, const void *ptr, size_t bytes) {
  (void)fd;
  const char *data = ptr;
  for (size_t i = 0; i < bytes; ++i) g_putchar(data[i]);
  return bytes;
}
int _read(int fd, void *ptr, size_t bytes) {
  (void)fd; (void)ptr; (void)bytes; return 0;
}
int _close(int fd) { (void)fd; errno = EBADF; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int offset, int whence) { (void)fd; (void)offset; (void)whence; return 0; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status) {
  *(volatile uint32_t *)(uintptr_t)G_HOST_REPORT = status ? 2 : 1;
  for (;;) {}
}
