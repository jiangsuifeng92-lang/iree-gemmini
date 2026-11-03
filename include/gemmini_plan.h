/* Shared planning model. Compiler chooses; Rocket validates and executes. */
#ifndef H_GEMMINI_PLAN_H
#define H_GEMMINI_PLAN_H
#include <stddef.h>
#include <stdint.h>
#include "gemmini_config.h"

/* Tile sizes are counts of DIM-by-DIM hardware tiles, not element counts.
 * block_m is the number of matrix rows in the reusable software workspace. */
typedef struct {
  size_t abi, tile_i, tile_j, tile_k, block_m, layout;
} h_gemmini_plan_t;

static inline size_t h_gemmini_ceil(size_t n, size_t d) {
  return n / d + (n % d != 0);
}
static inline size_t h_gemmini_min(size_t a, size_t b) {
  return a < b ? a : b;
}
static inline size_t h_gemmini_spad_rows(size_t i, size_t j, size_t k) {
  return (i * k + k * j) * H_GEMMINI_DIM;
}
static inline size_t h_gemmini_acc_rows(size_t i, size_t j) {
  return i * j * H_GEMMINI_DIM;
}

/* Bounded heuristic search for WS with double buffering. The cost is an
 * estimate of input traffic plus command overhead, not a measured latency.
 * N/K are bounded by the caller before this routine is entered. */
static inline h_gemmini_plan_t h_gemmini_choose_plan(
    size_t m, size_t n, size_t k, int convolution) {
  h_gemmini_plan_t p = {H_GEMMINI_PLAN_ABI, 1, 1, 1, 0, 0};
  if (!m || !n || !k || n > H_GEMMINI_MAX_N || k > H_GEMMINI_MAX_K)
    return p;
  p.block_m = h_gemmini_min(m, H_GEMMINI_MAX_M);
  if (convolution) {
    p.block_m = h_gemmini_min(p.block_m, H_GEMMINI_IM2COL_ROWS);
    size_t rows = H_GEMMINI_IM2COL_TARGET_BYTES / k;
    p.block_m = h_gemmini_min(p.block_m, rows ? rows : 1);
  }
  const size_t spad = H_GEMMINI_BANK_NUM * H_GEMMINI_BANK_ROWS / 2;
  const size_t acc = H_GEMMINI_ACC_ROWS / 2;
  const size_t mi = h_gemmini_ceil(p.block_m, H_GEMMINI_DIM);
  const size_t nj = h_gemmini_ceil(n, H_GEMMINI_DIM);
  const size_t kk = h_gemmini_ceil(k, H_GEMMINI_DIM);
  uint64_t best = UINT64_MAX;
  for (size_t i = 1; i <= mi; ++i) {
    for (size_t j = 1; j <= nj; ++j) {
      if (h_gemmini_acc_rows(i, j) > acc) break;
      for (size_t t = 1; t <= kk; ++t) {
        if (h_gemmini_spad_rows(i, j, t) > spad) break;
        size_t bi = h_gemmini_ceil(mi, i);
        size_t bj = h_gemmini_ceil(nj, j);
        size_t bk = h_gemmini_ceil(kk, t);
        uint64_t traffic = (uint64_t)p.block_m * k * bj +
                           (uint64_t)k * n * bi;
        uint64_t score = traffic + (uint64_t)bi * bj * bk *
                                    H_GEMMINI_COMMAND_COST_BYTES;
        if (score < best ||
            (score == best && i * j * t > p.tile_i * p.tile_j * p.tile_k)) {
          best = score;
          p.tile_i = i; p.tile_j = j; p.tile_k = t;
        }
      }
    }
  }
  return p;
}

static inline int h_gemmini_plan_valid(const h_gemmini_plan_t *p,
                                      size_t m, size_t n, size_t k) {
  if (!p || p->abi != H_GEMMINI_PLAN_ABI || !m || !n || !k ||
      n > H_GEMMINI_MAX_N || k > H_GEMMINI_MAX_K ||
      !p->block_m || p->block_m > H_GEMMINI_MAX_M ||
      !p->tile_i || !p->tile_j || !p->tile_k ||
      p->tile_i > h_gemmini_ceil(p->block_m, H_GEMMINI_DIM) ||
      p->tile_j > h_gemmini_ceil(n, H_GEMMINI_DIM) ||
      p->tile_k > h_gemmini_ceil(k, H_GEMMINI_DIM)) return 0;
  return h_gemmini_spad_rows(p->tile_i, p->tile_j, p->tile_k) <=
           H_GEMMINI_BANK_NUM * H_GEMMINI_BANK_ROWS / 2 &&
         h_gemmini_acc_rows(p->tile_i, p->tile_j) <= H_GEMMINI_ACC_ROWS / 2;
}
#endif
