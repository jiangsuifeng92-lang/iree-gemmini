/* H kernel ABI. Offsets/strides count elements; tiles count DIM units. */
#ifndef H_GEMMINI_KERNEL_H
#define H_GEMMINI_KERNEL_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int gemmini_matmul_i8(const int8_t *A, size_t A_off, size_t A_rs,
    const int8_t *B, size_t B_off, size_t B_rs,
    int32_t *C, size_t C_off, size_t C_rs,
    size_t M, size_t N, size_t K);
int gemmini_matmul_i8_planned(const int8_t *A, size_t A_off, size_t A_rs,
    const int8_t *B, size_t B_off, size_t B_rs,
    int32_t *C, size_t C_off, size_t C_rs,
    size_t M, size_t N, size_t K,
    size_t plan_abi, size_t tile_i, size_t tile_j, size_t tile_k,
    size_t block_m, size_t layout_flags);
int gemmini_matmul_i8_requant(const int8_t *A, size_t A_off, size_t A_rs,
    const int8_t *B, size_t B_off, size_t B_rs,
    int8_t *C, size_t C_off, size_t C_rs,
    size_t M, size_t N, size_t K,
    const int32_t *bias, size_t bias_off,
    int64_t mult, int64_t round, int64_t shift,
    int32_t min_v, int32_t max_v);
int gemmini_matmul_i8_requant_planned(const int8_t *A, size_t A_off, size_t A_rs,
    const int8_t *B, size_t B_off, size_t B_rs,
    int8_t *C, size_t C_off, size_t C_rs,
    size_t M, size_t N, size_t K,
    const int32_t *bias, size_t bias_off,
    int64_t mult, int64_t round, int64_t shift,
    int32_t min_v, int32_t max_v,
    size_t plan_abi, size_t tile_i, size_t tile_j, size_t tile_k,
    size_t block_m, size_t layout_flags);
int gemmini_conv2d_i8(const int8_t *in, size_t in_off, size_t in_rs, size_t in_ws,
    const int8_t *w, size_t w_off, size_t w_rs,
    int32_t *out, size_t out_off, size_t out_rs,
    size_t H, size_t W, size_t C, size_t OH, size_t OW, size_t OC,
    size_t KH, size_t KW, size_t stride);
int gemmini_conv2d_i8_planned(const int8_t *in, size_t in_off, size_t in_rs, size_t in_ws,
    const int8_t *w, size_t w_off, size_t w_rs,
    int32_t *out, size_t out_off, size_t out_rs,
    size_t H, size_t W, size_t C, size_t OH, size_t OW, size_t OC,
    size_t KH, size_t KW, size_t stride,
    size_t plan_abi, size_t tile_i, size_t tile_j, size_t tile_k,
    size_t block_m, size_t layout_flags);
int gemmini_conv2d_i8_requant_planned(
    const int8_t *in,size_t in_off,size_t in_rs,size_t in_ws,
    const int8_t *w,size_t w_off,size_t w_rs,
    int8_t *out,size_t out_off,size_t out_rs,
    size_t H,size_t W,size_t C,size_t OH,size_t OW,size_t OC,
    size_t KH,size_t KW,size_t stride,
    const int32_t *bias,size_t bias_off,
    int64_t mult,int64_t round,int64_t shift,int32_t min_v,int32_t max_v,
    size_t plan_abi,size_t tile_i,size_t tile_j,size_t tile_k,
    size_t block_m,size_t layout_flags);
#ifdef __cplusplus
}
#endif
#endif
