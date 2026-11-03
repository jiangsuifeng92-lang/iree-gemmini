/* Exact integer semantics and the narrowly supported accumulator fast path. */
#ifndef H_GEMMINI_EPILOGUE_H
#define H_GEMMINI_EPILOGUE_H
#include <stdint.h>
#include "gemmini_config.h"

/* Gemmini uses FP32/RNE scaling. General fixed-point multiply/add/shift is
 * not interchangeable with that operation. Identity scaling with ordinary
 * signed-int8 saturation (optionally ReLU) is the supported exact subset. */
static inline int h_gemmini_identity_epilogue(int64_t mult,int64_t round,
    int64_t shift,int32_t min_v,int32_t max_v) {
  return H_GEMMINI_IDENTITY_MVOUT && mult==1 && round==0 && shift==0 &&
         max_v==127 && (min_v==-128 || min_v==0);
}
/* MLIR add/mul truncate modulo the operand width. Use unsigned arithmetic
 * to preserve that behavior even when C signed arithmetic would overflow. */
static inline int8_t h_gemmini_requantize(int32_t acc,int32_t bias,
    int64_t mult,int64_t round,unsigned shift,int32_t min_v,int32_t max_v) {
  int32_t sum=(int32_t)((uint32_t)acc+(uint32_t)bias);
  uint64_t bits=(uint64_t)(int64_t)sum*(uint64_t)mult+(uint64_t)round;
  uint64_t shifted=bits>>shift;
  if (shift && (bits>>63)) shifted|=UINT64_MAX<<(64-shift);
  int32_t value=(int32_t)(uint32_t)shifted;
  if (value<min_v)value=min_v;
  if (value>max_v)value=max_v;
  return (int8_t)value;
}
#endif
