// H Gemmini C kernels. Planned entry points consume the compiler's static
// hardware tile, row-workspace and layout decisions. Legacy entry points
// retain the copied F ABI. Exact public signatures are in gemmini_kernel.h.
// Offsets and strides count elements; tile_i/j/k count DIM-sized tiles.
// Kernel code executes on Rocket and is linked into npu.elf, not VMFB.

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "gemmini_plan.h"
#include "gemmini_kernel.h"
#include "gemmini_epilogue.h"

#ifdef GEMMINI_DEBUG
#include <stdio.h>
#endif

// Upper bound on a single dispatch. Anything larger is refused loudly rather
// than silently scribbling over the static scratch buffers.
// Rows per chunk. M is chunked rather than bounded: SqueezeNet's 1x1
// convolutions become 3025-row contractions whose full int32 accumulator is
// megabytes, so the kernel streams the output instead of materializing it.
#ifndef GEMMINI_MAX_M
#define GEMMINI_MAX_M H_GEMMINI_MAX_M
#endif
#ifndef GEMMINI_MAX_N
#define GEMMINI_MAX_N H_GEMMINI_MAX_N
#endif
// K is bounded separately from N: conv2 of the MNIST net is 64x20x250, so K
// reaches 250 while N stays at 20.
#ifndef GEMMINI_MAX_K
#define GEMMINI_MAX_K H_GEMMINI_MAX_K
#endif

#ifdef GEMMINI

#include "gemmini.h"
_Static_assert(DIM == H_GEMMINI_DIM && BANK_NUM == H_GEMMINI_BANK_NUM &&
               BANK_ROWS == H_GEMMINI_BANK_ROWS && ACC_ROWS == H_GEMMINI_ACC_ROWS,
               "H compiler and Gemmini SDK hardware configurations differ");
_Static_assert(GEMMINI_MAX_M == H_GEMMINI_MAX_M &&
               GEMMINI_MAX_N == H_GEMMINI_MAX_N &&
               GEMMINI_MAX_K == H_GEMMINI_MAX_K, "H kernel limits differ from planner");

typedef int8_t elem_t;
typedef int32_t acc_t;

#define PAD16(x) ((((x) + 15) / 16) * 16)

// LOOP_WS handles partial DIM tiles. Dense input need not be DIM-aligned.
// Staging buffer for the conv path's im2col matrix. Sized for the largest
// software tile. H materializes at most IM2COL_ROWS, regardless of total M;
// the compiler separately bounds each tile by IM2COL_TARGET_BYTES.
#ifndef GEMMINI_MAX_IM2COL
#define GEMMINI_MAX_IM2COL (H_GEMMINI_IM2COL_ROWS * PAD16(GEMMINI_MAX_K))
#endif

static elem_t g_ap[GEMMINI_MAX_M * PAD16(GEMMINI_MAX_K)];
static elem_t g_bp[PAD16(GEMMINI_MAX_K) * PAD16(GEMMINI_MAX_N)];
static acc_t  g_cp[GEMMINI_MAX_M * PAD16(GEMMINI_MAX_N)];
static elem_t g_im2col[GEMMINI_MAX_IM2COL];

/* Compiler tile factors are in DIM-sized units. Only the final row chunk
 * truncates tile_i; no runtime search is performed for planned entry points. */
static void h_tiled_matmul(const h_gemmini_plan_t *plan,
    size_t m, size_t n, size_t k, const elem_t *a, const elem_t *b,
    const void *d, void *c, size_t as, size_t bs, size_t ds, size_t cs,
    scale_t sa, scale_t sb, scale_acc_t sd, int act, acc_scale_t scale,
    acc_scale_t bert, bool bias, bool ta, bool tb, bool full, bool low,
    uint8_t weight, enum tiled_matmul_type_t flow) {
  if (!plan) {
    tiled_matmul_auto(m, n, k, a, b, d, c, as, bs, ds, cs, sa, sb, sd,
                     act, scale, bert, bias, ta, tb, full, low, weight, flow);
    return;
  }
  size_t ti = h_gemmini_min(plan->tile_i, h_gemmini_ceil(m, DIM));
  tiled_matmul(m, n, k, a, b, d, c, as, bs, ds, cs, sa, sb, sd,
               act, scale, bert, bias, ti, plan->tile_j, plan->tile_k,
               ta, tb, full, low, weight, flow);
}

static int gemmini_matmul_i8_impl(const int8_t *A, size_t A_rs,
                                  const int8_t *B, size_t B_rs,
                                  int32_t *C, size_t C_rs,
                                  size_t M, size_t N, size_t K,
                                  const h_gemmini_plan_t *plan) {
  if (N > GEMMINI_MAX_N || K > GEMMINI_MAX_K) return -1;

  const size_t Kp = PAD16(K), Np = PAD16(N);

  // Staging exists for one reason only: to turn a row stride that is not the
  // contraction length into a dense K-byte row. It is not needed for 16-byte
  // alignment, which is what this code used to assume. gemmini's LOOP_WS
  // carries pad_K and pad_J, so the last k-tile and j-tile move only the valid
  // columns and the DMA never consumes a padded tail; whatever lands in the
  // accumulator's padding is discarded. Dropping the alignment requirement
  // changed no output bit on either model, and removed most of the copies.
  const int aDirect = (A_rs == K) && (!plan || (plan->layout & H_GEMMINI_DIRECT_A));
  const int bDirect = (B_rs == N) && (!plan || (plan->layout & H_GEMMINI_DIRECT_B));
  const int cDirect = (C_rs == N) && (!plan || (plan->layout & H_GEMMINI_DIRECT_C));

  // B is staged once for all chunks; only A and C are streamed.
  const int8_t *Bsrc = B;
  size_t Bstride = B_rs;
  if (!bDirect) {
    // Zero only the part the copy will not overwrite.
    if (Kp != K || Np != N) memset(g_bp, 0, Kp * Np);
    for (size_t k = 0; k < K; ++k) {
      memcpy(g_bp + k * Np, B + k * B_rs, N);
    }
    Bsrc = g_bp;
    Bstride = Np;
  }

  const size_t block_m = plan ? plan->block_m : GEMMINI_MAX_M;
  for (size_t m0 = 0; m0 < M; m0 += block_m) {
    size_t mc = h_gemmini_min(M - m0, block_m);

    const int8_t *Asrc;
    size_t Astride;
    if (aDirect) {
      Asrc = A + m0 * A_rs;
      Astride = A_rs;
    } else {
      if (Kp != K || PAD16(mc) != mc) memset(g_ap, 0, PAD16(mc) * Kp);
      for (size_t i = 0; i < mc; ++i) {
        memcpy(g_ap + i * Kp, A + (m0 + i) * A_rs, K);
      }
      Asrc = g_ap;
      Astride = Kp;
    }

    void *Cdst;
    size_t Cstride;
    if (cDirect) {
      Cdst = C + m0 * C_rs;
      Cstride = C_rs;
    } else {
      Cdst = g_cp;
      Cstride = Np;
    }

    h_tiled_matmul(plan, mc, N, K,
                      (const elem_t *)Asrc, (const elem_t *)Bsrc, NULL, Cdst,
                      Astride, Bstride, Np, Cstride,
                      MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
                      MVIN_SCALE_IDENTITY,
                      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
                      /*transpose_A=*/false, /*transpose_B=*/false,
                      /*full_C=*/true, /*low_D=*/false,
                      /*weightA=*/0, WS);

    const acc_t *accBase = (const acc_t *)Cdst;
#ifdef GEMMINI_DEBUG
    if (m0 == 0) {
      printf("plain: M=%d N=%d K=%d C_rs=%d A_rs=%d acc0=%d direct=%d%d%d\n",
             (int)M, (int)N, (int)K, (int)C_rs, (int)A_rs, (int)accBase[0],
             aDirect, bDirect, cDirect);
      printf("  acc[0..7] = %d %d %d %d %d %d %d %d\n",
             (int)accBase[0], (int)accBase[1], (int)accBase[2], (int)accBase[3],
             (int)accBase[4], (int)accBase[5], (int)accBase[6], (int)accBase[7]);
    }
#endif
    if (!cDirect) {
      for (size_t i = 0; i < mc; ++i) {
        memcpy(C + (m0 + i) * C_rs, g_cp + i * Np, N * sizeof(acc_t));
      }
    }
  }
  return 0;
}

// Convolution as im2col followed by the matmul above.
//
// The dispatch's linalg.generic indexes the input as (oh + kh, ow + kw, c) and
// the kernel as (kh, kw, c, oc), so flattening k = (kh*KW + kw)*C + c turns it
// into exactly A[OH*OW, K] x B[K, OC]. B needs no transform at all (the weight
// tensor is already [KH][KW][C][OC] row-major); only A has to be gathered.
//
// Deliberately not tiled_conv_stride_auto: that path hardcodes an elem_t (i8)
// output scaled by a 16-bit fixed-point multiplier, while this network's
// requantize is a 64-bit multiply with a 36-39 bit shift. Producing i32 keeps
// the existing epilogue untouched, so the numerics are identical to the
// already-verified matmul path.
//
// The gather is ~2.1 MB over the whole network (~0.3 M instructions), against
// the 1.7 G instructions the scalar 3x3 convolutions cost today.
/* Output positions are flattened [OH,OW]; each tile may cross row edges. */
static void h_im2col_chunk(const int8_t *in,size_t in_rs,size_t in_ws,
    size_t channels,size_t ow,size_t kh,size_t kw,size_t stride,
    size_t first,size_t count) {
  const size_t k=kh*kw*channels;
  for(size_t i=0;i<count;++i) {
    size_t pos=first+i;
    const int8_t *base=in+(pos/ow*stride)*in_rs+(pos%ow*stride)*in_ws;
    int8_t *row=g_im2col+i*k;
    for(size_t y=0;y<kh;++y)for(size_t x=0;x<kw;++x) {
      const int8_t *src=base+y*in_rs+x*in_ws;
      int8_t *dst=row+(y*kw+x)*channels;
      if(channels>=16)memcpy(dst,src,channels);
      else for(size_t c=0;c<channels;++c)dst[c]=src[c];
    }
  }
}

static int gemmini_conv2d_i8_impl(const int8_t *in, size_t in_rs, size_t in_ws,
                                  const int8_t *w, size_t w_rs,
                                  int32_t *out, size_t out_rs,
                                  size_t H, size_t W, size_t C,
                                  size_t OH, size_t OW, size_t OC,
                                  size_t KH, size_t KW, size_t stride,
                                  const h_gemmini_plan_t *plan) {
  (void)H;
  (void)W;
  const size_t M = OH * OW;
  const size_t K = KH * KW * C;
#ifdef GEMMINI_DEBUG
  printf("conv: H=%d W=%d C=%d OH=%d OW=%d OC=%d K=%d s=%d "
         "in_rs=%d in_ws=%d w_rs=%d out_rs=%d\n",
         (int)H, (int)W, (int)C, (int)OH, (int)OW, (int)OC, (int)K,
         (int)stride, (int)in_rs, (int)in_ws, (int)w_rs, (int)out_rs);
#endif
  if (OC > GEMMINI_MAX_N || K > GEMMINI_MAX_K) return -1;
  const size_t rows = plan ? plan->block_m : H_GEMMINI_IM2COL_ROWS;
  if (!rows || rows > H_GEMMINI_IM2COL_ROWS || rows * K > GEMMINI_MAX_IM2COL)
    return -1;
  if ((OH - 1) * stride + KH > H || (OW - 1) * stride + KW > W)
    return -1; // Only the valid-window convolutions accepted by the matcher.

  /* Materialize one dense [rows,K] tile, consume it, then reuse the same
   * workspace. Flat output indexing handles chunks crossing an output row. */
  for (size_t m0 = 0; m0 < M; m0 += rows) {
    size_t count = h_gemmini_min(M - m0, rows);
    h_im2col_chunk(in,in_rs,in_ws,C,OW,KH,KW,stride,m0,count);
    int rc = gemmini_matmul_i8_impl(g_im2col, K, w, w_rs,
                                    out + m0 * out_rs, out_rs,
                                    count, OC, K, plan);
    if (rc) return rc;
  }
  return 0;
}

#else  // !GEMMINI -- plain scalar reference, used for host-side ABI checks.

static int gemmini_matmul_i8_impl(const int8_t *A, size_t A_rs,
                                  const int8_t *B, size_t B_rs,
                                  int32_t *C, size_t C_rs,
                                  size_t M, size_t N, size_t K,
                                  const h_gemmini_plan_t *plan) {
  (void)plan;
  for (size_t i = 0; i < M; ++i) {
    for (size_t j = 0; j < N; ++j) {
      int32_t acc = 0;
      for (size_t k = 0; k < K; ++k) acc = (int32_t)((uint32_t)acc + (uint32_t)((int32_t)A[i*A_rs+k]*B[k*B_rs+j]));
      C[i * C_rs + j] = acc;
    }
  }
  return 0;
}

static int gemmini_conv2d_i8_impl(const int8_t *in, size_t in_rs, size_t in_ws,
                                  const int8_t *w, size_t w_rs,
                                  int32_t *out, size_t out_rs,
                                  size_t H, size_t W, size_t C,
                                  size_t OH, size_t OW, size_t OC,
                                  size_t KH, size_t KW, size_t stride,
                                  const h_gemmini_plan_t *plan) {
  (void)H;
  (void)W;
  (void)plan;
  for (size_t oh = 0; oh < OH; ++oh) {
    for (size_t ow = 0; ow < OW; ++ow) {
      for (size_t oc = 0; oc < OC; ++oc) {
        int32_t acc = 0;
        for (size_t kh = 0; kh < KH; ++kh)
          for (size_t kw = 0; kw < KW; ++kw)
            for (size_t c = 0; c < C; ++c)
              acc += (int32_t)in[(oh * stride + kh) * in_rs +
                                 (ow * stride + kw) * in_ws + c] *
                     w[((kh * KW + kw) * C + c) * w_rs + oc];
        out[(oh * OW + ow) * out_rs + oc] = acc;
      }
    }
  }
  return 0;
}

#endif  // GEMMINI
// The externally-visible entry point. Keeping the offset arithmetic in one
// place makes the ABI obvious when debugging.
#ifdef GEMMINI_DEBUG
#include <stdio.h>
#endif

int gemmini_matmul_i8(const int8_t *A, size_t A_off, size_t A_rs,
                      const int8_t *B, size_t B_off, size_t B_rs,
                      int32_t *C, size_t C_off, size_t C_rs,
                      size_t M, size_t N, size_t K) {
#ifdef GEMMINI_DEBUG
  printf("gemmini_mm M=%d N=%d K=%d A(off=%d rs=%d) B(off=%d rs=%d) C(off=%d rs=%d)\n",
         (int)M, (int)N, (int)K, (int)A_off, (int)A_rs, (int)B_off, (int)B_rs,
         (int)C_off, (int)C_rs);
#endif
  return gemmini_matmul_i8_impl(A + A_off, A_rs, B + B_off, B_rs, C + C_off,
                                C_rs, M, N, K, NULL);
}

// Same offset convention as the matmul: offsets are element offsets from
// iree_codegen.extract_strided_metadata, row strides are element counts.
int gemmini_conv2d_i8(const int8_t *in, size_t in_off, size_t in_rs, size_t in_ws,
                      const int8_t *w, size_t w_off, size_t w_rs,
                      int32_t *out, size_t out_off, size_t out_rs,
                      size_t H, size_t W, size_t C,
                      size_t OH, size_t OW, size_t OC,
                      size_t KH, size_t KW, size_t stride) {
  return gemmini_conv2d_i8_impl(in + in_off, in_rs, in_ws, w + w_off, w_rs,
                                out + out_off, out_rs, H, W, C, OH, OW, OC,
                                KH, KW, stride, NULL);
}


// Fused epilogue variant.
//
// Without this, every matmul writes its int32 accumulator to memory and the
// separate linalg.generic epilogue reads it back to apply bias + fixed-point
// requantize + clamp -- for SqueezeNet that is ~774 KB written and read per
// dispatch. The int32 tile is written to g_cp in DDR and requantized by
// Rocket. Fusion does not imply keeping the accumulator inside Gemmini.
//
// The parameters mirror exactly what the epilogue body computes:
//   acc  = in + bias[j]
//   v    = (int64)acc * mult + round
//   v  >>= shift                     (arithmetic)
//   t    = clamp((int32)v, min_v, max_v)
//   out  = (int8)t
static int h_matmul_requant_impl(const int8_t *A, size_t A_off, size_t A_rs,
                              const int8_t *B, size_t B_off, size_t B_rs,
                              int8_t *C, size_t C_off, size_t C_rs,
                              size_t M, size_t N, size_t K,
                              const int32_t *bias, size_t bias_off,
                              int64_t mult, int64_t round, int64_t shift,
                              int32_t min_v, int32_t max_v,
                              const h_gemmini_plan_t *plan) {
  if (N > GEMMINI_MAX_N || K > GEMMINI_MAX_K || shift<0 || shift>=64 ||
      min_v < -128 || max_v>127 || min_v>max_v) return -1;

  // This symbol is called directly from the compiled dispatch, not through the
  // wrapper that gemmini_matmul_i8 has, so every offset has to be applied here.
  // All three were missing at first, which silently computed the matmul on the
  // wrong rows: it went unnoticed on the fully-connected layers because their
  // offsets happen to be zero, and corrupted every convolution.
  A += A_off;
  B += B_off;
  C += C_off;
  bias += bias_off;
#ifdef GEMMINI
  const size_t Kp = PAD16(K), Np = PAD16(N);

  // Same direct-path rule as gemmini_matmul_i8_impl: stage only when the row
  // stride is not the contraction length. The output side cannot go direct
  // here -- the matmul has to land in int32 before the requantize reads it --
  // so only A and B get the shortcut.
  const int aDirect = (A_rs == K) && (!plan || (plan->layout & H_GEMMINI_DIRECT_A));
  const int bDirect = (B_rs == N) && (!plan || (plan->layout & H_GEMMINI_DIRECT_B));

  const int8_t *Bsrc = B;
  size_t Bstride = B_rs;
  if (!bDirect) {
    if (Kp != K || Np != N) memset(g_bp, 0, Kp * Np);
    for (size_t k = 0; k < K; ++k) {
      memcpy(g_bp + k * Np, B + k * B_rs, N);
    }
    Bsrc = g_bp;
    Bstride = Np;
  }

  const size_t block_m = plan ? plan->block_m : GEMMINI_MAX_M;
  for (size_t m0 = 0; m0 < M; m0 += block_m) {
    size_t mc = h_gemmini_min(M - m0, block_m);

    const int8_t *Asrc;
    size_t Astride;
    if (aDirect) {
      Asrc = A + m0 * A_rs;
      Astride = A_rs;
    } else {
      if (Kp != K || PAD16(mc) != mc) memset(g_ap, 0, PAD16(mc) * Kp);
      for (size_t i = 0; i < mc; ++i) {
        memcpy(g_ap + i * Kp, A + (m0 + i) * A_rs, K);
      }
      Asrc = g_ap;
      Astride = Kp;
    }

    if (h_gemmini_identity_epilogue(mult,round,shift,min_v,max_v)) {
      h_tiled_matmul(plan,mc,N,K,(const elem_t *)Asrc,(const elem_t *)Bsrc,
          bias,C+m0*C_rs,Astride,Bstride,N,C_rs,
          MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,
          min_v==0?RELU:NO_ACTIVATION,ACC_SCALE_IDENTITY,0,true,
          false,false,false,false,0,WS);
      continue; // g_cp is not written/read on this path.
    }

    h_tiled_matmul(plan, mc, N, K,
                      (const elem_t *)Asrc, (const elem_t *)Bsrc, NULL, g_cp,
                      Astride, Bstride, Np, Np,
                      MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
                      MVIN_SCALE_IDENTITY,
                      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
                      /*transpose_A=*/false, /*transpose_B=*/false,
                      /*full_C=*/true, /*low_D=*/false,
                      /*weightA=*/0, WS);

    for (size_t i = 0; i < mc; ++i) {
      int8_t *row = C + (m0 + i) * C_rs;
      const acc_t *acc = g_cp + i * Np;
#ifdef GEMMINI_DEBUG
      if (m0 == 0 && i == 0) {
        int64_t v0 = (int64_t)(acc[0] + bias[0]) * mult + round;
        v0 >>= shift;
        printf("fused: M=%d N=%d K=%d C_off=%d C_rs=%d A_off=%d A_rs=%d "
               "acc0=%d bias0=%d v=%lld\n",
               (int)M, (int)N, (int)K, (int)C_off, (int)C_rs, (int)A_off,
               (int)A_rs, (int)acc[0], (int)bias[0], (long long)v0);
        printf("      mult=%lld round=%lld shift=%lld min=%d max=%d\n",
               (long long)mult, (long long)round, (long long)shift,
               (int)min_v, (int)max_v, (long long)v0);
        printf("      acc[0..7] =");
        for (int q = 0; q < 8; ++q) printf(" %d", (int)acc[q]);
        printf("\n      bias[0..7]=");
        for (int q = 0; q < 8; ++q) printf(" %d", (int)bias[q]);
        printf("\n      v[0..7]   =");
        for (int q = 0; q < 8; ++q) {
          int64_t vv = (int64_t)(acc[q] + bias[q]) * mult + round;
          vv >>= shift;
          printf(" %d", (int)vv);
        }
        printf("\n");
      }
#endif
      for (size_t j = 0; j < N; ++j) {
        row[j]=h_gemmini_requantize(acc[j],bias[j],mult,round,
                                     (unsigned)shift,min_v,max_v);
      }
    }
  }
#else
  (void)plan;
  for (size_t i = 0; i < M; ++i) {
    for (size_t j = 0; j < N; ++j) {
      int32_t acc = 0;
      for (size_t kk = 0; kk < K; ++kk)
        acc = (int32_t)((uint32_t)acc + (uint32_t)((int32_t)A[i*A_rs+kk]*B[kk*B_rs+j]));
      C[i*C_rs+j]=h_gemmini_requantize(acc,bias[j],mult,round,
                                       (unsigned)shift,min_v,max_v);
    }
  }
#endif

  return 0;
}

int gemmini_matmul_i8_requant(const int8_t *A, size_t A_off, size_t A_rs,
    const int8_t *B, size_t B_off, size_t B_rs,
    int8_t *C, size_t C_off, size_t C_rs,
    size_t M, size_t N, size_t K,
    const int32_t *bias, size_t bias_off,
    int64_t mult, int64_t round, int64_t shift,
    int32_t min_v, int32_t max_v) {
  return h_matmul_requant_impl(A, A_off, A_rs, B, B_off, B_rs,
      C, C_off, C_rs, M, N, K, bias, bias_off, mult, round, shift,
      min_v, max_v, NULL);
}

/* Generated constants are validated before any DMA/workspace access. */
int gemmini_matmul_i8_planned(const int8_t *A, size_t A_off, size_t A_rs,
    const int8_t *B, size_t B_off, size_t B_rs,
    int32_t *C, size_t C_off, size_t C_rs,
    size_t M, size_t N, size_t K,
    size_t plan_abi, size_t tile_i, size_t tile_j, size_t tile_k,
    size_t block_m, size_t layout_flags) {
  const h_gemmini_plan_t p = {plan_abi, tile_i, tile_j, tile_k, block_m, layout_flags};
  if (!h_gemmini_plan_valid(&p, M, N, K)) return -1;
  if (A_rs < K || B_rs < N || C_rs < N) return -2;
  return gemmini_matmul_i8_impl(A + A_off, A_rs, B + B_off, B_rs,
      C + C_off, C_rs, M, N, K, &p);
}

int gemmini_matmul_i8_requant_planned(const int8_t *A, size_t A_off, size_t A_rs,
    const int8_t *B, size_t B_off, size_t B_rs,
    int8_t *C, size_t C_off, size_t C_rs,
    size_t M, size_t N, size_t K,
    const int32_t *bias, size_t bias_off,
    int64_t mult, int64_t round, int64_t shift,
    int32_t min_v, int32_t max_v,
    size_t plan_abi, size_t tile_i, size_t tile_j, size_t tile_k,
    size_t block_m, size_t layout_flags) {
  const h_gemmini_plan_t p = {plan_abi, tile_i, tile_j, tile_k, block_m, layout_flags};
  if (!h_gemmini_plan_valid(&p, M, N, K)) return -1;
  if (A_rs < K || B_rs < N || C_rs < N || shift < 0 || shift >= 64) return -2;
  return h_matmul_requant_impl(A, A_off, A_rs, B, B_off, B_rs,
      C, C_off, C_rs, M, N, K, bias, bias_off, mult, round, shift,
      min_v, max_v, &p);
}

int gemmini_conv2d_i8_planned(const int8_t *in, size_t in_off, size_t in_rs, size_t in_ws,
    const int8_t *w, size_t w_off, size_t w_rs,
    int32_t *out, size_t out_off, size_t out_rs,
    size_t H, size_t W, size_t C, size_t OH, size_t OW, size_t OC,
    size_t KH, size_t KW, size_t stride,
    size_t plan_abi, size_t tile_i, size_t tile_j, size_t tile_k,
    size_t block_m, size_t layout_flags) {
  const h_gemmini_plan_t p = {plan_abi, tile_i, tile_j, tile_k, block_m, layout_flags};
  if (!OH || !OW || !KH || !KW || !C || !stride) return -1;
  if (!h_gemmini_plan_valid(&p, OH * OW, OC, KH * KW * C) ||
      block_m > H_GEMMINI_IM2COL_ROWS) return -1;
  if (w_rs < OC || out_rs < OC || in_ws < C || in_rs < W * in_ws) return -2;
  return gemmini_conv2d_i8_impl(in + in_off, in_rs, in_ws,
      w + w_off, w_rs, out + out_off, out_rs,
      H, W, C, OH, OW, OC, KH, KW, stride, &p);
}

/* Gather, compute and requantize one output tile before gathering the next.
 * No full [OH*OW,OC] int32 intermediate is materialized by this entry point. */
int gemmini_conv2d_i8_requant_planned(
    const int8_t *in,size_t in_off,size_t in_rs,size_t in_ws,
    const int8_t *w,size_t w_off,size_t w_rs,
    int8_t *out,size_t out_off,size_t out_rs,
    size_t H,size_t W,size_t C,size_t OH,size_t OW,size_t OC,
    size_t KH,size_t KW,size_t stride,
    const int32_t *bias,size_t bias_off,
    int64_t mult,int64_t round,int64_t shift,int32_t min_v,int32_t max_v,
    size_t plan_abi,size_t tile_i,size_t tile_j,size_t tile_k,
    size_t block_m,size_t layout_flags) {
  h_gemmini_plan_t p={plan_abi,tile_i,tile_j,tile_k,block_m,layout_flags};
  if(!OH || !OW || !KH || !KW || !C || !stride || shift<0 || shift>=64 ||
     min_v < -128 || max_v>127 || min_v>max_v ||
     !h_gemmini_plan_valid(&p,OH*OW,OC,KH*KW*C) ||
     block_m>H_GEMMINI_IM2COL_ROWS) return -1;
  if(w_rs<OC || out_rs<OC || in_ws<C || in_rs<W*in_ws ||
     (OH-1)*stride+KH>H || (OW-1)*stride+KW>W) return -2;
  in+=in_off;w+=w_off;out+=out_off;bias+=bias_off;
#ifdef GEMMINI
  const size_t K=KH*KW*C,M=OH*OW;
  // The gathered A tile and matched weights are dense. Output need not be.
  p.layout|=H_GEMMINI_DIRECT_A;
  for(size_t m0=0;m0<M;m0+=block_m) {
    size_t count=h_gemmini_min(M-m0,block_m);
    h_im2col_chunk(in,in_rs,in_ws,C,OW,KH,KW,stride,m0,count);
    int status=h_matmul_requant_impl(g_im2col,0,K,w,0,w_rs,
        out+m0*out_rs,0,out_rs,count,OC,K,bias,0,mult,round,shift,min_v,max_v,&p);
    if(status)return status;
  }
#else
  for(size_t y=0;y<OH;++y)for(size_t x=0;x<OW;++x)for(size_t oc=0;oc<OC;++oc) {
    int32_t acc=0;
    for(size_t ky=0;ky<KH;++ky)for(size_t kx=0;kx<KW;++kx)for(size_t c=0;c<C;++c) {
      int32_t product=(int32_t)in[(y*stride+ky)*in_rs+(x*stride+kx)*in_ws+c]*
                         w[((ky*KW+kx)*C+c)*w_rs+oc];
      acc=(int32_t)((uint32_t)acc+(uint32_t)product);
    }
    out[(y*OW+x)*out_rs+oc]=h_gemmini_requantize(acc,bias[oc],mult,round,
                                              (unsigned)shift,min_v,max_v);
  }
#endif
  return 0;
}
