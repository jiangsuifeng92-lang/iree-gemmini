// The handful of C library functions the IREE runtime references but that we
// do not have, because the binary is linked with -nostdlib.
//
// Why this file exists at all
// ---------------------------
// On Linux the IREE runtime gets these from glibc. On the bare-metal target
// there is no libc: support/syscalls.c supplies the small core (printf,
// memcpy, memset, strlen, strcmp, ...) and this file supplies the rest.
//
// Almost everything here is reached only from IREE's *debug* code paths
// (iree_status_fprint, bytecode disassembly, flag parsing). Those objects are
// linked in because they share a translation unit with functions we do call,
// so the symbols must resolve even though they never execute. The exceptions
// are memmove/memcmp/strncmp, which the VM really uses, and those are real
// implementations.
//
// The alternative -- link newlib -- was rejected on purpose: it would drag in
// _sbrk/_read/_close/_fstat and a reent struct for the sake of dead code, and
// would collide with the printf/memcpy that syscalls.c already defines.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

//==============================================================================
// string.h
//==============================================================================

void* memchr(const void* s, int c, size_t n) {
  const unsigned char* p = (const unsigned char*)s;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] == (unsigned char)c) return (void*)(p + i);
  }
  return NULL;
}

int memcmp(const void* a, const void* b, size_t n) {
  const unsigned char* pa = (const unsigned char*)a;
  const unsigned char* pb = (const unsigned char*)b;
  for (size_t i = 0; i < n; ++i) {
    if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
  }
  return 0;
}

void* memmove(void* dst, const void* src, size_t n) {
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  if (d == s || n == 0) return dst;
  if (d < s || d >= s + n) {
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
  } else {
    for (size_t i = n; i > 0; --i) d[i - 1] = s[i - 1];
  }
  return dst;
}

int strncmp(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    unsigned char ca = (unsigned char)a[i];
    unsigned char cb = (unsigned char)b[i];
    if (ca != cb) return (int)ca - (int)cb;
    if (ca == 0) return 0;
  }
  return 0;
}

//==============================================================================
// ctype
//==============================================================================
// newlib's <ctype.h> expands isspace()/isalpha()/... into a table lookup on
// `_ctype_`, so the table has to exist as a real symbol. Index 0 is the EOF
// entry (unused here); index i+1 is the class of character i-128.
//
// Bit values are newlib's: _C control, _U upper, _L lower, _N digit,
// _S white space, _P punctuation, _X hex digit, _B blank.

const char _ctype_[257] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x11, 0x11, 0x11, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x90, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x20, 0x20, 0x20, 0x20,
    0x01,
};

//==============================================================================
// newlib reentrancy
//==============================================================================
// stdio macros such as `stderr` expand to (_REENT->_stderr), and _REENT is
// _impure_ptr on this toolchain. We never use those streams; the symbol only
// has to exist so the link succeeds.

struct _reent;
struct _reent* _impure_ptr = NULL;

int* __errno(void) {
  static int g_errno = 0;
  return &g_errno;
}

//==============================================================================
// strtol / strtoul / strtoll / strtoull / strtod / strtof
//==============================================================================
// Used by IREE's flag parser, which is dead code here. Implemented properly
// anyway so that a future step (reading a model from a file, say) does not
// trip over a stub.

static int digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return -1;
}

// Parses [sign][0x]digits in |base|. Returns the magnitude and advances
// |*s| past the consumed characters.
static unsigned long long parse_unsigned(const char** s, int base,
                                         int* out_negative) {
  const char* p = *s;
  *out_negative = 0;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' ||
         *p == '\v') {
    ++p;
  }
  if (*p == '+' || *p == '-') {
    *out_negative = (*p == '-');
    ++p;
  }
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
      digit_value(p[2]) >= 0 && digit_value(p[2]) < 16) {
    base = 16;
    p += 2;
  } else if (base == 0 && p[0] == '0') {
    base = 8;
  } else if (base == 0) {
    base = 10;
  }

  unsigned long long value = 0;
  while (1) {
    int d = digit_value(*p);
    if (d < 0 || d >= base) break;
    value = value * (unsigned)base + (unsigned)d;
    ++p;
  }
  *s = p;
  return value;
}

long strtol(const char* s, char** end, int base) {
  int neg;
  const char* start = s;
  unsigned long long v = parse_unsigned(&s, base, &neg);
  if (end) *end = (char*)(s == start ? start : s);
  return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char* s, char** end, int base) {
  int neg;
  const char* start = s;
  unsigned long long v = parse_unsigned(&s, base, &neg);
  if (end) *end = (char*)(s == start ? start : s);
  return neg ? (unsigned long)-v : (unsigned long)v;
}

long long strtoll(const char* s, char** end, int base) {
  int neg;
  const char* start = s;
  unsigned long long v = parse_unsigned(&s, base, &neg);
  if (end) *end = (char*)(s == start ? start : s);
  return neg ? -(long long)v : (long long)v;
}

unsigned long long strtoull(const char* s, char** end, int base) {
  int neg;
  const char* start = s;
  unsigned long long v = parse_unsigned(&s, base, &neg);
  if (end) *end = (char*)(s == start ? start : s);
  return neg ? (unsigned long long)-v : v;
}

static double parse_double(const char* s, char** end) {
  const char* start = s;
  int neg = 0;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
  if (*s == '+' || *s == '-') {
    neg = (*s == '-');
    ++s;
  }
  double value = 0.0;
  int seen_digit = 0;
  while (*s >= '0' && *s <= '9') {
    value = value * 10.0 + (double)(*s - '0');
    ++s;
    seen_digit = 1;
  }
  if (*s == '.') {
    ++s;
    double scale = 0.1;
    while (*s >= '0' && *s <= '9') {
      value += (double)(*s - '0') * scale;
      scale *= 0.1;
      ++s;
      seen_digit = 1;
    }
  }
  if (seen_digit && (*s == 'e' || *s == 'E')) {
    const char* exp_start = s;
    ++s;
    int exp_neg = 0;
    if (*s == '+' || *s == '-') {
      exp_neg = (*s == '-');
      ++s;
    }
    int exp = 0;
    if (*s >= '0' && *s <= '9') {
      while (*s >= '0' && *s <= '9') exp = exp * 10 + (*s++ - '0');
      for (int i = 0; i < exp; ++i) value = exp_neg ? value / 10.0 : value * 10.0;
    } else {
      s = exp_start;  // not an exponent after all
    }
  }
  if (end) *end = (char*)(seen_digit ? s : start);
  return neg ? -value : value;
}

double strtod(const char* s, char** end) { return parse_double(s, end); }
float strtof(const char* s, char** end) {
  return (float)parse_double(s, end);
}

//==============================================================================
// stdio (write side only, and only what the runtime references)
//==============================================================================
// These are never called on our path; they exist so that IREE's debug print
// helpers link. They forward to putchar() from syscalls.c so that if one does
// get called the output at least appears on the console.

int fflush(FILE* stream) {
  (void)stream;
  return 0;
}

int fputc(int c, FILE* stream) {
  (void)stream;
  extern int putchar(int);
  return putchar(c);
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
  (void)stream;
  extern int putchar(int);
  const unsigned char* p = (const unsigned char*)ptr;
  size_t total = size * nmemb;
  for (size_t i = 0; i < total; ++i) putchar(p[i]);
  return nmemb;
}

int fprintf(FILE* stream, const char* format, ...) {
  (void)stream;
  (void)format;
  // Deliberately not formatted: this path is only reachable from IREE's
  // debugging helpers, which we never call. Returning 0 keeps it silent
  // instead of pulling vsnprintf into the binary.
  return 0;
}
