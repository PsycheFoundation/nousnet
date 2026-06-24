#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>

#include "cuda_compat_stub.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PSYCHE_CUDA_METAL_STRINGIFY_(value) #value
#define PSYCHE_CUDA_METAL_STRINGIFY(value) PSYCHE_CUDA_METAL_STRINGIFY_(value)
#define PSYCHE_CUDA_METAL_BWD_FILTER_PARTIAL_THREADS_LITERAL 256u

static pthread_mutex_t psyche_cuda_metal_mutex = PTHREAD_MUTEX_INITIALIZER;
static int psyche_cuda_metal_initialized = 0;
static CUresult psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
static __strong id<MTLDevice> psyche_cuda_metal_device = nil;
static __strong id<MTLCommandQueue> psyche_cuda_metal_queue = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_vector_add_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_saxpy_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_scale_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_copy_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_dot_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_sum_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_abs_sum_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_nrm2_pair_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_nrm2_combine_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_sgemv_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_sger_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cusparse_spmv_csr_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cusparse_spmm_csr_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_axpby_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_activation_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_activation_backward_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_transform_tensor_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_add_tensor_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_batchnorm_inference_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_convolution_forward_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_convolution_bias_activation_forward_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_convolution_backward_data_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_convolution_backward_filter_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_convolution_backward_filter_partial_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_convolution_backward_filter_reduce_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_convolution_mpsgraph_apply_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_convolution_bias_activation_mpsgraph_apply_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_convolution_backward_filter_mpsgraph_apply_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_pooling_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_pooling_backward_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_softmax_f32 = nil;
static __strong id<MTLComputePipelineState> psyche_cuda_metal_cudnn_softmax_backward_f32 = nil;

enum {
  PSYCHE_CUDA_METAL_REDUCTION_THREADS = PSYCHE_CUDA_METAL_BWD_FILTER_PARTIAL_THREADS_LITERAL,
  PSYCHE_CUDA_METAL_BWD_FILTER_PARTIAL_THREADS = PSYCHE_CUDA_METAL_BWD_FILTER_PARTIAL_THREADS_LITERAL,
  PSYCHE_CUDA_METAL_BWD_FILTER_CHUNK_SPAN = 1024u,
  PSYCHE_CUDA_METAL_BWD_FILTER_SCRATCH_CAP_BYTES = 64u * 1024u * 1024u,
};

_Static_assert(
    PSYCHE_CUDA_METAL_BWD_FILTER_PARTIAL_THREADS == 256u,
    "The embedded backward-filter partial MSL kernel uses a 256-lane threadgroup tree.");

typedef struct psyche_cuda_metal_cudnn_convolution_bias_activation_forward_params {
  uint32_t total;
  uint32_t activation_mode;
  uint32_t mode;
  uint32_t groups;
  uint32_t batches;
  uint32_t in_channels;
  uint32_t in_h;
  uint32_t in_w;
  uint32_t out_channels;
  uint32_t out_h;
  uint32_t out_w;
  uint32_t filter_h;
  uint32_t filter_w;
  uint32_t pad_h;
  uint32_t pad_w;
  uint32_t stride_h;
  uint32_t stride_w;
  uint32_t dilation_h;
  uint32_t dilation_w;
  uint32_t has_z;
  float alpha1;
  float alpha2;
} psyche_cuda_metal_cudnn_convolution_bias_activation_forward_params;

static int psyche_cuda_metal_ranges_overlap_sized(
    const void *a,
    size_t a_bytes,
    const void *b,
    size_t b_bytes) {
  uintptr_t a_start;
  uintptr_t b_start;
  uintptr_t a_end;
  uintptr_t b_end;
  if (a_bytes == 0 || b_bytes == 0 || a == 0 || b == 0) {
    return 0;
  }
  a_start = (uintptr_t)a;
  b_start = (uintptr_t)b;
  if (a_start > UINTPTR_MAX - a_bytes || b_start > UINTPTR_MAX - b_bytes) {
    return 1;
  }
  a_end = a_start + a_bytes;
  b_end = b_start + b_bytes;
  return a_start < b_end && b_start < a_end;
}

static CUresult psyche_cuda_metal_init_locked(void) {
  static NSString *const source =
      @"#include <metal_stdlib>\n"
       "using namespace metal;\n"
       "struct PsycheCudnnConvolutionBiasActivationForwardParams {\n"
       "  uint total;\n"
       "  uint activation_mode;\n"
       "  uint mode;\n"
       "  uint groups;\n"
       "  uint batches;\n"
       "  uint in_channels;\n"
       "  uint in_h;\n"
       "  uint in_w;\n"
       "  uint out_channels;\n"
       "  uint out_h;\n"
       "  uint out_w;\n"
       "  uint filter_h;\n"
       "  uint filter_w;\n"
       "  uint pad_h;\n"
       "  uint pad_w;\n"
       "  uint stride_h;\n"
       "  uint stride_w;\n"
       "  uint dilation_h;\n"
       "  uint dilation_w;\n"
       "  uint has_z;\n"
       "  float alpha1;\n"
       "  float alpha2;\n"
       "};\n"
       "kernel void psyche_vector_add_f32(\n"
       "    device const float *a [[buffer(0)]],\n"
       "    device const float *b [[buffer(1)]],\n"
       "    device float *out [[buffer(2)]],\n"
       "    constant uint &n [[buffer(3)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < n) {\n"
       "    out[tid] = a[tid] + b[tid];\n"
       "  }\n"
       "}\n"
       "kernel void psyche_saxpy_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device float *y [[buffer(1)]],\n"
       "    constant float &alpha [[buffer(2)]],\n"
       "    constant uint &n [[buffer(3)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < n) {\n"
       "    y[tid] = alpha * x[tid] + y[tid];\n"
       "  }\n"
       "}\n"
       "kernel void psyche_scale_f32(\n"
       "    device float *x [[buffer(0)]],\n"
       "    constant float &alpha [[buffer(1)]],\n"
       "    constant uint &n [[buffer(2)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < n) {\n"
       "    x[tid] = alpha * x[tid];\n"
       "  }\n"
       "}\n"
       "kernel void psyche_copy_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device float *y [[buffer(1)]],\n"
       "    constant uint &n [[buffer(2)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < n) {\n"
       "    y[tid] = x[tid];\n"
       "  }\n"
       "}\n"
       "kernel void psyche_dot_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *y [[buffer(1)]],\n"
       "    device float *partials [[buffer(2)]],\n"
       "    constant uint &n [[buffer(3)]],\n"
       "    uint tid [[thread_position_in_grid]],\n"
       "    uint lid [[thread_index_in_threadgroup]],\n"
       "    uint group [[threadgroup_position_in_grid]]) {\n"
       "  threadgroup float scratch[256];\n"
       "  float value = 0.0f;\n"
       "  if (tid < n) {\n"
       "    value = x[tid] * y[tid];\n"
       "  }\n"
       "  scratch[lid] = value;\n"
       "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  for (uint stride = 128; stride > 0; stride >>= 1) {\n"
       "    if (lid < stride) {\n"
       "      scratch[lid] += scratch[lid + stride];\n"
       "    }\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  }\n"
       "  if (lid == 0) {\n"
       "    partials[group] = scratch[0];\n"
       "  }\n"
       "}\n"
       "kernel void psyche_sum_f32(\n"
       "    device const float *input [[buffer(0)]],\n"
       "    device float *partials [[buffer(1)]],\n"
       "    constant uint &n [[buffer(2)]],\n"
       "    uint tid [[thread_position_in_grid]],\n"
       "    uint lid [[thread_index_in_threadgroup]],\n"
       "    uint group [[threadgroup_position_in_grid]]) {\n"
       "  threadgroup float scratch[256];\n"
       "  float value = 0.0f;\n"
       "  if (tid < n) {\n"
       "    value = input[tid];\n"
       "  }\n"
       "  scratch[lid] = value;\n"
       "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  for (uint stride = 128; stride > 0; stride >>= 1) {\n"
       "    if (lid < stride) {\n"
       "      scratch[lid] += scratch[lid + stride];\n"
       "    }\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  }\n"
       "  if (lid == 0) {\n"
       "    partials[group] = scratch[0];\n"
       "  }\n"
       "}\n"
       "kernel void psyche_abs_sum_f32(\n"
       "    device const float *input [[buffer(0)]],\n"
       "    device float *partials [[buffer(1)]],\n"
       "    constant uint &n [[buffer(2)]],\n"
       "    uint tid [[thread_position_in_grid]],\n"
       "    uint lid [[thread_index_in_threadgroup]],\n"
       "    uint group [[threadgroup_position_in_grid]]) {\n"
       "  threadgroup float scratch[256];\n"
       "  float value = 0.0f;\n"
       "  if (tid < n) {\n"
       "    value = fabs(input[tid]);\n"
       "  }\n"
       "  scratch[lid] = value;\n"
       "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  for (uint stride = 128; stride > 0; stride >>= 1) {\n"
       "    if (lid < stride) {\n"
       "      scratch[lid] += scratch[lid + stride];\n"
       "    }\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  }\n"
       "  if (lid == 0) {\n"
       "    partials[group] = scratch[0];\n"
       "  }\n"
       "}\n"
       "static inline float2 psyche_nrm2_combine_pair(float2 a, float2 b) {\n"
       "  if (isnan(a.x) || isnan(b.x) || isnan(a.y) || isnan(b.y)) {\n"
       "    return float2(a.x + b.x, a.y + b.y);\n"
       "  }\n"
       "  if (a.x == 0.0f) {\n"
       "    return b;\n"
       "  }\n"
       "  if (b.x == 0.0f) {\n"
       "    return a;\n"
       "  }\n"
       "  if (isinf(a.x)) {\n"
       "    return a;\n"
       "  }\n"
       "  if (isinf(b.x)) {\n"
       "    return b;\n"
       "  }\n"
       "  if (a.x < b.x) {\n"
       "    float ratio = a.x / b.x;\n"
       "    return float2(b.x, b.y + a.y * ratio * ratio);\n"
       "  }\n"
       "  if (a.x > b.x) {\n"
       "    float ratio = b.x / a.x;\n"
       "    return float2(a.x, a.y + b.y * ratio * ratio);\n"
       "  }\n"
       "  return float2(a.x, a.y + b.y);\n"
       "}\n"
       "kernel void psyche_nrm2_pair_f32(\n"
       "    device const float *input [[buffer(0)]],\n"
       "    device float2 *partials [[buffer(1)]],\n"
       "    constant uint &n [[buffer(2)]],\n"
       "    uint tid [[thread_position_in_grid]],\n"
       "    uint lid [[thread_index_in_threadgroup]],\n"
       "    uint group [[threadgroup_position_in_grid]]) {\n"
       "  threadgroup float2 scratch[256];\n"
       "  float2 value = float2(0.0f, 0.0f);\n"
       "  if (tid < n) {\n"
       "    float ax = fabs(input[tid]);\n"
       "    if (ax != 0.0f) {\n"
       "      value = float2(ax, 1.0f);\n"
       "    }\n"
       "  }\n"
       "  scratch[lid] = value;\n"
       "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  for (uint stride = 128; stride > 0; stride >>= 1) {\n"
       "    if (lid < stride) {\n"
       "      scratch[lid] = psyche_nrm2_combine_pair(scratch[lid], scratch[lid + stride]);\n"
       "    }\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  }\n"
       "  if (lid == 0) {\n"
       "    partials[group] = scratch[0];\n"
       "  }\n"
       "}\n"
       "kernel void psyche_nrm2_combine_f32(\n"
       "    device const float2 *input [[buffer(0)]],\n"
       "    device float2 *partials [[buffer(1)]],\n"
       "    constant uint &n [[buffer(2)]],\n"
       "    uint tid [[thread_position_in_grid]],\n"
       "    uint lid [[thread_index_in_threadgroup]],\n"
       "    uint group [[threadgroup_position_in_grid]]) {\n"
       "  threadgroup float2 scratch[256];\n"
       "  float2 value = float2(0.0f, 0.0f);\n"
       "  if (tid < n) {\n"
       "    value = input[tid];\n"
       "  }\n"
       "  scratch[lid] = value;\n"
       "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  for (uint stride = 128; stride > 0; stride >>= 1) {\n"
       "    if (lid < stride) {\n"
       "      scratch[lid] = psyche_nrm2_combine_pair(scratch[lid], scratch[lid + stride]);\n"
       "    }\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  }\n"
       "  if (lid == 0) {\n"
       "    partials[group] = scratch[0];\n"
       "  }\n"
       "}\n"
       "kernel void psyche_sgemv_f32(\n"
       "    device const float *A [[buffer(0)]],\n"
       "    device const float *x [[buffer(1)]],\n"
       "    device const float *y_in [[buffer(2)]],\n"
       "    device float *y_out [[buffer(3)]],\n"
       "    constant float &alpha [[buffer(4)]],\n"
       "    constant float &beta [[buffer(5)]],\n"
       "    constant uint &lda [[buffer(6)]],\n"
       "    constant uint &trans [[buffer(7)]],\n"
       "    constant uint &input_len [[buffer(8)]],\n"
       "    constant uint &output_len [[buffer(9)]],\n"
       "    uint out [[thread_position_in_grid]]) {\n"
       "  if (out >= output_len) {\n"
       "    return;\n"
       "  }\n"
       "  float acc = 0.0f;\n"
       "  if (alpha != 0.0f) {\n"
       "    for (uint inner = 0; inner < input_len; inner++) {\n"
       "      uint a_index = trans == 0u ? out + inner * lda : inner + out * lda;\n"
       "      acc += A[a_index] * x[inner];\n"
       "    }\n"
       "  }\n"
       "  float beta_term = 0.0f;\n"
       "  if (beta != 0.0f) {\n"
       "    beta_term = beta * y_in[out];\n"
       "  }\n"
       "  y_out[out] = alpha * acc + beta_term;\n"
       "}\n"
       "kernel void psyche_sger_f32(\n"
       "    device const float *A_in [[buffer(0)]],\n"
       "    device const float *x [[buffer(1)]],\n"
       "    device const float *y [[buffer(2)]],\n"
       "    device float *A_out [[buffer(3)]],\n"
       "    constant float &alpha [[buffer(4)]],\n"
       "    constant uint &m [[buffer(5)]],\n"
       "    constant uint &lda [[buffer(6)]],\n"
       "    constant uint &total [[buffer(7)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid >= total) {\n"
       "    return;\n"
       "  }\n"
       "  uint row = tid % m;\n"
       "  uint col = tid / m;\n"
       "  uint a_index = row + col * lda;\n"
       "  A_out[a_index] = A_in[a_index] + alpha * x[row] * y[col];\n"
       "}\n"
       "kernel void psyche_cusparse_spmv_csr_f32(\n"
       "    device const int *row_offsets [[buffer(0)]],\n"
       "    device const int *col_indices [[buffer(1)]],\n"
       "    device const float *values [[buffer(2)]],\n"
       "    device const float *x [[buffer(3)]],\n"
       "    device const float *y_in [[buffer(4)]],\n"
       "    device float *y_out [[buffer(5)]],\n"
       "    constant float &alpha [[buffer(6)]],\n"
       "    constant float &beta [[buffer(7)]],\n"
       "    constant uint &rows [[buffer(8)]],\n"
       "    constant uint &cols [[buffer(9)]],\n"
       "    constant uint &nnz [[buffer(10)]],\n"
       "    constant int &index_base [[buffer(11)]],\n"
       "    uint row [[thread_position_in_grid]]) {\n"
       "  if (row >= rows) {\n"
       "    return;\n"
       "  }\n"
       "  int row_start = row_offsets[row] - index_base;\n"
       "  int row_end = row_offsets[row + 1u] - index_base;\n"
       "  float sum = 0.0f;\n"
       "  if (alpha != 0.0f) {\n"
       "    for (int j = row_start; j < row_end; j++) {\n"
       "      if (j >= 0 && uint(j) < nnz) {\n"
       "        int col = col_indices[j] - index_base;\n"
       "        if (col >= 0 && uint(col) < cols) {\n"
       "          sum += values[j] * x[col];\n"
       "        }\n"
       "      }\n"
       "    }\n"
       "  }\n"
       "  float prior = beta != 0.0f ? y_in[row] : 0.0f;\n"
       "  y_out[row] = alpha * sum + beta * prior;\n"
       "}\n"
       "kernel void psyche_cusparse_spmm_csr_f32(\n"
       "    device const int *row_offsets [[buffer(0)]],\n"
       "    device const int *col_indices [[buffer(1)]],\n"
       "    device const float *values [[buffer(2)]],\n"
       "    device const float *b [[buffer(3)]],\n"
       "    device const float *c_in [[buffer(4)]],\n"
       "    device float *out [[buffer(5)]],\n"
       "    constant float &alpha [[buffer(6)]],\n"
       "    constant float &beta [[buffer(7)]],\n"
       "    constant uint &rows [[buffer(8)]],\n"
       "    constant uint &cols [[buffer(9)]],\n"
       "    constant uint &nnz [[buffer(10)]],\n"
       "    constant uint &n [[buffer(11)]],\n"
       "    constant uint &b_ld [[buffer(12)]],\n"
       "    constant uint &c_ld [[buffer(13)]],\n"
       "    constant uint &b_order [[buffer(14)]],\n"
       "    constant uint &c_order [[buffer(15)]],\n"
       "    constant int &index_base [[buffer(16)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  uint total = rows * n;\n"
       "  if (tid >= total) {\n"
       "    return;\n"
       "  }\n"
       "  uint row = tid / n;\n"
       "  uint out_col = tid - row * n;\n"
       "  int row_start = row_offsets[row] - index_base;\n"
       "  int row_end = row_offsets[row + 1u] - index_base;\n"
       "  float sum = 0.0f;\n"
       "  if (alpha != 0.0f) {\n"
       "    for (int j = row_start; j < row_end; j++) {\n"
       "      if (j >= 0 && uint(j) < nnz) {\n"
       "        int b_row = col_indices[j] - index_base;\n"
       "        if (b_row >= 0 && uint(b_row) < cols) {\n"
       "          uint b_index = b_order == 1u ? uint(b_row) + out_col * b_ld : uint(b_row) * b_ld + out_col;\n"
       "          sum += values[j] * b[b_index];\n"
       "        }\n"
       "      }\n"
       "    }\n"
       "  }\n"
       "  float prior = 0.0f;\n"
       "  if (beta != 0.0f) {\n"
       "    uint c_index = c_order == 1u ? row + out_col * c_ld : row * c_ld + out_col;\n"
       "    prior = c_in[c_index];\n"
       "  }\n"
       "  out[tid] = alpha * sum + beta * prior;\n"
       "}\n"
       "kernel void psyche_axpby_f32(\n"
       "    device float *x [[buffer(0)]],\n"
       "    device const float *y [[buffer(1)]],\n"
       "    constant float &alpha [[buffer(2)]],\n"
       "    constant float &beta [[buffer(3)]],\n"
       "    constant uint &n [[buffer(4)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < n) {\n"
       "    x[tid] = alpha * x[tid] + beta * y[tid];\n"
       "  }\n"
       "}\n"
       "static inline float psyche_cudnn_activation_value(\n"
       "    float x,\n"
       "    uint mode,\n"
       "    uint nan_opt) {\n"
       "  if (isnan(x) && nan_opt == 0u) {\n"
       "    return 0.0f;\n"
       "  }\n"
       "  if (mode == 0u) {\n"
       "    return 1.0f / (1.0f + exp(-x));\n"
       "  }\n"
       "  if (mode == 1u) {\n"
       "    if (isnan(x)) {\n"
       "      return x;\n"
       "    }\n"
       "    return fmax(x, 0.0f);\n"
       "  }\n"
       "  if (mode == 5u) {\n"
       "    return x;\n"
       "  }\n"
       "  return tanh(x);\n"
       "}\n"
       "kernel void psyche_cudnn_activation_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *y_in [[buffer(1)]],\n"
       "    device float *out [[buffer(2)]],\n"
       "    constant float &alpha [[buffer(3)]],\n"
       "    constant float &beta [[buffer(4)]],\n"
       "    constant uint &mode [[buffer(5)]],\n"
       "    constant uint &nan_opt [[buffer(6)]],\n"
       "    constant uint &n [[buffer(7)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < n) {\n"
       "    float activated = psyche_cudnn_activation_value(x[tid], mode, nan_opt);\n"
       "    float prior = beta != 0.0f ? y_in[tid] : 0.0f;\n"
       "    out[tid] = alpha * activated + beta * prior;\n"
       "  }\n"
       "}\n"
       "static inline float psyche_cudnn_activation_derivative(\n"
       "    float x,\n"
       "    uint mode,\n"
       "    uint nan_opt) {\n"
       "  if (isnan(x)) {\n"
       "    if (nan_opt == 0u) {\n"
       "      x = 0.0f;\n"
       "    } else {\n"
       "      return x;\n"
       "    }\n"
       "  }\n"
       "  if (mode == 0u) {\n"
       "    float sigmoid = 1.0f / (1.0f + exp(-x));\n"
       "    return sigmoid * (1.0f - sigmoid);\n"
       "  }\n"
       "  if (mode == 1u) {\n"
       "    return x > 0.0f ? 1.0f : 0.0f;\n"
       "  }\n"
       "  if (mode == 5u) {\n"
       "    return 1.0f;\n"
       "  }\n"
       "  float t = tanh(x);\n"
       "  return 1.0f - t * t;\n"
       "}\n"
       "kernel void psyche_cudnn_activation_backward_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *dy [[buffer(1)]],\n"
       "    device const float *dx_in [[buffer(2)]],\n"
       "    device float *out [[buffer(3)]],\n"
       "    constant float &alpha [[buffer(4)]],\n"
       "    constant float &beta [[buffer(5)]],\n"
       "    constant uint &mode [[buffer(6)]],\n"
       "    constant uint &nan_opt [[buffer(7)]],\n"
       "    constant uint &n [[buffer(8)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < n) {\n"
       "    float derivative = psyche_cudnn_activation_derivative(x[tid], mode, nan_opt);\n"
       "    float result = dy[tid] * derivative;\n"
       "    float prior = beta != 0.0f ? dx_in[tid] : 0.0f;\n"
       "    out[tid] = alpha * result + beta * prior;\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_transform_tensor_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *y_in [[buffer(1)]],\n"
       "    device float *out [[buffer(2)]],\n"
       "    constant float &alpha [[buffer(3)]],\n"
       "    constant float &beta [[buffer(4)]],\n"
       "    constant uint &n [[buffer(5)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < n) {\n"
       "    float prior = beta != 0.0f ? y_in[tid] : 0.0f;\n"
       "    out[tid] = alpha * x[tid] + beta * prior;\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_add_tensor_f32(\n"
       "    device const float *a [[buffer(0)]],\n"
       "    device const float *c_in [[buffer(1)]],\n"
       "    device float *out [[buffer(2)]],\n"
       "    constant float &alpha [[buffer(3)]],\n"
       "    constant float &beta [[buffer(4)]],\n"
       "    constant uint &a_n [[buffer(5)]],\n"
       "    constant uint &a_c [[buffer(6)]],\n"
       "    constant uint &a_h [[buffer(7)]],\n"
       "    constant uint &a_w [[buffer(8)]],\n"
       "    constant uint &c_n [[buffer(9)]],\n"
       "    constant uint &c_c [[buffer(10)]],\n"
       "    constant uint &c_h [[buffer(11)]],\n"
       "    constant uint &c_w [[buffer(12)]],\n"
       "    constant uint &total [[buffer(13)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < total) {\n"
       "    uint column = tid % c_w;\n"
       "    uint tmp = tid / c_w;\n"
       "    uint row = tmp % c_h;\n"
       "    tmp = tmp / c_h;\n"
       "    uint channel = tmp % c_c;\n"
       "    uint batch = tmp / c_c;\n"
       "    uint a_batch = a_n == 1u ? 0u : batch;\n"
       "    uint a_channel = a_c == 1u ? 0u : channel;\n"
       "    uint a_row = a_h == 1u ? 0u : row;\n"
       "    uint a_column = a_w == 1u ? 0u : column;\n"
       "    uint a_index = ((a_batch * a_c + a_channel) * a_h + a_row) * a_w + a_column;\n"
       "    float prior = beta != 0.0f ? c_in[tid] : 0.0f;\n"
       "    out[tid] = alpha * a[a_index] + beta * prior;\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_batchnorm_inference_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *y_in [[buffer(1)]],\n"
       "    device float *out [[buffer(2)]],\n"
       "    device const float *scale [[buffer(3)]],\n"
       "    device const float *bias [[buffer(4)]],\n"
       "    device const float *mean [[buffer(5)]],\n"
       "    device const float *variance [[buffer(6)]],\n"
       "    constant float &alpha [[buffer(7)]],\n"
       "    constant float &beta [[buffer(8)]],\n"
       "    constant float &epsilon [[buffer(9)]],\n"
       "    constant uint &mode [[buffer(10)]],\n"
       "    constant uint &channels [[buffer(11)]],\n"
       "    constant uint &height [[buffer(12)]],\n"
       "    constant uint &width [[buffer(13)]],\n"
       "    constant uint &total [[buffer(14)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < total) {\n"
       "    uint column = tid % width;\n"
       "    uint tmp = tid / width;\n"
       "    uint row = tmp % height;\n"
       "    tmp = tmp / height;\n"
       "    uint channel = tmp % channels;\n"
       "    uint param_index = mode == 0u ? (channel * height + row) * width + column : channel;\n"
       "    // Exact x == y is safe because x/y were staged before this separate output buffer.\n"
       "    float normalized = bias[param_index] + scale[param_index] * (x[tid] - mean[param_index]) / sqrt(epsilon + variance[param_index]);\n"
       "    float prior = beta != 0.0f ? y_in[tid] : 0.0f;\n"
       "    out[tid] = alpha * normalized + beta * prior;\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_convolution_forward_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *w [[buffer(1)]],\n"
       "    device const float *y_in [[buffer(2)]],\n"
       "    device float *out [[buffer(3)]],\n"
	       "    constant float &alpha [[buffer(4)]],\n"
	       "    constant float &beta [[buffer(5)]],\n"
	       "    constant uint &mode [[buffer(6)]],\n"
	       "    constant uint &groups [[buffer(7)]],\n"
	       "    constant uint &batches [[buffer(8)]],\n"
	       "    constant uint &in_channels [[buffer(9)]],\n"
	       "    constant uint &in_h [[buffer(10)]],\n"
	       "    constant uint &in_w [[buffer(11)]],\n"
	       "    constant uint &out_channels [[buffer(12)]],\n"
	       "    constant uint &out_h [[buffer(13)]],\n"
	       "    constant uint &out_w [[buffer(14)]],\n"
	       "    constant uint &filter_h [[buffer(15)]],\n"
	       "    constant uint &filter_w [[buffer(16)]],\n"
	       "    constant uint &pad_h [[buffer(17)]],\n"
	       "    constant uint &pad_w [[buffer(18)]],\n"
	       "    constant uint &stride_h [[buffer(19)]],\n"
	       "    constant uint &stride_w [[buffer(20)]],\n"
	       "    constant uint &dilation_h [[buffer(21)]],\n"
	       "    constant uint &dilation_w [[buffer(22)]],\n"
	       "    constant uint &total [[buffer(23)]],\n"
	       "    uint tid [[thread_position_in_grid]]) {\n"
	       "  if (tid < total) {\n"
	       "    uint ow = tid % out_w;\n"
       "    uint tmp = tid / out_w;\n"
       "    uint oh = tmp % out_h;\n"
       "    tmp = tmp / out_h;\n"
	       "    uint k = tmp % out_channels;\n"
	       "    uint n = tmp / out_channels;\n"
	       "    float sum = 0.0f;\n"
	       "    if (n < batches) {\n"
	       "      uint in_channels_per_group = in_channels / groups;\n"
	       "      uint out_channels_per_group = out_channels / groups;\n"
	       "      uint group = k / out_channels_per_group;\n"
	       "      uint input_channel_base = group * in_channels_per_group;\n"
	       "      for (uint c_local = 0u; c_local < in_channels_per_group; c_local++) {\n"
	       "        uint c = input_channel_base + c_local;\n"
	       "        for (uint r = 0u; r < filter_h; r++) {\n"
	       "          int ih = int(oh * stride_h) - int(pad_h) + int(r * dilation_h);\n"
	       "          if (ih < 0 || ih >= int(in_h)) {\n"
       "            continue;\n"
       "          }\n"
       "          uint filter_r = mode == 0u ? (filter_h - 1u - r) : r;\n"
       "          for (uint s = 0u; s < filter_w; s++) {\n"
       "            int iw = int(ow * stride_w) - int(pad_w) + int(s * dilation_w);\n"
       "            if (iw < 0 || iw >= int(in_w)) {\n"
       "              continue;\n"
       "            }\n"
	       "            uint filter_s = mode == 0u ? (filter_w - 1u - s) : s;\n"
	       "            uint x_index = ((n * in_channels + c) * in_h + uint(ih)) * in_w + uint(iw);\n"
	       "            uint w_index = ((k * in_channels_per_group + c_local) * filter_h + filter_r) * filter_w + filter_s;\n"
	       "            sum += x[x_index] * w[w_index];\n"
	       "          }\n"
	       "        }\n"
       "      }\n"
       "    }\n"
       "    float prior = beta != 0.0f ? y_in[tid] : 0.0f;\n"
       "    out[tid] = alpha * sum + beta * prior;\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_convolution_bias_activation_forward_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *w [[buffer(1)]],\n"
       "    device const float *z [[buffer(2)]],\n"
       "    device const float *bias [[buffer(3)]],\n"
       "    device float *out [[buffer(4)]],\n"
       "    constant PsycheCudnnConvolutionBiasActivationForwardParams &p [[buffer(5)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < p.total) {\n"
       "    uint ow = tid % p.out_w;\n"
       "    uint tmp = tid / p.out_w;\n"
       "    uint oh = tmp % p.out_h;\n"
       "    tmp = tmp / p.out_h;\n"
       "    uint k = tmp % p.out_channels;\n"
       "    uint n = tmp / p.out_channels;\n"
       "    float sum = 0.0f;\n"
       "    if (n < p.batches) {\n"
       "      uint in_channels_per_group = p.in_channels / p.groups;\n"
       "      uint out_channels_per_group = p.out_channels / p.groups;\n"
       "      uint group = k / out_channels_per_group;\n"
       "      uint input_channel_base = group * in_channels_per_group;\n"
       "      for (uint c_local = 0u; c_local < in_channels_per_group; c_local++) {\n"
       "        uint c = input_channel_base + c_local;\n"
       "        for (uint r = 0u; r < p.filter_h; r++) {\n"
       "          int ih = int(oh) * int(p.stride_h) - int(p.pad_h) + int(r) * int(p.dilation_h);\n"
       "          if (ih < 0 || ih >= int(p.in_h)) {\n"
       "            continue;\n"
       "          }\n"
       "          uint filter_r = p.mode == 0u ? (p.filter_h - 1u - r) : r;\n"
       "          for (uint s = 0u; s < p.filter_w; s++) {\n"
       "            int iw = int(ow) * int(p.stride_w) - int(p.pad_w) + int(s) * int(p.dilation_w);\n"
       "            if (iw < 0 || iw >= int(p.in_w)) {\n"
       "              continue;\n"
       "            }\n"
       "            uint filter_s = p.mode == 0u ? (p.filter_w - 1u - s) : s;\n"
       "            ulong x_index = ((ulong(n) * ulong(p.in_channels) + ulong(c)) * ulong(p.in_h) + ulong(ih)) * ulong(p.in_w) + ulong(iw);\n"
       "            ulong w_index = ((ulong(k) * ulong(in_channels_per_group) + ulong(c_local)) * ulong(p.filter_h) + ulong(filter_r)) * ulong(p.filter_w) + ulong(filter_s);\n"
       "            sum += x[x_index] * w[w_index];\n"
       "          }\n"
       "        }\n"
       "      }\n"
       "    }\n"
       "    float fused = p.alpha1 * sum + (p.has_z != 0u ? p.alpha2 * z[tid] : 0.0f) + bias[k];\n"
       "    if (p.activation_mode == 1u) {\n"
       "      if (isnan(fused) || fused < 0.0f) {\n"
       "        fused = 0.0f;\n"
       "      }\n"
       "    }\n"
       "    out[tid] = fused;\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_convolution_backward_data_f32(\n"
       "    device const float *w [[buffer(0)]],\n"
       "    device const float *dy [[buffer(1)]],\n"
       "    device const float *dx_in [[buffer(2)]],\n"
       "    device float *out [[buffer(3)]],\n"
       "    constant float &alpha [[buffer(4)]],\n"
       "    constant float &beta [[buffer(5)]],\n"
       "    constant uint &mode [[buffer(6)]],\n"
       "    constant uint &groups [[buffer(7)]],\n"
       "    constant uint &batches [[buffer(8)]],\n"
       "    constant uint &in_channels [[buffer(9)]],\n"
       "    constant uint &in_h [[buffer(10)]],\n"
       "    constant uint &in_w [[buffer(11)]],\n"
       "    constant uint &out_channels [[buffer(12)]],\n"
       "    constant uint &out_h [[buffer(13)]],\n"
       "    constant uint &out_w [[buffer(14)]],\n"
       "    constant uint &filter_h [[buffer(15)]],\n"
       "    constant uint &filter_w [[buffer(16)]],\n"
       "    constant uint &pad_h [[buffer(17)]],\n"
       "    constant uint &pad_w [[buffer(18)]],\n"
       "    constant uint &stride_h [[buffer(19)]],\n"
       "    constant uint &stride_w [[buffer(20)]],\n"
       "    constant uint &dilation_h [[buffer(21)]],\n"
       "    constant uint &dilation_w [[buffer(22)]],\n"
       "    constant uint &total [[buffer(23)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < total) {\n"
       "    uint iw = tid % in_w;\n"
       "    uint tmp = tid / in_w;\n"
       "    uint ih = tmp % in_h;\n"
       "    tmp = tmp / in_h;\n"
       "    uint c = tmp % in_channels;\n"
       "    uint n = tmp / in_channels;\n"
       "    float sum = 0.0f;\n"
       "    if (n < batches) {\n"
       "      uint in_channels_per_group = in_channels / groups;\n"
       "      uint out_channels_per_group = out_channels / groups;\n"
       "      uint group = c / in_channels_per_group;\n"
       "      uint c_local = c % in_channels_per_group;\n"
       "      uint output_channel_base = group * out_channels_per_group;\n"
       "      for (uint k_local = 0u; k_local < out_channels_per_group; k_local++) {\n"
       "        uint k = output_channel_base + k_local;\n"
       "        for (uint r = 0u; r < filter_h; r++) {\n"
       "          int oh_num = int(ih) + int(pad_h) - int(r * dilation_h);\n"
       "          if (oh_num % int(stride_h) != 0) {\n"
       "            continue;\n"
       "          }\n"
       "          int oh = oh_num / int(stride_h);\n"
       "          if (oh < 0 || oh >= int(out_h)) {\n"
       "            continue;\n"
       "          }\n"
       "          uint filter_r = mode == 0u ? (filter_h - 1u - r) : r;\n"
       "          for (uint s = 0u; s < filter_w; s++) {\n"
       "            int ow_num = int(iw) + int(pad_w) - int(s * dilation_w);\n"
       "            if (ow_num % int(stride_w) != 0) {\n"
       "              continue;\n"
       "            }\n"
       "            int ow = ow_num / int(stride_w);\n"
       "            if (ow < 0 || ow >= int(out_w)) {\n"
       "              continue;\n"
       "            }\n"
       "            uint filter_s = mode == 0u ? (filter_w - 1u - s) : s;\n"
       "            uint dy_index = ((n * out_channels + k) * out_h + uint(oh)) * out_w + uint(ow);\n"
       "            uint w_index = ((k * in_channels_per_group + c_local) * filter_h + filter_r) * filter_w + filter_s;\n"
       "            sum += dy[dy_index] * w[w_index];\n"
       "          }\n"
       "        }\n"
       "      }\n"
       "    }\n"
	       "    float prior = beta != 0.0f ? dx_in[tid] : 0.0f;\n"
	       "    out[tid] = alpha * sum + beta * prior;\n"
	       "  }\n"
	       "}\n"
	       "kernel void psyche_cudnn_convolution_mpsgraph_prepare_weights_f32(\n"
	       "    device const float *w [[buffer(0)]],\n"
	       "    device float *graph_w [[buffer(1)]],\n"
	       "    constant uint &mode [[buffer(2)]],\n"
	       "    constant uint &out_channels [[buffer(3)]],\n"
	       "    constant uint &channels_per_group [[buffer(4)]],\n"
	       "    constant uint &filter_h [[buffer(5)]],\n"
	       "    constant uint &filter_w [[buffer(6)]],\n"
	       "    constant uint &total [[buffer(7)]],\n"
	       "    uint tid [[thread_position_in_grid]]) {\n"
	       "  if (tid < total) {\n"
	       "    uint s = tid % filter_w;\n"
	       "    uint tmp = tid / filter_w;\n"
	       "    uint r = tmp % filter_h;\n"
	       "    tmp = tmp / filter_h;\n"
	       "    uint c_local = tmp % channels_per_group;\n"
	       "    uint k = tmp / channels_per_group;\n"
	       "    if (k >= out_channels) {\n"
	       "      return;\n"
	       "    }\n"
	       "    /* Shared forward/backward-data MPSGraph weight staging: flip only logical R/S indices. */\n"
	       "    /* Dilation is represented in the MPSGraph descriptor and must not affect this tensor flip. */\n"
	       "    uint source_r = mode == 0u ? (filter_h - 1u - r) : r;\n"
	       "    uint source_s = mode == 0u ? (filter_w - 1u - s) : s;\n"
	       "    uint source_index = ((k * channels_per_group + c_local) * filter_h + source_r) * filter_w + source_s;\n"
	       "    graph_w[tid] = w[source_index];\n"
	       "  }\n"
	       "}\n"
	       "kernel void psyche_cudnn_convolution_mpsgraph_apply_f32(\n"
	       "    device const float *raw [[buffer(0)]],\n"
	       "    device const float *prior_in [[buffer(1)]],\n"
	       "    device float *out [[buffer(2)]],\n"
	       "    constant float &alpha [[buffer(3)]],\n"
	       "    constant float &beta [[buffer(4)]],\n"
	       "    constant uint &total [[buffer(5)]],\n"
	       "    uint tid [[thread_position_in_grid]]) {\n"
	       "  if (tid < total) {\n"
	       "    float prior = beta != 0.0f ? prior_in[tid] : 0.0f;\n"
	       "    out[tid] = alpha * raw[tid] + beta * prior;\n"
	       "  }\n"
	       "}\n"
	       "kernel void psyche_cudnn_convolution_bias_activation_mpsgraph_apply_f32(\n"
	       "    device const float *raw [[buffer(0)]],\n"
	       "    device const float *z [[buffer(1)]],\n"
	       "    device const float *bias [[buffer(2)]],\n"
	       "    device float *out [[buffer(3)]],\n"
	       "    constant float &alpha1 [[buffer(4)]],\n"
	       "    constant float &alpha2 [[buffer(5)]],\n"
	       "    constant uint &activation_mode [[buffer(6)]],\n"
	       "    constant uint &out_channels [[buffer(7)]],\n"
	       "    constant uint &out_h [[buffer(8)]],\n"
	       "    constant uint &out_w [[buffer(9)]],\n"
	       "    constant uint &has_z [[buffer(10)]],\n"
	       "    constant uint &total [[buffer(11)]],\n"
	       "    uint tid [[thread_position_in_grid]]) {\n"
	       "  if (tid < total) {\n"
	       "    ulong spatial = ulong(out_h) * ulong(out_w);\n"
	       "    uint channel = uint((ulong(tid) / spatial) % ulong(out_channels));\n"
	       "    float fused = alpha1 * raw[tid] + (has_z != 0u ? alpha2 * z[tid] : 0.0f) + bias[channel];\n"
	       "    if (activation_mode == 1u) {\n"
	       "      if (isnan(fused) || fused < 0.0f) {\n"
	       "        fused = 0.0f;\n"
	       "      }\n"
	       "    }\n"
	       "    out[tid] = fused;\n"
	       "  }\n"
	       "}\n"
	       "#define PSYCHE_CUDNN_BWD_FILTER_PARTIAL_THREADS "
	       PSYCHE_CUDA_METAL_STRINGIFY(PSYCHE_CUDA_METAL_BWD_FILTER_PARTIAL_THREADS_LITERAL)
	       "\n"
       "static inline float psyche_cudnn_bwd_filter_contribution_f32(\n"
       "    device const float *x,\n"
       "    device const float *dy,\n"
       "    uint mode,\n"
       "    uint groups,\n"
       "    uint in_channels,\n"
       "    uint in_h,\n"
       "    uint in_w,\n"
       "    uint out_channels,\n"
       "    uint out_h,\n"
       "    uint out_w,\n"
       "    uint filter_h,\n"
       "    uint filter_w,\n"
       "    uint pad_h,\n"
       "    uint pad_w,\n"
       "    uint stride_h,\n"
       "    uint stride_w,\n"
       "    uint dilation_h,\n"
       "    uint dilation_w,\n"
       "    uint dw_index,\n"
       "    uint reduction_index) {\n"
       "  uint s_phys = dw_index % filter_w;\n"
       "  uint tmp = dw_index / filter_w;\n"
       "    uint r_phys = tmp % filter_h;\n"
       "    tmp = tmp / filter_h;\n"
       "    uint in_channels_per_group = in_channels / groups;\n"
       "    uint c_local = tmp % in_channels_per_group;\n"
       "    uint k = tmp / in_channels_per_group;\n"
       "  if (k >= out_channels) {\n"
       "    return 0.0f;\n"
       "  }\n"
       "  uint out_channels_per_group = out_channels / groups;\n"
       "  uint group = k / out_channels_per_group;\n"
       "  uint c = group * in_channels_per_group + c_local;\n"
       "  uint ow = reduction_index % out_w;\n"
       "  uint red_tmp = reduction_index / out_w;\n"
       "  uint oh = red_tmp % out_h;\n"
       "  uint n = red_tmp / out_h;\n"
       "  uint r_tap = mode == 0u ? (filter_h - 1u - r_phys) : r_phys;\n"
       "  uint s_tap = mode == 0u ? (filter_w - 1u - s_phys) : s_phys;\n"
       "  int ih = int(oh * stride_h) - int(pad_h) + int(r_tap * dilation_h);\n"
       "  int iw = int(ow * stride_w) - int(pad_w) + int(s_tap * dilation_w);\n"
       "  if (ih < 0 || ih >= int(in_h) || iw < 0 || iw >= int(in_w)) {\n"
       "    return 0.0f;\n"
       "  }\n"
       "  uint x_index = ((n * in_channels + c) * in_h + uint(ih)) * in_w + uint(iw);\n"
       "  uint dy_index = ((n * out_channels + k) * out_h + oh) * out_w + ow;\n"
       "  return x[x_index] * dy[dy_index];\n"
       "}\n"
       "kernel void psyche_cudnn_convolution_backward_filter_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *dy [[buffer(1)]],\n"
       "    device const float *dw_in [[buffer(2)]],\n"
       "    device float *out [[buffer(3)]],\n"
       "    constant float &alpha [[buffer(4)]],\n"
       "    constant float &beta [[buffer(5)]],\n"
       "    constant uint &mode [[buffer(6)]],\n"
       "    constant uint &groups [[buffer(7)]],\n"
       "    constant uint &batches [[buffer(8)]],\n"
       "    constant uint &in_channels [[buffer(9)]],\n"
       "    constant uint &in_h [[buffer(10)]],\n"
       "    constant uint &in_w [[buffer(11)]],\n"
       "    constant uint &out_channels [[buffer(12)]],\n"
       "    constant uint &out_h [[buffer(13)]],\n"
       "    constant uint &out_w [[buffer(14)]],\n"
       "    constant uint &filter_h [[buffer(15)]],\n"
       "    constant uint &filter_w [[buffer(16)]],\n"
       "    constant uint &pad_h [[buffer(17)]],\n"
       "    constant uint &pad_w [[buffer(18)]],\n"
       "    constant uint &stride_h [[buffer(19)]],\n"
       "    constant uint &stride_w [[buffer(20)]],\n"
       "    constant uint &dilation_h [[buffer(21)]],\n"
       "    constant uint &dilation_w [[buffer(22)]],\n"
       "    constant uint &total [[buffer(23)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < total) {\n"
       "    uint reduction_steps = batches * out_h * out_w;\n"
       "    float sum = 0.0f;\n"
       "    for (uint reduction_index = 0u; reduction_index < reduction_steps; reduction_index++) {\n"
       "      sum += psyche_cudnn_bwd_filter_contribution_f32(\n"
       "          x, dy, mode, groups, in_channels, in_h, in_w, out_channels, out_h, out_w,\n"
       "          filter_h, filter_w, pad_h, pad_w, stride_h, stride_w, dilation_h, dilation_w,\n"
       "          tid, reduction_index);\n"
       "    }\n"
       "    float prior = beta != 0.0f ? dw_in[tid] : 0.0f;\n"
       "    out[tid] = alpha * sum + beta * prior;\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_convolution_backward_filter_partial_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *dy [[buffer(1)]],\n"
       "    device float *partials [[buffer(2)]],\n"
       "    constant uint &mode [[buffer(3)]],\n"
       "    constant uint &groups [[buffer(4)]],\n"
       "    constant uint &in_channels [[buffer(5)]],\n"
       "    constant uint &in_h [[buffer(6)]],\n"
       "    constant uint &in_w [[buffer(7)]],\n"
       "    constant uint &out_channels [[buffer(8)]],\n"
       "    constant uint &out_h [[buffer(9)]],\n"
       "    constant uint &out_w [[buffer(10)]],\n"
       "    constant uint &filter_h [[buffer(11)]],\n"
       "    constant uint &filter_w [[buffer(12)]],\n"
       "    constant uint &pad_h [[buffer(13)]],\n"
       "    constant uint &pad_w [[buffer(14)]],\n"
       "    constant uint &stride_h [[buffer(15)]],\n"
       "    constant uint &stride_w [[buffer(16)]],\n"
       "    constant uint &dilation_h [[buffer(17)]],\n"
       "    constant uint &dilation_w [[buffer(18)]],\n"
       "    constant uint &total [[buffer(19)]],\n"
       "    constant uint &reduction_steps [[buffer(20)]],\n"
       "    constant uint &chunk_span [[buffer(21)]],\n"
       "    uint lid [[thread_index_in_threadgroup]],\n"
       "    uint2 group_position [[threadgroup_position_in_grid]]) {\n"
       "  threadgroup float scratch[PSYCHE_CUDNN_BWD_FILTER_PARTIAL_THREADS];\n"
       "  uint dw_index = group_position.x;\n"
       "  uint chunk = group_position.y;\n"
       "  uint chunk_start = chunk * chunk_span;\n"
       "  uint chunk_end = min(chunk_start + chunk_span, reduction_steps);\n"
       "  float sum = 0.0f;\n"
       "  if (dw_index < total) {\n"
       "    for (uint reduction_index = chunk_start + lid; reduction_index < chunk_end; reduction_index += PSYCHE_CUDNN_BWD_FILTER_PARTIAL_THREADS) {\n"
       "      sum += psyche_cudnn_bwd_filter_contribution_f32(\n"
       "          x, dy, mode, groups, in_channels, in_h, in_w, out_channels, out_h, out_w,\n"
       "          filter_h, filter_w, pad_h, pad_w, stride_h, stride_w, dilation_h, dilation_w,\n"
       "          dw_index, reduction_index);\n"
       "    }\n"
       "  }\n"
       "  scratch[lid] = sum;\n"
       "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  for (uint stride = PSYCHE_CUDNN_BWD_FILTER_PARTIAL_THREADS >> 1u; stride > 0u; stride >>= 1) {\n"
       "    if (lid < stride) {\n"
       "      scratch[lid] += scratch[lid + stride];\n"
       "    }\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  }\n"
       "  if (lid == 0u && dw_index < total) {\n"
       "    partials[chunk * total + dw_index] = scratch[0];\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_convolution_backward_filter_reduce_f32(\n"
       "    device const float *partials [[buffer(0)]],\n"
       "    device const float *dw_in [[buffer(1)]],\n"
       "    device float *out [[buffer(2)]],\n"
       "    constant float &alpha [[buffer(3)]],\n"
       "    constant float &beta [[buffer(4)]],\n"
       "    constant uint &total [[buffer(5)]],\n"
       "    constant uint &chunks [[buffer(6)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < total) {\n"
       "    float sum = 0.0f;\n"
       "    for (uint chunk = 0u; chunk < chunks; chunk++) {\n"
       "      sum += partials[chunk * total + tid];\n"
       "    }\n"
       "    float prior = beta != 0.0f ? dw_in[tid] : 0.0f;\n"
       "    out[tid] = alpha * sum + beta * prior;\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_convolution_backward_filter_mpsgraph_apply_f32(\n"
       "    device const float *raw [[buffer(0)]],\n"
       "    device const float *dw_in [[buffer(1)]],\n"
       "    device float *out [[buffer(2)]],\n"
       "    constant float &alpha [[buffer(3)]],\n"
       "    constant float &beta [[buffer(4)]],\n"
       "    constant uint &mode [[buffer(5)]],\n"
       "    constant uint &out_channels [[buffer(6)]],\n"
       "    constant uint &channels_per_group [[buffer(7)]],\n"
       "    constant uint &filter_h [[buffer(8)]],\n"
       "    constant uint &filter_w [[buffer(9)]],\n"
       "    constant uint &total [[buffer(10)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid < total) {\n"
       "    uint s = tid % filter_w;\n"
       "    uint tmp = tid / filter_w;\n"
       "    uint r = tmp % filter_h;\n"
       "    tmp = tmp / filter_h;\n"
       "    uint c_local = tmp % channels_per_group;\n"
       "    uint k = tmp / channels_per_group;\n"
       "    if (k >= out_channels) {\n"
       "      return;\n"
       "    }\n"
       "    uint raw_r = mode == 0u ? (filter_h - 1u - r) : r;\n"
       "    uint raw_s = mode == 0u ? (filter_w - 1u - s) : s;\n"
       "    uint raw_index = ((k * channels_per_group + c_local) * filter_h + raw_r) * filter_w + raw_s;\n"
       "    float prior = beta != 0.0f ? dw_in[tid] : 0.0f;\n"
       "    out[tid] = alpha * raw[raw_index] + beta * prior;\n"
       "  }\n"
       "}\n"
       "static inline uint psyche_cudnn_softmax_offset(\n"
       "    uint mode,\n"
       "    uint channels,\n"
       "    uint height,\n"
       "    uint width,\n"
       "    uint vector,\n"
       "    uint lane) {\n"
       "  if (mode == 0u) {\n"
       "    return vector * channels * height * width + lane;\n"
       "  }\n"
       "  uint ow = vector % width;\n"
       "  uint tmp = vector / width;\n"
       "  uint oh = tmp % height;\n"
       "  uint batch = tmp / height;\n"
       "  return ((batch * channels + lane) * height + oh) * width + ow;\n"
       "}\n"
       "kernel void psyche_cudnn_softmax_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *y_in [[buffer(1)]],\n"
       "    device float *out [[buffer(2)]],\n"
       "    constant float &alpha [[buffer(3)]],\n"
       "    constant float &beta [[buffer(4)]],\n"
       "    constant uint &algorithm [[buffer(5)]],\n"
       "    constant uint &mode [[buffer(6)]],\n"
       "    constant uint &batches [[buffer(7)]],\n"
       "    constant uint &channels [[buffer(8)]],\n"
       "    constant uint &height [[buffer(9)]],\n"
       "    constant uint &width [[buffer(10)]],\n"
       "    constant uint &vector_count [[buffer(11)]],\n"
       "    constant uint &vector_len [[buffer(12)]],\n"
       "    uint lid [[thread_index_in_threadgroup]],\n"
       "    uint vector [[threadgroup_position_in_grid]]) {\n"
       "  (void)batches;\n"
       "  threadgroup float scratch_float[256];\n"
       "  threadgroup uint scratch_nan[256];\n"
       "  threadgroup uint scratch_inf[256];\n"
       "  float neg_inf = as_type<float>(0xff800000u);\n"
       "  float qnan = as_type<float>(0x7fc00000u);\n"
       "  float local_max = neg_inf;\n"
       "  float local_sum = 0.0f;\n"
       "  uint local_nan = 0u;\n"
       "  uint local_pos_inf = 0u;\n"
       "  if (vector >= vector_count) {\n"
       "    return;\n"
       "  }\n"
       "  for (uint lane = lid; lane < vector_len; lane += 256u) {\n"
       "    uint offset = psyche_cudnn_softmax_offset(mode, channels, height, width, vector, lane);\n"
       "    float value = x[offset];\n"
       "    if (isnan(value)) {\n"
       "      local_nan = 1u;\n"
       "    } else if (isinf(value) && value > 0.0f) {\n"
       "      local_pos_inf += 1u;\n"
       "    } else if (value > local_max) {\n"
       "      local_max = value;\n"
       "    }\n"
       "  }\n"
       "  scratch_float[lid] = local_max;\n"
       "  scratch_nan[lid] = local_nan;\n"
       "  scratch_inf[lid] = local_pos_inf;\n"
       "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  for (uint stride = 128u; stride > 0u; stride >>= 1u) {\n"
       "    if (lid < stride) {\n"
       "      if (scratch_float[lid + stride] > scratch_float[lid]) {\n"
       "        scratch_float[lid] = scratch_float[lid + stride];\n"
       "      }\n"
       "      scratch_nan[lid] |= scratch_nan[lid + stride];\n"
       "      scratch_inf[lid] += scratch_inf[lid + stride];\n"
       "    }\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  }\n"
       "  float group_max = scratch_float[0];\n"
       "  uint has_nan = scratch_nan[0];\n"
       "  uint positive_inf_count = scratch_inf[0];\n"
       "  if (has_nan == 0u && !(algorithm != 0u && positive_inf_count > 0u)) {\n"
       "    for (uint lane = lid; lane < vector_len; lane += 256u) {\n"
       "      uint offset = psyche_cudnn_softmax_offset(mode, channels, height, width, vector, lane);\n"
       "      float value = x[offset];\n"
       "      local_sum += algorithm == 0u ? exp(value) : exp(value - group_max);\n"
       "    }\n"
       "  }\n"
       "  scratch_float[lid] = local_sum;\n"
       "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  for (uint stride = 128u; stride > 0u; stride >>= 1u) {\n"
       "    if (lid < stride) {\n"
       "      scratch_float[lid] += scratch_float[lid + stride];\n"
       "    }\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  }\n"
       "  float sum = scratch_float[0];\n"
       "  for (uint lane = lid; lane < vector_len; lane += 256u) {\n"
       "    uint offset = psyche_cudnn_softmax_offset(mode, channels, height, width, vector, lane);\n"
       "    float value = x[offset];\n"
       "    float result = 0.0f;\n"
       "    if (has_nan != 0u) {\n"
       "      result = qnan;\n"
       "    } else if (algorithm != 0u && positive_inf_count > 0u) {\n"
       "      if (isinf(value) && value > 0.0f) {\n"
       "        result = algorithm == 2u ? -log(float(positive_inf_count)) : 1.0f / float(positive_inf_count);\n"
       "      } else {\n"
       "        result = algorithm == 2u ? neg_inf : 0.0f;\n"
       "      }\n"
       "    } else if (algorithm == 2u) {\n"
       "      result = value - group_max - log(sum);\n"
       "    } else {\n"
       "      float numerator = algorithm == 0u ? exp(value) : exp(value - group_max);\n"
       "      result = numerator / sum;\n"
       "    }\n"
       "    float prior = beta != 0.0f ? y_in[offset] : 0.0f;\n"
       "    out[offset] = alpha * result + beta * prior;\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_softmax_backward_f32(\n"
       "    device const float *y [[buffer(0)]],\n"
       "    device const float *dy [[buffer(1)]],\n"
       "    device const float *dx_in [[buffer(2)]],\n"
       "    device float *out [[buffer(3)]],\n"
       "    constant float &alpha [[buffer(4)]],\n"
       "    constant float &beta [[buffer(5)]],\n"
       "    constant uint &algorithm [[buffer(6)]],\n"
       "    constant uint &mode [[buffer(7)]],\n"
       "    constant uint &batches [[buffer(8)]],\n"
       "    constant uint &channels [[buffer(9)]],\n"
       "    constant uint &height [[buffer(10)]],\n"
       "    constant uint &width [[buffer(11)]],\n"
       "    constant uint &vector_count [[buffer(12)]],\n"
       "    constant uint &vector_len [[buffer(13)]],\n"
       "    uint lid [[thread_index_in_threadgroup]],\n"
       "    uint vector [[threadgroup_position_in_grid]]) {\n"
       "  (void)batches;\n"
       "  threadgroup float scratch_float[256];\n"
       "  threadgroup uint scratch_nan[256];\n"
       "  float qnan = as_type<float>(0x7fc00000u);\n"
       "  float local_sum = 0.0f;\n"
       "  uint local_nan = 0u;\n"
       "  if (vector >= vector_count) {\n"
       "    return;\n"
       "  }\n"
       "  for (uint lane = lid; lane < vector_len; lane += 256u) {\n"
       "    uint offset = psyche_cudnn_softmax_offset(mode, channels, height, width, vector, lane);\n"
       "    float y_value = y[offset];\n"
       "    float dy_value = dy[offset];\n"
       "    if (isnan(y_value) || isnan(dy_value)) {\n"
       "      local_nan = 1u;\n"
       "    } else {\n"
       "      local_sum += algorithm == 2u ? dy_value : y_value * dy_value;\n"
       "    }\n"
       "  }\n"
       "  scratch_float[lid] = local_sum;\n"
       "  scratch_nan[lid] = local_nan;\n"
       "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  for (uint stride = 128u; stride > 0u; stride >>= 1u) {\n"
       "    if (lid < stride) {\n"
       "      scratch_float[lid] += scratch_float[lid + stride];\n"
       "      scratch_nan[lid] |= scratch_nan[lid + stride];\n"
       "    }\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  }\n"
       "  float accum = scratch_float[0];\n"
       "  uint has_nan = scratch_nan[0] | (isnan(accum) ? 1u : 0u);\n"
       "  for (uint lane = lid; lane < vector_len; lane += 256u) {\n"
       "    uint offset = psyche_cudnn_softmax_offset(mode, channels, height, width, vector, lane);\n"
       "    float y_value = y[offset];\n"
       "    float dy_value = dy[offset];\n"
       "    float result = 0.0f;\n"
       "    if (has_nan != 0u) {\n"
       "      result = qnan;\n"
       "    } else if (algorithm == 2u) {\n"
       "      result = dy_value - exp(y_value) * accum;\n"
       "    } else {\n"
       "      result = y_value * (dy_value - accum);\n"
       "    }\n"
       "    float prior = beta != 0.0f ? dx_in[offset] : 0.0f;\n"
       "    out[offset] = alpha * result + beta * prior;\n"
       "  }\n"
       "}\n"
       "kernel void psyche_cudnn_pooling_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *y_in [[buffer(1)]],\n"
       "    device float *out [[buffer(2)]],\n"
       "    constant float &alpha [[buffer(3)]],\n"
       "    constant float &beta [[buffer(4)]],\n"
       "    constant uint &mode [[buffer(5)]],\n"
       "    constant uint &nan_opt [[buffer(6)]],\n"
       "    constant uint &batches [[buffer(7)]],\n"
       "    constant uint &channels [[buffer(8)]],\n"
       "    constant uint &in_h [[buffer(9)]],\n"
       "    constant uint &in_w [[buffer(10)]],\n"
       "    constant uint &out_h [[buffer(11)]],\n"
       "    constant uint &out_w [[buffer(12)]],\n"
       "    constant uint &window_h [[buffer(13)]],\n"
       "    constant uint &window_w [[buffer(14)]],\n"
       "    constant int &pad_h [[buffer(15)]],\n"
       "    constant int &pad_w [[buffer(16)]],\n"
       "    constant uint &stride_h [[buffer(17)]],\n"
       "    constant uint &stride_w [[buffer(18)]],\n"
       "    constant uint &total [[buffer(19)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  if (tid >= total) {\n"
       "    return;\n"
       "  }\n"
       "  uint ow = tid % out_w;\n"
       "  uint tmp = tid / out_w;\n"
       "  uint oh = tmp % out_h;\n"
       "  tmp = tmp / out_h;\n"
       "  uint channel = tmp % channels;\n"
       "  uint batch = tmp / channels;\n"
       "  if (batch >= batches) {\n"
       "    return;\n"
       "  }\n"
       "  int h_start = int(oh * stride_h) - pad_h;\n"
       "  int w_start = int(ow * stride_w) - pad_w;\n"
       "  bool is_average = mode == 1u || mode == 2u;\n"
       "  float best = -3.4028234663852886e38f;\n"
       "  float sum = 0.0f;\n"
       "  uint valid_count = 0u;\n"
       "  bool found_nan = false;\n"
       "  for (uint kh = 0u; kh < window_h && !found_nan; kh++) {\n"
       "    int ih = h_start + int(kh);\n"
       "    if (ih < 0 || ih >= int(in_h)) {\n"
       "      continue;\n"
       "    }\n"
       "    for (uint kw = 0u; kw < window_w; kw++) {\n"
       "      int iw = w_start + int(kw);\n"
       "      if (iw < 0 || iw >= int(in_w)) {\n"
       "        continue;\n"
       "      }\n"
       "      valid_count += 1u;\n"
       "      uint index = (((batch * channels + channel) * in_h + uint(ih)) * in_w + uint(iw));\n"
       "      float value = x[index];\n"
       "      if (isnan(value)) {\n"
       "        if (nan_opt == 1u) {\n"
       "          best = value;\n"
       "          found_nan = true;\n"
       "          break;\n"
       "        }\n"
       "        // NOT_PROPAGATE_NAN drops NaN from the sum; padding alone controls the denominator.\n"
       "        continue;\n"
       "      }\n"
       "      if (is_average) {\n"
       "        sum += value;\n"
       "        continue;\n"
       "      }\n"
       "      if (value > best) {\n"
       "        best = value;\n"
       "      }\n"
       "    }\n"
       "  }\n"
       "  float pooled = best;\n"
       "  if (!found_nan && mode == 1u) {\n"
       "    pooled = sum / float(window_h * window_w);\n"
       "  } else if (!found_nan && mode == 2u) {\n"
       "    pooled = valid_count == 0u ? 0.0f : sum / float(valid_count);\n"
       "  }\n"
       "  float prior = beta != 0.0f ? y_in[tid] : 0.0f;\n"
       "  out[tid] = alpha * pooled + beta * prior;\n"
       "}\n"
       "static inline bool psyche_cudnn_pooling_window_selects_input(\n"
       "    device const float *x,\n"
       "    uint batch,\n"
       "    uint channel,\n"
       "    uint target_h,\n"
       "    uint target_w,\n"
       "    uint oh,\n"
       "    uint ow,\n"
       "    uint channels,\n"
       "    uint in_h,\n"
       "    uint in_w,\n"
       "    uint window_h,\n"
       "    uint window_w,\n"
       "    int pad_h,\n"
       "    int pad_w,\n"
       "    uint stride_h,\n"
       "    uint stride_w,\n"
       "    uint nan_opt) {\n"
       "  int h_start = int(oh * stride_h) - pad_h;\n"
       "  int w_start = int(ow * stride_w) - pad_w;\n"
       "  bool found = false;\n"
       "  uint best_h = 0u;\n"
       "  uint best_w = 0u;\n"
       "  float best = -3.4028234663852886e38f;\n"
       "  for (uint kh = 0u; kh < window_h; kh++) {\n"
       "    int ih = h_start + int(kh);\n"
       "    if (ih < 0 || ih >= int(in_h)) {\n"
       "      continue;\n"
       "    }\n"
       "    for (uint kw = 0u; kw < window_w; kw++) {\n"
       "      int iw = w_start + int(kw);\n"
       "      if (iw < 0 || iw >= int(in_w)) {\n"
       "        continue;\n"
       "      }\n"
       "      uint input_index = (((batch * channels + channel) * in_h + uint(ih)) * in_w + uint(iw));\n"
       "      float value = x[input_index];\n"
       "      if (isnan(value)) {\n"
       "        if (nan_opt == 1u) {\n"
       "          return uint(ih) == target_h && uint(iw) == target_w;\n"
       "        }\n"
       "        continue;\n"
       "      }\n"
       "      if (!found || value > best) {\n"
       "        found = true;\n"
       "        best = value;\n"
       "        best_h = uint(ih);\n"
       "        best_w = uint(iw);\n"
       "      }\n"
       "    }\n"
       "  }\n"
       "  return found && best_h == target_h && best_w == target_w;\n"
       "}\n"
       "static inline uint psyche_cudnn_pooling_valid_count(\n"
       "    uint oh,\n"
       "    uint ow,\n"
       "    uint in_h,\n"
       "    uint in_w,\n"
       "    uint window_h,\n"
       "    uint window_w,\n"
       "    int pad_h,\n"
       "    int pad_w,\n"
       "    uint stride_h,\n"
       "    uint stride_w) {\n"
       "  int h_start = int(oh * stride_h) - pad_h;\n"
       "  int w_start = int(ow * stride_w) - pad_w;\n"
       "  uint count = 0u;\n"
       "  for (uint kh = 0u; kh < window_h; kh++) {\n"
       "    int ih = h_start + int(kh);\n"
       "    if (ih < 0 || ih >= int(in_h)) {\n"
       "      continue;\n"
       "    }\n"
       "    for (uint kw = 0u; kw < window_w; kw++) {\n"
       "      int iw = w_start + int(kw);\n"
       "      if (iw >= 0 && iw < int(in_w)) {\n"
       "        count += 1u;\n"
       "      }\n"
       "    }\n"
       "  }\n"
       "  return count;\n"
       "}\n"
       "kernel void psyche_cudnn_pooling_backward_f32(\n"
       "    device const float *x [[buffer(0)]],\n"
       "    device const float *dy [[buffer(1)]],\n"
       "    device const float *dx_in [[buffer(2)]],\n"
       "    device float *out [[buffer(3)]],\n"
       "    constant float &alpha [[buffer(4)]],\n"
       "    constant float &beta [[buffer(5)]],\n"
       "    constant uint &mode [[buffer(6)]],\n"
       "    constant uint &nan_opt [[buffer(7)]],\n"
       "    constant uint &batches [[buffer(8)]],\n"
       "    constant uint &channels [[buffer(9)]],\n"
       "    constant uint &in_h [[buffer(10)]],\n"
       "    constant uint &in_w [[buffer(11)]],\n"
       "    constant uint &out_h [[buffer(12)]],\n"
       "    constant uint &out_w [[buffer(13)]],\n"
       "    constant uint &window_h [[buffer(14)]],\n"
       "    constant uint &window_w [[buffer(15)]],\n"
       "    constant int &pad_h [[buffer(16)]],\n"
       "    constant int &pad_w [[buffer(17)]],\n"
       "    constant uint &stride_h [[buffer(18)]],\n"
       "    constant uint &stride_w [[buffer(19)]],\n"
       "    constant uint &total [[buffer(20)]],\n"
       "    uint tid [[thread_position_in_grid]]) {\n"
       "  (void)batches;\n"
       "  if (tid >= total) {\n"
       "    return;\n"
       "  }\n"
       "  uint iw = tid % in_w;\n"
       "  uint tmp = tid / in_w;\n"
       "  uint ih = tmp % in_h;\n"
       "  tmp = tmp / in_h;\n"
       "  uint channel = tmp % channels;\n"
       "  uint batch = tmp / channels;\n"
       "  bool is_average = mode == 1u || mode == 2u;\n"
       "  float grad = 0.0f;\n"
       "  for (uint oh = 0u; oh < out_h; oh++) {\n"
       "    int h_start = int(oh * stride_h) - pad_h;\n"
       "    if (int(ih) < h_start || int(ih) >= h_start + int(window_h)) {\n"
       "      continue;\n"
       "    }\n"
       "    for (uint ow = 0u; ow < out_w; ow++) {\n"
       "      int w_start = int(ow * stride_w) - pad_w;\n"
       "      if (int(iw) < w_start || int(iw) >= w_start + int(window_w)) {\n"
       "        continue;\n"
       "      }\n"
       "      uint out_index = (((batch * channels + channel) * out_h + oh) * out_w + ow);\n"
       "      if (is_average) {\n"
       "        uint denominator = window_h * window_w;\n"
       "        if (mode == 2u) {\n"
       "          denominator = psyche_cudnn_pooling_valid_count(\n"
       "              oh, ow, in_h, in_w, window_h, window_w, pad_h, pad_w, stride_h, stride_w);\n"
       "        }\n"
       "        if (denominator > 0u) {\n"
       "          grad += dy[out_index] / float(denominator);\n"
       "        }\n"
       "      } else if (psyche_cudnn_pooling_window_selects_input(\n"
       "          x,\n"
       "          batch,\n"
       "          channel,\n"
       "          ih,\n"
       "          iw,\n"
       "          oh,\n"
       "          ow,\n"
       "          channels,\n"
       "          in_h,\n"
       "          in_w,\n"
       "          window_h,\n"
       "          window_w,\n"
       "          pad_h,\n"
       "          pad_w,\n"
       "          stride_h,\n"
       "          stride_w,\n"
       "          nan_opt)) {\n"
       "        grad += dy[out_index];\n"
       "      }\n"
       "    }\n"
       "  }\n"
       "  float prior = beta != 0.0f ? dx_in[tid] : 0.0f;\n"
       "  out[tid] = alpha * grad + beta * prior;\n"
       "}\n";
  NSError *error = nil;
  id<MTLLibrary> library = nil;
  id<MTLFunction> function = nil;
  if (
      getenv("PSYCHE_CUDA_COMPAT_METAL_DISABLE_BACKEND_FOR_TEST") != 0 &&
      strcasecmp(getenv("PSYCHE_CUDA_COMPAT_METAL_DISABLE_BACKEND_FOR_TEST"), "0") != 0) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  if (psyche_cuda_metal_initialized) {
    return psyche_cuda_metal_init_result;
  }
  psyche_cuda_metal_initialized = 1;
  psyche_cuda_metal_device = MTLCreateSystemDefaultDevice();
  if (psyche_cuda_metal_device == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  psyche_cuda_metal_queue = [psyche_cuda_metal_device newCommandQueue];
  if (psyche_cuda_metal_queue == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_OUT_OF_MEMORY;
    return psyche_cuda_metal_init_result;
  }
  library = [psyche_cuda_metal_device newLibraryWithSource:source options:nil error:&error];
  if (library == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_vector_add_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_vector_add_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_vector_add_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_saxpy_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_saxpy_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_saxpy_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_scale_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_scale_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_scale_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_copy_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_copy_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_copy_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_dot_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_dot_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_dot_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_sum_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_sum_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_sum_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_abs_sum_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_abs_sum_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_abs_sum_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_nrm2_pair_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_nrm2_pair_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_nrm2_pair_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_nrm2_combine_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_nrm2_combine_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_nrm2_combine_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_sgemv_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_sgemv_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_sgemv_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_sger_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_sger_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_sger_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cusparse_spmv_csr_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cusparse_spmv_csr_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cusparse_spmv_csr_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cusparse_spmm_csr_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cusparse_spmm_csr_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cusparse_spmm_csr_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_axpby_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_axpby_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_axpby_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_activation_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_activation_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_activation_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_activation_backward_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_activation_backward_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_activation_backward_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_transform_tensor_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_transform_tensor_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_transform_tensor_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_add_tensor_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_add_tensor_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_add_tensor_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_batchnorm_inference_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_batchnorm_inference_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_batchnorm_inference_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_convolution_forward_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_convolution_forward_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_convolution_forward_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_convolution_bias_activation_forward_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_convolution_bias_activation_forward_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_convolution_bias_activation_forward_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_convolution_backward_data_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_convolution_backward_data_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_convolution_backward_data_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_convolution_backward_filter_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_convolution_backward_filter_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_convolution_backward_filter_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_convolution_backward_filter_partial_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_convolution_backward_filter_partial_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_convolution_backward_filter_partial_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_convolution_backward_filter_reduce_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_convolution_backward_filter_reduce_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
	  if (psyche_cuda_metal_cudnn_convolution_backward_filter_reduce_f32 == nil) {
	    (void)error;
	    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
	    return psyche_cuda_metal_init_result;
	  }
	  function = [library newFunctionWithName:@"psyche_cudnn_convolution_mpsgraph_prepare_weights_f32"];
	  if (function == nil) {
	    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
	    return psyche_cuda_metal_init_result;
	  }
	  error = nil;
	  psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32 =
	      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
	  if (psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32 == nil) {
	    (void)error;
	    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
	    return psyche_cuda_metal_init_result;
	  }
	  function = [library newFunctionWithName:@"psyche_cudnn_convolution_mpsgraph_apply_f32"];
	  if (function == nil) {
	    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
	    return psyche_cuda_metal_init_result;
	  }
	  error = nil;
	  psyche_cuda_metal_cudnn_convolution_mpsgraph_apply_f32 =
	      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
	  if (psyche_cuda_metal_cudnn_convolution_mpsgraph_apply_f32 == nil) {
	    (void)error;
	    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
	    return psyche_cuda_metal_init_result;
	  }
	  function = [library newFunctionWithName:@"psyche_cudnn_convolution_bias_activation_mpsgraph_apply_f32"];
	  if (function == nil) {
	    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
	    return psyche_cuda_metal_init_result;
	  }
	  error = nil;
	  psyche_cuda_metal_cudnn_convolution_bias_activation_mpsgraph_apply_f32 =
	      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
	  if (psyche_cuda_metal_cudnn_convolution_bias_activation_mpsgraph_apply_f32 == nil) {
	    (void)error;
	    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
	    return psyche_cuda_metal_init_result;
	  }
	  function = [library newFunctionWithName:@"psyche_cudnn_convolution_backward_filter_mpsgraph_apply_f32"];
	  if (function == nil) {
	    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_convolution_backward_filter_mpsgraph_apply_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_convolution_backward_filter_mpsgraph_apply_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_pooling_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_pooling_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_pooling_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_pooling_backward_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_pooling_backward_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_pooling_backward_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_softmax_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_softmax_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_softmax_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  function = [library newFunctionWithName:@"psyche_cudnn_softmax_backward_f32"];
  if (function == nil) {
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  error = nil;
  psyche_cuda_metal_cudnn_softmax_backward_f32 =
      [psyche_cuda_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (psyche_cuda_metal_cudnn_softmax_backward_f32 == nil) {
    (void)error;
    psyche_cuda_metal_init_result = CUDA_ERROR_NOT_SUPPORTED;
    return psyche_cuda_metal_init_result;
  }
  psyche_cuda_metal_init_result = CUDA_SUCCESS;
  return psyche_cuda_metal_init_result;
}

CUresult psyche_cuda_metal_launch_vector_add_f32(
    const float *a,
    const float *b,
    float *out,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> a_buffer = nil;
  id<MTLBuffer> b_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLBuffer> n_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (a == 0 || b == 0 || out == 0 || n == 0 || bytes == 0 || gridDimX == 0 || blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (result == CUDA_SUCCESS && threads_per_group > psyche_cuda_metal_vector_add_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    a_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:(const void *)(uintptr_t)a
                    length:bytes
                   options:MTLResourceStorageModeShared];
    b_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:(const void *)(uintptr_t)b
                    length:bytes
                   options:MTLResourceStorageModeShared];
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:bytes
                    options:MTLResourceStorageModeShared];
    n_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&n
                    length:sizeof(n)
                   options:MTLResourceStorageModeShared];
    if (a_buffer == nil || b_buffer == nil || out_buffer == nil || n_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_vector_add_f32];
    [encoder setBuffer:a_buffer offset:0 atIndex:0];
    [encoder setBuffer:b_buffer offset:0 atIndex:1];
    [encoder setBuffer:out_buffer offset:0 atIndex:2];
    [encoder setBuffer:n_buffer offset:0 atIndex:3];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_saxpy_f32(
    const float *x,
    float *y,
    float alpha,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> alpha_buffer = nil;
  id<MTLBuffer> n_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (x == 0 || y == 0 || n == 0 || bytes == 0 || gridDimX == 0 || blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (result == CUDA_SUCCESS && threads_per_group > psyche_cuda_metal_saxpy_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:bytes
                   options:MTLResourceStorageModeShared];
    y_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:y
                    length:bytes
                   options:MTLResourceStorageModeShared];
    alpha_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&alpha
                    length:sizeof(alpha)
                   options:MTLResourceStorageModeShared];
    n_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&n
                    length:sizeof(n)
                   options:MTLResourceStorageModeShared];
    if (x_buffer == nil || y_buffer == nil || alpha_buffer == nil || n_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_saxpy_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:y_buffer offset:0 atIndex:1];
    [encoder setBuffer:alpha_buffer offset:0 atIndex:2];
    [encoder setBuffer:n_buffer offset:0 atIndex:3];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(y, [y_buffer contents], bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_scale_f32(
    float *x,
    float alpha,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> alpha_buffer = nil;
  id<MTLBuffer> n_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (x == 0 || n == 0 || bytes == 0 || gridDimX == 0 || blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (result == CUDA_SUCCESS && threads_per_group > psyche_cuda_metal_scale_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:bytes
                   options:MTLResourceStorageModeShared];
    alpha_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&alpha
                    length:sizeof(alpha)
                   options:MTLResourceStorageModeShared];
    n_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&n
                    length:sizeof(n)
                   options:MTLResourceStorageModeShared];
    if (x_buffer == nil || alpha_buffer == nil || n_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_scale_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:alpha_buffer offset:0 atIndex:1];
    [encoder setBuffer:n_buffer offset:0 atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(x, [x_buffer contents], bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_copy_f32(
    const float *x,
    float *y,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> n_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (x == 0 || y == 0 || n == 0 || bytes == 0 || gridDimX == 0 || blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (result == CUDA_SUCCESS && threads_per_group > psyche_cuda_metal_copy_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:bytes
                   options:MTLResourceStorageModeShared];
    y_buffer = [psyche_cuda_metal_device
        newBufferWithLength:bytes
                    options:MTLResourceStorageModeShared];
    n_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&n
                    length:sizeof(n)
                   options:MTLResourceStorageModeShared];
    if (x_buffer == nil || y_buffer == nil || n_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_copy_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:y_buffer offset:0 atIndex:1];
    [encoder setBuffer:n_buffer offset:0 atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(y, [y_buffer contents], bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cudnn_activation_f32(
    const float *x,
    const float *y_in,
    float *out,
    float alpha,
    float beta,
    unsigned int mode,
    unsigned int nan_opt,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLBuffer> alpha_buffer = nil;
  id<MTLBuffer> beta_buffer = nil;
  id<MTLBuffer> mode_buffer = nil;
  id<MTLBuffer> nan_opt_buffer = nil;
  id<MTLBuffer> n_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (
        x == 0 ||
        y_in == 0 ||
        out == 0 ||
        n == 0 ||
        bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)n > SIZE_MAX / sizeof(float) || bytes != (size_t)n * sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_activation_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    /*
     * newBufferWithBytes copies from the host pointer before the command buffer
     * is submitted, so x and old y are captured before the output copy-back can
     * mutate an exact in-place x == y caller buffer.
     */
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:y_in
                      length:bytes
                     options:MTLResourceStorageModeShared];
    } else {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithLength:bytes
                      options:MTLResourceStorageModeShared];
    }
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:bytes
                    options:MTLResourceStorageModeShared];
    alpha_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&alpha
                    length:sizeof(alpha)
                   options:MTLResourceStorageModeShared];
    beta_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&beta
                    length:sizeof(beta)
                   options:MTLResourceStorageModeShared];
    mode_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&mode
                    length:sizeof(mode)
                   options:MTLResourceStorageModeShared];
    nan_opt_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&nan_opt
                    length:sizeof(nan_opt)
                   options:MTLResourceStorageModeShared];
    n_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&n
                    length:sizeof(n)
                   options:MTLResourceStorageModeShared];
    if (
        x_buffer == nil ||
        y_buffer == nil ||
        out_buffer == nil ||
        alpha_buffer == nil ||
        beta_buffer == nil ||
        mode_buffer == nil ||
        nan_opt_buffer == nil ||
        n_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_activation_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:y_buffer offset:0 atIndex:1];
    [encoder setBuffer:out_buffer offset:0 atIndex:2];
    [encoder setBuffer:alpha_buffer offset:0 atIndex:3];
    [encoder setBuffer:beta_buffer offset:0 atIndex:4];
    [encoder setBuffer:mode_buffer offset:0 atIndex:5];
    [encoder setBuffer:nan_opt_buffer offset:0 atIndex:6];
    [encoder setBuffer:n_buffer offset:0 atIndex:7];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cudnn_activation_backward_f32(
    const float *x,
    const float *dy,
    const float *dx_in,
    float *out,
    float alpha,
    float beta,
    unsigned int mode,
    unsigned int nan_opt,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> dy_buffer = nil;
  id<MTLBuffer> dx_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (
        x == 0 ||
        dy == 0 ||
        dx_in == 0 ||
        out == 0 ||
        mode > 2U ||
        nan_opt > 1U ||
        n == 0 ||
        bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)n > SIZE_MAX / sizeof(float) || bytes != (size_t)n * sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_activation_backward_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:bytes
                   options:MTLResourceStorageModeShared];
    dy_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:dy
                    length:bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      dx_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:dx_in
                      length:bytes
                     options:MTLResourceStorageModeShared];
    } else {
      dx_buffer = [psyche_cuda_metal_device
          newBufferWithLength:bytes
                      options:MTLResourceStorageModeShared];
    }
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:bytes
                    options:MTLResourceStorageModeShared];
    if (x_buffer == nil || dy_buffer == nil || dx_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_activation_backward_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:dy_buffer offset:0 atIndex:1];
    [encoder setBuffer:dx_buffer offset:0 atIndex:2];
    [encoder setBuffer:out_buffer offset:0 atIndex:3];
    [encoder setBytes:&alpha length:sizeof(alpha) atIndex:4];
    [encoder setBytes:&beta length:sizeof(beta) atIndex:5];
    [encoder setBytes:&mode length:sizeof(mode) atIndex:6];
    [encoder setBytes:&nan_opt length:sizeof(nan_opt) atIndex:7];
    [encoder setBytes:&n length:sizeof(n) atIndex:8];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cudnn_transform_tensor_f32(
    const float *x,
    const float *y_in,
    float *out,
    float alpha,
    float beta,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (
        x == 0 ||
        y_in == 0 ||
        out == 0 ||
        n == 0 ||
        bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)n > SIZE_MAX / sizeof(float) || bytes != (size_t)n * sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_transform_tensor_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:y_in
                      length:bytes
                     options:MTLResourceStorageModeShared];
    } else {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithLength:bytes
                      options:MTLResourceStorageModeShared];
    }
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:bytes
                    options:MTLResourceStorageModeShared];
    if (x_buffer == nil || y_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_transform_tensor_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:y_buffer offset:0 atIndex:1];
    [encoder setBuffer:out_buffer offset:0 atIndex:2];
    [encoder setBytes:&alpha length:sizeof(alpha) atIndex:3];
    [encoder setBytes:&beta length:sizeof(beta) atIndex:4];
    [encoder setBytes:&n length:sizeof(n) atIndex:5];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cudnn_add_tensor_f32(
    const float *a,
    const float *c_in,
    float *out,
    float alpha,
    float beta,
    unsigned int a_n,
    unsigned int a_c,
    unsigned int a_h,
    unsigned int a_w,
    unsigned int c_n,
    unsigned int c_c,
    unsigned int c_h,
    unsigned int c_w,
    unsigned int total,
    size_t a_bytes,
    size_t c_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> a_buffer = nil;
  id<MTLBuffer> c_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  size_t a_total = 0;
  size_t c_total = 0;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (
        a == 0 ||
        c_in == 0 ||
        out == 0 ||
        a_n == 0 ||
        a_c == 0 ||
        a_h == 0 ||
        a_w == 0 ||
        c_n == 0 ||
        c_c == 0 ||
        c_h == 0 ||
        c_w == 0 ||
        total == 0 ||
        a_bytes == 0 ||
        c_bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (
        (size_t)a_n > SIZE_MAX / (size_t)a_c ||
        (size_t)a_n * (size_t)a_c > SIZE_MAX / (size_t)a_h ||
        (size_t)a_n * (size_t)a_c * (size_t)a_h > SIZE_MAX / (size_t)a_w ||
        (size_t)c_n > SIZE_MAX / (size_t)c_c ||
        (size_t)c_n * (size_t)c_c > SIZE_MAX / (size_t)c_h ||
        (size_t)c_n * (size_t)c_c * (size_t)c_h > SIZE_MAX / (size_t)c_w) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    a_total = (size_t)a_n * (size_t)a_c * (size_t)a_h * (size_t)a_w;
    c_total = (size_t)c_n * (size_t)c_c * (size_t)c_h * (size_t)c_w;
    if (
        a_total > SIZE_MAX / sizeof(float) ||
        c_total > SIZE_MAX / sizeof(float) ||
        a_bytes != a_total * sizeof(float) ||
        c_bytes != c_total * sizeof(float) ||
        (size_t)total != c_total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_add_tensor_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    a_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:a
                    length:a_bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      c_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:c_in
                      length:c_bytes
                     options:MTLResourceStorageModeShared];
    } else {
      c_buffer = [psyche_cuda_metal_device
          newBufferWithLength:c_bytes
                      options:MTLResourceStorageModeShared];
    }
    /* Keep output separate from caller C storage until the command buffer has completed successfully. */
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:c_bytes
                    options:MTLResourceStorageModeShared];
    if (a_buffer == nil || c_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_add_tensor_f32];
    [encoder setBuffer:a_buffer offset:0 atIndex:0];
    [encoder setBuffer:c_buffer offset:0 atIndex:1];
    [encoder setBuffer:out_buffer offset:0 atIndex:2];
    [encoder setBytes:&alpha length:sizeof(alpha) atIndex:3];
    [encoder setBytes:&beta length:sizeof(beta) atIndex:4];
    [encoder setBytes:&a_n length:sizeof(a_n) atIndex:5];
    [encoder setBytes:&a_c length:sizeof(a_c) atIndex:6];
    [encoder setBytes:&a_h length:sizeof(a_h) atIndex:7];
    [encoder setBytes:&a_w length:sizeof(a_w) atIndex:8];
    [encoder setBytes:&c_n length:sizeof(c_n) atIndex:9];
    [encoder setBytes:&c_c length:sizeof(c_c) atIndex:10];
    [encoder setBytes:&c_h length:sizeof(c_h) atIndex:11];
    [encoder setBytes:&c_w length:sizeof(c_w) atIndex:12];
    [encoder setBytes:&total length:sizeof(total) atIndex:13];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], c_bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cudnn_batchnorm_inference_f32(
    const float *x,
    const float *y_in,
    float *out,
    const float *scale,
    const float *bias,
    const float *mean,
    const float *variance,
    float alpha,
    float beta,
    float epsilon,
    unsigned int mode,
    unsigned int channels,
    unsigned int height,
    unsigned int width,
    unsigned int total,
    size_t tensor_bytes,
    size_t param_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLBuffer> scale_buffer = nil;
  id<MTLBuffer> bias_buffer = nil;
  id<MTLBuffer> mean_buffer = nil;
  id<MTLBuffer> variance_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (
        x == 0 ||
        y_in == 0 ||
        out == 0 ||
        scale == 0 ||
        bias == 0 ||
        mean == 0 ||
        variance == 0 ||
        mode > 1U ||
        channels == 0 ||
        height == 0 ||
        width == 0 ||
        total == 0 ||
        tensor_bytes == 0 ||
        param_bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)channels > SIZE_MAX / (size_t)height) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)channels * (size_t)height > SIZE_MAX / (size_t)width) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)total > SIZE_MAX / sizeof(float) || tensor_bytes != (size_t)total * sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (mode == (unsigned int)CUDNN_BATCHNORM_SPATIAL) {
      if (param_bytes != (size_t)channels * sizeof(float)) {
        return CUDA_ERROR_INVALID_VALUE;
      }
    } else if (param_bytes != (size_t)channels * (size_t)height * (size_t)width * sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_batchnorm_inference_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:tensor_bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:y_in
                      length:tensor_bytes
                     options:MTLResourceStorageModeShared];
    } else {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithLength:tensor_bytes
                      options:MTLResourceStorageModeShared];
    }
    scale_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:scale
                    length:param_bytes
                   options:MTLResourceStorageModeShared];
    bias_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:bias
                    length:param_bytes
                   options:MTLResourceStorageModeShared];
    mean_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:mean
                    length:param_bytes
                   options:MTLResourceStorageModeShared];
    variance_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:variance
                    length:param_bytes
                   options:MTLResourceStorageModeShared];
    /* Keep output separate from caller y storage until the command buffer has completed successfully. */
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:tensor_bytes
                    options:MTLResourceStorageModeShared];
    if (
        x_buffer == nil ||
        y_buffer == nil ||
        out_buffer == nil ||
        scale_buffer == nil ||
        bias_buffer == nil ||
        mean_buffer == nil ||
        variance_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_batchnorm_inference_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:y_buffer offset:0 atIndex:1];
    [encoder setBuffer:out_buffer offset:0 atIndex:2];
    [encoder setBuffer:scale_buffer offset:0 atIndex:3];
    [encoder setBuffer:bias_buffer offset:0 atIndex:4];
    [encoder setBuffer:mean_buffer offset:0 atIndex:5];
    [encoder setBuffer:variance_buffer offset:0 atIndex:6];
    [encoder setBytes:&alpha length:sizeof(alpha) atIndex:7];
    [encoder setBytes:&beta length:sizeof(beta) atIndex:8];
    [encoder setBytes:&epsilon length:sizeof(epsilon) atIndex:9];
    [encoder setBytes:&mode length:sizeof(mode) atIndex:10];
    [encoder setBytes:&channels length:sizeof(channels) atIndex:11];
    [encoder setBytes:&height length:sizeof(height) atIndex:12];
    [encoder setBytes:&width length:sizeof(width) atIndex:13];
    [encoder setBytes:&total length:sizeof(total) atIndex:14];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], tensor_bytes);
  }
	  return CUDA_SUCCESS;
	}

	static int psyche_cuda_metal_cudnn_mpsgraph_enabled(void);
	static int psyche_cuda_metal_cudnn_mpsgraph_required(void);
	static CUresult psyche_cuda_metal_launch_cudnn_convolution_forward_mpsgraph_f32(
	    const float *x,
	    const float *w,
	    const float *y_in,
	    float *out,
	    float alpha,
	    float beta,
	    unsigned int mode,
	    unsigned int groups,
	    unsigned int n,
	    unsigned int in_c,
	    unsigned int in_h,
	    unsigned int in_w,
	    unsigned int out_c,
	    unsigned int out_h,
	    unsigned int out_w,
	    unsigned int filter_h,
	    unsigned int filter_w,
	    unsigned int pad_h,
	    unsigned int pad_w,
	    unsigned int stride_h,
	    unsigned int stride_w,
	    unsigned int dilation_h,
	    unsigned int dilation_w,
	    unsigned int total,
	    size_t x_bytes,
	    size_t w_bytes,
	    size_t y_bytes,
	    unsigned int gridDimX,
	    unsigned int blockDimX,
	    int required);
	static CUresult psyche_cuda_metal_launch_cudnn_convolution_bias_activation_forward_mpsgraph_f32(
	    const float *x,
	    const float *w,
	    const float *z,
	    const float *bias,
	    float *out,
	    float alpha1,
	    float alpha2,
	    unsigned int activation_mode,
	    unsigned int mode,
	    unsigned int groups,
	    unsigned int n,
	    unsigned int in_c,
	    unsigned int in_h,
	    unsigned int in_w,
	    unsigned int out_c,
	    unsigned int out_h,
	    unsigned int out_w,
	    unsigned int filter_h,
	    unsigned int filter_w,
	    unsigned int pad_h,
	    unsigned int pad_w,
	    unsigned int stride_h,
	    unsigned int stride_w,
	    unsigned int dilation_h,
	    unsigned int dilation_w,
	    unsigned int total,
	    size_t x_bytes,
	    size_t w_bytes,
	    size_t z_bytes,
	    size_t bias_bytes,
	    size_t y_bytes,
	    unsigned int gridDimX,
	    unsigned int blockDimX,
	    int required);
	static CUresult psyche_cuda_metal_launch_cudnn_convolution_backward_data_mpsgraph_f32(
	    const float *w,
	    const float *dy,
	    const float *dx_in,
	    float *out,
	    float alpha,
	    float beta,
	    unsigned int mode,
	    unsigned int groups,
	    unsigned int n,
	    unsigned int in_c,
	    unsigned int in_h,
	    unsigned int in_w,
	    unsigned int out_c,
	    unsigned int out_h,
	    unsigned int out_w,
	    unsigned int filter_h,
	    unsigned int filter_w,
	    unsigned int pad_h,
	    unsigned int pad_w,
	    unsigned int stride_h,
	    unsigned int stride_w,
	    unsigned int dilation_h,
	    unsigned int dilation_w,
	    unsigned int total,
	    size_t w_bytes,
	    size_t dy_bytes,
	    size_t dx_bytes,
	    unsigned int gridDimX,
	    unsigned int blockDimX,
	    int required);

	CUresult psyche_cuda_metal_launch_cudnn_convolution_forward_f32(
	    const float *x,
	    const float *w,
    const float *y_in,
    float *out,
    float alpha,
    float beta,
    unsigned int mode,
    unsigned int groups,
    unsigned int n,
    unsigned int in_c,
    unsigned int in_h,
    unsigned int in_w,
    unsigned int out_c,
    unsigned int out_h,
    unsigned int out_w,
    unsigned int filter_h,
    unsigned int filter_w,
    unsigned int pad_h,
    unsigned int pad_w,
    unsigned int stride_h,
    unsigned int stride_w,
    unsigned int dilation_h,
    unsigned int dilation_w,
    unsigned int total,
    size_t x_bytes,
    size_t w_bytes,
    size_t y_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> w_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  size_t x_total = 0;
	  size_t w_total = 0;
	  size_t y_total = 0;
	  unsigned int filter_c = 0;
	  NSUInteger total_threads = 0;
	  NSUInteger threads_per_group = 0;
	  int mpsgraph_required = 0;
	  @autoreleasepool {
    if (
        x == 0 ||
        w == 0 ||
        y_in == 0 ||
	        out == 0 ||
	        mode > 1U ||
	        groups == 0 ||
	        n == 0 ||
	        in_c == 0 ||
	        in_h == 0 ||
        in_w == 0 ||
        out_c == 0 ||
        out_h == 0 ||
        out_w == 0 ||
        filter_h == 0 ||
        filter_w == 0 ||
        stride_h == 0 ||
        stride_w == 0 ||
        dilation_h == 0 ||
        dilation_w == 0 ||
        total == 0 ||
        x_bytes == 0 ||
        w_bytes == 0 ||
        y_bytes == 0 ||
        gridDimX == 0 ||
	        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (in_c % groups != 0 || out_c % groups != 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    filter_c = in_c / groups;
    if (
	        (size_t)n > SIZE_MAX / (size_t)in_c ||
	        (size_t)n * (size_t)in_c > SIZE_MAX / (size_t)in_h ||
	        (size_t)n * (size_t)in_c * (size_t)in_h > SIZE_MAX / (size_t)in_w ||
	        (size_t)out_c > SIZE_MAX / (size_t)filter_c ||
	        (size_t)out_c * (size_t)filter_c > SIZE_MAX / (size_t)filter_h ||
	        (size_t)out_c * (size_t)filter_c * (size_t)filter_h > SIZE_MAX / (size_t)filter_w ||
	        (size_t)n > SIZE_MAX / (size_t)out_c ||
	        (size_t)n * (size_t)out_c > SIZE_MAX / (size_t)out_h ||
	        (size_t)n * (size_t)out_c * (size_t)out_h > SIZE_MAX / (size_t)out_w) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    x_total = (size_t)n * (size_t)in_c * (size_t)in_h * (size_t)in_w;
    w_total = (size_t)out_c * (size_t)filter_c * (size_t)filter_h * (size_t)filter_w;
    y_total = (size_t)n * (size_t)out_c * (size_t)out_h * (size_t)out_w;
    if (
        x_total > SIZE_MAX / sizeof(float) ||
        w_total > SIZE_MAX / sizeof(float) ||
        y_total > SIZE_MAX / sizeof(float) ||
        x_bytes != x_total * sizeof(float) ||
        w_bytes != w_total * sizeof(float) ||
        y_bytes != y_total * sizeof(float) ||
        (size_t)total != y_total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_convolution_forward_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
	    if (result != CUDA_SUCCESS) {
	      return result;
	    }
	    mpsgraph_required = psyche_cuda_metal_cudnn_mpsgraph_required();
	    if (psyche_cuda_metal_cudnn_mpsgraph_enabled()) {
	      result = psyche_cuda_metal_launch_cudnn_convolution_forward_mpsgraph_f32(
	          x,
	          w,
	          y_in,
	          out,
	          alpha,
	          beta,
	          mode,
	          groups,
	          n,
	          in_c,
	          in_h,
	          in_w,
	          out_c,
	          out_h,
	          out_w,
	          filter_h,
	          filter_w,
	          pad_h,
	          pad_w,
	          stride_h,
	          stride_w,
	          dilation_h,
	          dilation_w,
	          total,
	          x_bytes,
	          w_bytes,
	          y_bytes,
	          gridDimX,
	          blockDimX,
	          mpsgraph_required);
	      if (result == CUDA_SUCCESS || mpsgraph_required) {
	        return result;
	      }
	    }
	    x_buffer = [psyche_cuda_metal_device
	        newBufferWithBytes:x
                    length:x_bytes
                   options:MTLResourceStorageModeShared];
    w_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:w
                    length:w_bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:y_in
                      length:y_bytes
                     options:MTLResourceStorageModeShared];
    } else {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithLength:y_bytes
                      options:MTLResourceStorageModeShared];
    }
    /* Keep output separate from caller y storage until the command buffer has completed successfully. */
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:y_bytes
                    options:MTLResourceStorageModeShared];
    if (x_buffer == nil || w_buffer == nil || y_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_convolution_forward_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:w_buffer offset:0 atIndex:1];
    [encoder setBuffer:y_buffer offset:0 atIndex:2];
    [encoder setBuffer:out_buffer offset:0 atIndex:3];
    [encoder setBytes:&alpha length:sizeof(alpha) atIndex:4];
    [encoder setBytes:&beta length:sizeof(beta) atIndex:5];
    [encoder setBytes:&mode length:sizeof(mode) atIndex:6];
    [encoder setBytes:&groups length:sizeof(groups) atIndex:7];
    [encoder setBytes:&n length:sizeof(n) atIndex:8];
    [encoder setBytes:&in_c length:sizeof(in_c) atIndex:9];
    [encoder setBytes:&in_h length:sizeof(in_h) atIndex:10];
    [encoder setBytes:&in_w length:sizeof(in_w) atIndex:11];
    [encoder setBytes:&out_c length:sizeof(out_c) atIndex:12];
    [encoder setBytes:&out_h length:sizeof(out_h) atIndex:13];
    [encoder setBytes:&out_w length:sizeof(out_w) atIndex:14];
    [encoder setBytes:&filter_h length:sizeof(filter_h) atIndex:15];
    [encoder setBytes:&filter_w length:sizeof(filter_w) atIndex:16];
    [encoder setBytes:&pad_h length:sizeof(pad_h) atIndex:17];
    [encoder setBytes:&pad_w length:sizeof(pad_w) atIndex:18];
    [encoder setBytes:&stride_h length:sizeof(stride_h) atIndex:19];
    [encoder setBytes:&stride_w length:sizeof(stride_w) atIndex:20];
    [encoder setBytes:&dilation_h length:sizeof(dilation_h) atIndex:21];
    [encoder setBytes:&dilation_w length:sizeof(dilation_w) atIndex:22];
    [encoder setBytes:&total length:sizeof(total) atIndex:23];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], y_bytes);
  }
  return CUDA_SUCCESS;
}

/*
 * cuDNN 9 deprecates cudnnConvolutionBiasActivationForward, but legacy
 * training stacks still call it. This Metal path exists as a bounded
 * compatibility route for the FP32 NCHW/KCRS subset validated by libcudnn_stub.c.
 */
CUresult psyche_cuda_metal_launch_cudnn_convolution_bias_activation_forward_f32(
    const float *x,
    const float *w,
    const float *z,
    const float *bias,
    float *out,
    float alpha1,
    float alpha2,
    unsigned int activation_mode,
    unsigned int mode,
    unsigned int groups,
    unsigned int n,
    unsigned int in_c,
    unsigned int in_h,
    unsigned int in_w,
    unsigned int out_c,
    unsigned int out_h,
    unsigned int out_w,
    unsigned int filter_h,
    unsigned int filter_w,
    unsigned int pad_h,
    unsigned int pad_w,
    unsigned int stride_h,
    unsigned int stride_w,
    unsigned int dilation_h,
    unsigned int dilation_w,
    unsigned int total,
    size_t x_bytes,
    size_t w_bytes,
    size_t z_bytes,
    size_t bias_bytes,
    size_t y_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> w_buffer = nil;
  id<MTLBuffer> z_buffer = nil;
  id<MTLBuffer> bias_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  psyche_cuda_metal_cudnn_convolution_bias_activation_forward_params params;
  size_t x_total = 0;
  size_t w_total = 0;
  size_t y_total = 0;
  size_t filter_c = 0;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  NSUInteger z_stage_bytes = 0;
  int mpsgraph_required = 0;
  @autoreleasepool {
    if (
        x == 0 ||
        w == 0 ||
        z == 0 ||
        bias == 0 ||
        out == 0 ||
        (activation_mode != 1U && activation_mode != 5U) ||
        mode > 1U ||
        groups == 0 ||
        n == 0 ||
        in_c == 0 ||
        in_h == 0 ||
        in_w == 0 ||
        out_c == 0 ||
        out_h == 0 ||
        out_w == 0 ||
        filter_h == 0 ||
        filter_w == 0 ||
        stride_h == 0 ||
        stride_w == 0 ||
        dilation_h == 0 ||
        dilation_w == 0 ||
        total == 0 ||
        x_bytes == 0 ||
        w_bytes == 0 ||
        z_bytes == 0 ||
        bias_bytes == 0 ||
        y_bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (in_c % groups != 0 || out_c % groups != 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    filter_c = (size_t)in_c / (size_t)groups;
    if (
        (size_t)n > SIZE_MAX / (size_t)in_c ||
        (size_t)n * (size_t)in_c > SIZE_MAX / (size_t)in_h ||
        (size_t)n * (size_t)in_c * (size_t)in_h > SIZE_MAX / (size_t)in_w ||
        (size_t)out_c > SIZE_MAX / filter_c ||
        (size_t)out_c * filter_c > SIZE_MAX / (size_t)filter_h ||
        (size_t)out_c * filter_c * (size_t)filter_h > SIZE_MAX / (size_t)filter_w ||
        (size_t)n > SIZE_MAX / (size_t)out_c ||
        (size_t)n * (size_t)out_c > SIZE_MAX / (size_t)out_h ||
        (size_t)n * (size_t)out_c * (size_t)out_h > SIZE_MAX / (size_t)out_w ||
        (size_t)out_c > SIZE_MAX / sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    x_total = (size_t)n * (size_t)in_c * (size_t)in_h * (size_t)in_w;
    w_total = (size_t)out_c * filter_c * (size_t)filter_h * (size_t)filter_w;
    y_total = (size_t)n * (size_t)out_c * (size_t)out_h * (size_t)out_w;
    if (
        x_total > SIZE_MAX / sizeof(float) ||
        w_total > SIZE_MAX / sizeof(float) ||
        y_total > SIZE_MAX / sizeof(float) ||
        x_bytes != x_total * sizeof(float) ||
        w_bytes != w_total * sizeof(float) ||
        z_bytes != y_total * sizeof(float) ||
        bias_bytes != (size_t)out_c * sizeof(float) ||
        y_bytes != y_total * sizeof(float) ||
        (size_t)total != y_total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (
        psyche_cuda_metal_ranges_overlap_sized(x, x_bytes, w, w_bytes) ||
        psyche_cuda_metal_ranges_overlap_sized(x, x_bytes, z, z_bytes) ||
        psyche_cuda_metal_ranges_overlap_sized(x, x_bytes, bias, bias_bytes) ||
        psyche_cuda_metal_ranges_overlap_sized(x, x_bytes, out, y_bytes) ||
        psyche_cuda_metal_ranges_overlap_sized(w, w_bytes, z, z_bytes) ||
        psyche_cuda_metal_ranges_overlap_sized(w, w_bytes, bias, bias_bytes) ||
        psyche_cuda_metal_ranges_overlap_sized(w, w_bytes, out, y_bytes) ||
        psyche_cuda_metal_ranges_overlap_sized(bias, bias_bytes, z, z_bytes) ||
        psyche_cuda_metal_ranges_overlap_sized(bias, bias_bytes, out, y_bytes) ||
        (psyche_cuda_metal_ranges_overlap_sized(z, z_bytes, out, y_bytes) && z != out)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_convolution_bias_activation_forward_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    mpsgraph_required = psyche_cuda_metal_cudnn_mpsgraph_required();
    if (psyche_cuda_metal_cudnn_mpsgraph_enabled()) {
      result = psyche_cuda_metal_launch_cudnn_convolution_bias_activation_forward_mpsgraph_f32(
          x,
          w,
          z,
          bias,
          out,
          alpha1,
          alpha2,
          activation_mode,
          mode,
          groups,
          n,
          in_c,
          in_h,
          in_w,
          out_c,
          out_h,
          out_w,
          filter_h,
          filter_w,
          pad_h,
          pad_w,
          stride_h,
          stride_w,
          dilation_h,
          dilation_w,
          total,
          x_bytes,
          w_bytes,
          z_bytes,
          bias_bytes,
          y_bytes,
          gridDimX,
          blockDimX,
          mpsgraph_required);
      if (result == CUDA_SUCCESS || mpsgraph_required) {
        return result;
      }
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:x_bytes
                   options:MTLResourceStorageModeShared];
    w_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:w
                    length:w_bytes
                   options:MTLResourceStorageModeShared];
    bias_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:bias
                    length:bias_bytes
                   options:MTLResourceStorageModeShared];
    if (alpha2 != 0.0f) {
      z_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:z
                      length:z_bytes
                     options:MTLResourceStorageModeShared];
    } else {
      z_stage_bytes = sizeof(float);
      z_buffer = [psyche_cuda_metal_device
          newBufferWithLength:z_stage_bytes
                      options:MTLResourceStorageModeShared];
    }
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:y_bytes
                    options:MTLResourceStorageModeShared];
    if (x_buffer == nil || w_buffer == nil || z_buffer == nil || bias_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    memset(&params, 0, sizeof(params));
    params.total = total;
    params.activation_mode = activation_mode;
    params.mode = mode;
    params.groups = groups;
    params.batches = n;
    params.in_channels = in_c;
    params.in_h = in_h;
    params.in_w = in_w;
    params.out_channels = out_c;
    params.out_h = out_h;
    params.out_w = out_w;
    params.filter_h = filter_h;
    params.filter_w = filter_w;
    params.pad_h = pad_h;
    params.pad_w = pad_w;
    params.stride_h = stride_h;
    params.stride_w = stride_w;
    params.dilation_h = dilation_h;
    params.dilation_w = dilation_w;
    params.has_z = alpha2 != 0.0f ? 1U : 0U;
    params.alpha1 = alpha1;
    params.alpha2 = alpha2;
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_convolution_bias_activation_forward_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:w_buffer offset:0 atIndex:1];
    [encoder setBuffer:z_buffer offset:0 atIndex:2];
    [encoder setBuffer:bias_buffer offset:0 atIndex:3];
    [encoder setBuffer:out_buffer offset:0 atIndex:4];
    [encoder setBytes:&params length:sizeof(params) atIndex:5];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (
        getenv("PSYCHE_CUDA_COMPAT_CUDNN_FUSED_FAIL_AFTER_DISPATCH_FOR_TEST") != 0 &&
        strcasecmp(getenv("PSYCHE_CUDA_COMPAT_CUDNN_FUSED_FAIL_AFTER_DISPATCH_FOR_TEST"), "0") != 0) {
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], y_bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cudnn_convolution_backward_data_f32(
    const float *w,
    const float *dy,
    const float *dx_in,
    float *out,
    float alpha,
    float beta,
    unsigned int mode,
    unsigned int groups,
    unsigned int n,
    unsigned int in_c,
    unsigned int in_h,
    unsigned int in_w,
    unsigned int out_c,
    unsigned int out_h,
    unsigned int out_w,
    unsigned int filter_h,
    unsigned int filter_w,
    unsigned int pad_h,
    unsigned int pad_w,
    unsigned int stride_h,
    unsigned int stride_w,
    unsigned int dilation_h,
    unsigned int dilation_w,
    unsigned int total,
    size_t w_bytes,
    size_t dy_bytes,
    size_t dx_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> w_buffer = nil;
  id<MTLBuffer> dy_buffer = nil;
  id<MTLBuffer> dx_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  size_t w_total = 0;
  size_t dy_total = 0;
  size_t dx_total = 0;
	  unsigned int filter_c = 0;
	  NSUInteger total_threads = 0;
	  NSUInteger threads_per_group = 0;
	  int mpsgraph_required = 0;
	  @autoreleasepool {
    if (
        w == 0 ||
        dy == 0 ||
        dx_in == 0 ||
        out == 0 ||
        mode > 1U ||
        groups == 0 ||
        n == 0 ||
        in_c == 0 ||
        in_h == 0 ||
        in_w == 0 ||
        out_c == 0 ||
        out_h == 0 ||
        out_w == 0 ||
        filter_h == 0 ||
        filter_w == 0 ||
        stride_h == 0 ||
        stride_w == 0 ||
        dilation_h == 0 ||
        dilation_w == 0 ||
        total == 0 ||
        w_bytes == 0 ||
        dy_bytes == 0 ||
        dx_bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (in_c % groups != 0 || out_c % groups != 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    filter_c = in_c / groups;
    if (
        (size_t)out_c > SIZE_MAX / (size_t)filter_c ||
        (size_t)out_c * (size_t)filter_c > SIZE_MAX / (size_t)filter_h ||
        (size_t)out_c * (size_t)filter_c * (size_t)filter_h > SIZE_MAX / (size_t)filter_w ||
        (size_t)n > SIZE_MAX / (size_t)out_c ||
        (size_t)n * (size_t)out_c > SIZE_MAX / (size_t)out_h ||
        (size_t)n * (size_t)out_c * (size_t)out_h > SIZE_MAX / (size_t)out_w ||
        (size_t)n > SIZE_MAX / (size_t)in_c ||
        (size_t)n * (size_t)in_c > SIZE_MAX / (size_t)in_h ||
        (size_t)n * (size_t)in_c * (size_t)in_h > SIZE_MAX / (size_t)in_w) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    w_total = (size_t)out_c * (size_t)filter_c * (size_t)filter_h * (size_t)filter_w;
    dy_total = (size_t)n * (size_t)out_c * (size_t)out_h * (size_t)out_w;
    dx_total = (size_t)n * (size_t)in_c * (size_t)in_h * (size_t)in_w;
    if (
        w_total > SIZE_MAX / sizeof(float) ||
        dy_total > SIZE_MAX / sizeof(float) ||
        dx_total > SIZE_MAX / sizeof(float) ||
        w_bytes != w_total * sizeof(float) ||
        dy_bytes != dy_total * sizeof(float) ||
        dx_bytes != dx_total * sizeof(float) ||
        (size_t)total != dx_total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_convolution_backward_data_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
	    if (result != CUDA_SUCCESS) {
	      return result;
	    }
	    mpsgraph_required = psyche_cuda_metal_cudnn_mpsgraph_required();
	    if (psyche_cuda_metal_cudnn_mpsgraph_enabled()) {
	      result = psyche_cuda_metal_launch_cudnn_convolution_backward_data_mpsgraph_f32(
	          w,
	          dy,
	          dx_in,
	          out,
	          alpha,
	          beta,
	          mode,
	          groups,
	          n,
	          in_c,
	          in_h,
	          in_w,
	          out_c,
	          out_h,
	          out_w,
	          filter_h,
	          filter_w,
	          pad_h,
	          pad_w,
	          stride_h,
	          stride_w,
	          dilation_h,
	          dilation_w,
	          total,
	          w_bytes,
	          dy_bytes,
	          dx_bytes,
	          gridDimX,
	          blockDimX,
	          mpsgraph_required);
	      if (result == CUDA_SUCCESS || mpsgraph_required) {
	        return result;
	      }
	    }
	    w_buffer = [psyche_cuda_metal_device
	        newBufferWithBytes:w
                    length:w_bytes
                   options:MTLResourceStorageModeShared];
    dy_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:dy
                    length:dy_bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      dx_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:dx_in
                      length:dx_bytes
                     options:MTLResourceStorageModeShared];
    } else {
      dx_buffer = [psyche_cuda_metal_device
          newBufferWithLength:dx_bytes
                      options:MTLResourceStorageModeShared];
    }
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:dx_bytes
                    options:MTLResourceStorageModeShared];
    if (w_buffer == nil || dy_buffer == nil || dx_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_convolution_backward_data_f32];
    [encoder setBuffer:w_buffer offset:0 atIndex:0];
    [encoder setBuffer:dy_buffer offset:0 atIndex:1];
    [encoder setBuffer:dx_buffer offset:0 atIndex:2];
    [encoder setBuffer:out_buffer offset:0 atIndex:3];
    [encoder setBytes:&alpha length:sizeof(alpha) atIndex:4];
    [encoder setBytes:&beta length:sizeof(beta) atIndex:5];
    [encoder setBytes:&mode length:sizeof(mode) atIndex:6];
    [encoder setBytes:&groups length:sizeof(groups) atIndex:7];
    [encoder setBytes:&n length:sizeof(n) atIndex:8];
    [encoder setBytes:&in_c length:sizeof(in_c) atIndex:9];
    [encoder setBytes:&in_h length:sizeof(in_h) atIndex:10];
    [encoder setBytes:&in_w length:sizeof(in_w) atIndex:11];
    [encoder setBytes:&out_c length:sizeof(out_c) atIndex:12];
    [encoder setBytes:&out_h length:sizeof(out_h) atIndex:13];
    [encoder setBytes:&out_w length:sizeof(out_w) atIndex:14];
    [encoder setBytes:&filter_h length:sizeof(filter_h) atIndex:15];
    [encoder setBytes:&filter_w length:sizeof(filter_w) atIndex:16];
    [encoder setBytes:&pad_h length:sizeof(pad_h) atIndex:17];
    [encoder setBytes:&pad_w length:sizeof(pad_w) atIndex:18];
    [encoder setBytes:&stride_h length:sizeof(stride_h) atIndex:19];
    [encoder setBytes:&stride_w length:sizeof(stride_w) atIndex:20];
    [encoder setBytes:&dilation_h length:sizeof(dilation_h) atIndex:21];
    [encoder setBytes:&dilation_w length:sizeof(dilation_w) atIndex:22];
    [encoder setBytes:&total length:sizeof(total) atIndex:23];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], dx_bytes);
  }
  return CUDA_SUCCESS;
}

static int psyche_cuda_metal_env_truthy(const char *value) {
  if (value == 0 || value[0] == '\0') {
    return 0;
  }
  if (
      strcasecmp(value, "0") == 0 ||
      strcasecmp(value, "false") == 0 ||
      strcasecmp(value, "no") == 0 ||
      strcasecmp(value, "off") == 0) {
    return 0;
  }
  return 1;
}

static int psyche_cuda_metal_env_required(const char *value) {
  return value != 0 && strcasecmp(value, "required") == 0;
}

static int psyche_cuda_metal_cudnn_mpsgraph_enabled(void) {
  const char *value = getenv("PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH");
  return psyche_cuda_metal_env_truthy(value) || psyche_cuda_metal_env_required(value);
}

static int psyche_cuda_metal_cudnn_mpsgraph_required(void) {
  return psyche_cuda_metal_env_required(getenv("PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH"));
}

static CUresult psyche_cuda_metal_cudnn_mpsgraph_failure_result(int required, CUresult result) {
  if (required) {
    return result == CUDA_SUCCESS ? CUDA_ERROR_UNKNOWN : result;
  }
  return CUDA_ERROR_NOT_SUPPORTED;
}

	static int psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(const char *name) {
	  return psyche_cuda_metal_env_truthy(getenv(name));
	}

	static CUresult psyche_cuda_metal_cudnn_mpsgraph_check_command(id<MTLCommandBuffer> command_buffer) {
	  if (command_buffer == nil) {
	    return CUDA_ERROR_OUT_OF_MEMORY;
	  }
	  if (command_buffer.error != nil) {
	    NSError *command_error = command_buffer.error;
	    (void)command_error;
	    return CUDA_ERROR_UNKNOWN;
	  }
	  if (command_buffer.status != MTLCommandBufferStatusCompleted) {
	    NSError *command_error = command_buffer.error;
	    (void)command_error;
	    return CUDA_ERROR_UNKNOWN;
	  }
	  return CUDA_SUCCESS;
	}

	static CUresult psyche_cuda_metal_cudnn_mpsgraph_prepare_weights_buffer(
	    id<MTLBuffer> w_buffer,
	    id<MTLBuffer> graph_w_buffer,
	    unsigned int mode,
	    unsigned int out_c,
	    unsigned int channels_per_group,
	    unsigned int filter_h,
	    unsigned int filter_w,
	    unsigned int total,
	    NSUInteger threads_per_group) {
	  id<MTLCommandBuffer> command_buffer = nil;
	  id<MTLComputeCommandEncoder> encoder = nil;
	  if (
	      w_buffer == nil ||
	      graph_w_buffer == nil ||
	      out_c == 0 ||
	      channels_per_group == 0 ||
	      filter_h == 0 ||
	      filter_w == 0 ||
	      total == 0 ||
	      psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32 == nil ||
	      threads_per_group == 0 ||
	      threads_per_group >
	          psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32.maxTotalThreadsPerThreadgroup) {
	    return CUDA_ERROR_INVALID_VALUE;
	  }
	  command_buffer = [psyche_cuda_metal_queue commandBuffer];
	  if (command_buffer == nil) {
	    return CUDA_ERROR_OUT_OF_MEMORY;
	  }
	  encoder = [command_buffer computeCommandEncoder];
	  if (encoder == nil) {
	    return CUDA_ERROR_OUT_OF_MEMORY;
	  }
	  [encoder setComputePipelineState:psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32];
	  [encoder setBuffer:w_buffer offset:0 atIndex:0];
	  [encoder setBuffer:graph_w_buffer offset:0 atIndex:1];
	  [encoder setBytes:&mode length:sizeof(mode) atIndex:2];
	  [encoder setBytes:&out_c length:sizeof(out_c) atIndex:3];
	  [encoder setBytes:&channels_per_group length:sizeof(channels_per_group) atIndex:4];
	  [encoder setBytes:&filter_h length:sizeof(filter_h) atIndex:5];
	  [encoder setBytes:&filter_w length:sizeof(filter_w) atIndex:6];
	  [encoder setBytes:&total length:sizeof(total) atIndex:7];
	  [encoder dispatchThreads:MTLSizeMake((NSUInteger)total, 1, 1)
	      threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
	  [encoder endEncoding];
	  [command_buffer commit];
	  [command_buffer waitUntilCompleted];
	  return psyche_cuda_metal_cudnn_mpsgraph_check_command(command_buffer);
	}

	static CUresult psyche_cuda_metal_cudnn_mpsgraph_apply_buffer(
	    id<MTLBuffer> raw_buffer,
	    id<MTLBuffer> prior_buffer,
	    id<MTLBuffer> final_buffer,
	    float alpha,
	    float beta,
	    unsigned int total,
	    NSUInteger threads_per_group) {
	  id<MTLCommandBuffer> command_buffer = nil;
	  id<MTLComputeCommandEncoder> encoder = nil;
	  if (
	      raw_buffer == nil ||
	      prior_buffer == nil ||
	      final_buffer == nil ||
	      total == 0 ||
	      psyche_cuda_metal_cudnn_convolution_mpsgraph_apply_f32 == nil ||
	      threads_per_group == 0 ||
	      threads_per_group >
	          psyche_cuda_metal_cudnn_convolution_mpsgraph_apply_f32.maxTotalThreadsPerThreadgroup) {
	    return CUDA_ERROR_INVALID_VALUE;
	  }
	  command_buffer = [psyche_cuda_metal_queue commandBuffer];
	  if (command_buffer == nil) {
	    return CUDA_ERROR_OUT_OF_MEMORY;
	  }
	  encoder = [command_buffer computeCommandEncoder];
	  if (encoder == nil) {
	    return CUDA_ERROR_OUT_OF_MEMORY;
	  }
	  [encoder setComputePipelineState:psyche_cuda_metal_cudnn_convolution_mpsgraph_apply_f32];
	  [encoder setBuffer:raw_buffer offset:0 atIndex:0];
	  [encoder setBuffer:prior_buffer offset:0 atIndex:1];
	  [encoder setBuffer:final_buffer offset:0 atIndex:2];
	  [encoder setBytes:&alpha length:sizeof(alpha) atIndex:3];
	  [encoder setBytes:&beta length:sizeof(beta) atIndex:4];
	  [encoder setBytes:&total length:sizeof(total) atIndex:5];
	  [encoder dispatchThreads:MTLSizeMake((NSUInteger)total, 1, 1)
	      threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
	  [encoder endEncoding];
	  [command_buffer commit];
	  [command_buffer waitUntilCompleted];
	  return psyche_cuda_metal_cudnn_mpsgraph_check_command(command_buffer);
	}

	static CUresult psyche_cuda_metal_cudnn_mpsgraph_apply_fused_bias_activation_buffer(
	    id<MTLBuffer> raw_buffer,
	    id<MTLBuffer> z_buffer,
	    id<MTLBuffer> bias_buffer,
	    id<MTLBuffer> final_buffer,
	    float alpha1,
	    float alpha2,
	    unsigned int activation_mode,
	    unsigned int out_channels,
	    unsigned int out_h,
	    unsigned int out_w,
	    unsigned int has_z,
	    unsigned int total,
	    NSUInteger threads_per_group) {
	  id<MTLCommandBuffer> command_buffer = nil;
	  id<MTLComputeCommandEncoder> encoder = nil;
	  if (
	      raw_buffer == nil ||
	      z_buffer == nil ||
	      bias_buffer == nil ||
	      final_buffer == nil ||
	      (activation_mode != 1U && activation_mode != 5U) ||
	      out_channels == 0 ||
	      out_h == 0 ||
	      out_w == 0 ||
	      total == 0 ||
	      psyche_cuda_metal_cudnn_convolution_bias_activation_mpsgraph_apply_f32 == nil ||
	      threads_per_group == 0 ||
	      threads_per_group >
	          psyche_cuda_metal_cudnn_convolution_bias_activation_mpsgraph_apply_f32.maxTotalThreadsPerThreadgroup) {
	    return CUDA_ERROR_INVALID_VALUE;
	  }
	  command_buffer = [psyche_cuda_metal_queue commandBuffer];
	  if (command_buffer == nil) {
	    return CUDA_ERROR_OUT_OF_MEMORY;
	  }
	  encoder = [command_buffer computeCommandEncoder];
	  if (encoder == nil) {
	    return CUDA_ERROR_OUT_OF_MEMORY;
	  }
	  [encoder setComputePipelineState:psyche_cuda_metal_cudnn_convolution_bias_activation_mpsgraph_apply_f32];
	  [encoder setBuffer:raw_buffer offset:0 atIndex:0];
	  [encoder setBuffer:z_buffer offset:0 atIndex:1];
	  [encoder setBuffer:bias_buffer offset:0 atIndex:2];
	  [encoder setBuffer:final_buffer offset:0 atIndex:3];
	  [encoder setBytes:&alpha1 length:sizeof(alpha1) atIndex:4];
	  [encoder setBytes:&alpha2 length:sizeof(alpha2) atIndex:5];
	  [encoder setBytes:&activation_mode length:sizeof(activation_mode) atIndex:6];
	  [encoder setBytes:&out_channels length:sizeof(out_channels) atIndex:7];
	  [encoder setBytes:&out_h length:sizeof(out_h) atIndex:8];
	  [encoder setBytes:&out_w length:sizeof(out_w) atIndex:9];
	  [encoder setBytes:&has_z length:sizeof(has_z) atIndex:10];
	  [encoder setBytes:&total length:sizeof(total) atIndex:11];
	  [encoder dispatchThreads:MTLSizeMake((NSUInteger)total, 1, 1)
	      threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
	  [encoder endEncoding];
	  [command_buffer commit];
	  [command_buffer waitUntilCompleted];
	  return psyche_cuda_metal_cudnn_mpsgraph_check_command(command_buffer);
	}

	static MPSGraphConvolution2DOpDescriptor *psyche_cuda_metal_cudnn_mpsgraph_convolution_descriptor(
	    Class descriptor_class,
	    unsigned int groups,
	    unsigned int pad_h,
	    unsigned int pad_w,
	    unsigned int stride_h,
	    unsigned int stride_w,
	    unsigned int dilation_h,
	    unsigned int dilation_w) {
	  return [(id)descriptor_class descriptorWithStrideInX:(NSUInteger)stride_w
	                                             strideInY:(NSUInteger)stride_h
	                                       dilationRateInX:(NSUInteger)dilation_w
	                                       dilationRateInY:(NSUInteger)dilation_h
	                                                groups:(NSUInteger)groups
	                                           paddingLeft:(NSUInteger)pad_w
	                                          paddingRight:(NSUInteger)pad_w
	                                            paddingTop:(NSUInteger)pad_h
	                                         paddingBottom:(NSUInteger)pad_h
	                                          paddingStyle:MPSGraphPaddingStyleExplicit
	                                            dataLayout:MPSGraphTensorNamedDataLayoutNCHW
	                                         weightsLayout:MPSGraphTensorNamedDataLayoutOIHW];
	}

	static CUresult psyche_cuda_metal_launch_cudnn_convolution_bias_activation_forward_mpsgraph_f32(
	    const float *x,
	    const float *w,
	    const float *z,
	    const float *bias,
	    float *out,
	    float alpha1,
	    float alpha2,
	    unsigned int activation_mode,
	    unsigned int mode,
	    unsigned int groups,
	    unsigned int n,
	    unsigned int in_c,
	    unsigned int in_h,
	    unsigned int in_w,
	    unsigned int out_c,
	    unsigned int out_h,
	    unsigned int out_w,
	    unsigned int filter_h,
	    unsigned int filter_w,
	    unsigned int pad_h,
	    unsigned int pad_w,
	    unsigned int stride_h,
	    unsigned int stride_w,
	    unsigned int dilation_h,
	    unsigned int dilation_w,
	    unsigned int total,
	    size_t x_bytes,
	    size_t w_bytes,
	    size_t z_bytes,
	    size_t bias_bytes,
	    size_t y_bytes,
	    unsigned int gridDimX,
	    unsigned int blockDimX,
	    int required) {
	  CUresult result = CUDA_SUCCESS;
	  id<MTLBuffer> x_buffer = nil;
	  id<MTLBuffer> w_buffer = nil;
	  id<MTLBuffer> graph_w_buffer = nil;
	  id<MTLBuffer> z_buffer = nil;
	  id<MTLBuffer> bias_buffer = nil;
	  id<MTLBuffer> raw_buffer = nil;
	  id<MTLBuffer> final_buffer = nil;
	  NSUInteger total_threads = 0;
	  NSUInteger threads_per_group = 0;
	  NSUInteger z_stage_bytes = 0;
	  unsigned int channels_per_group = 0;
	  unsigned int has_z = alpha2 != 0.0f ? 1U : 0U;
	  size_t x_total = 0;
	  size_t w_total = 0;
	  size_t y_total = 0;
	  @autoreleasepool {
	    if (@available(macOS 11.0, *)) {
	      Class graph_class = NSClassFromString(@"MPSGraph");
	      Class descriptor_class = NSClassFromString(@"MPSGraphConvolution2DOpDescriptor");
	      Class tensor_data_class = NSClassFromString(@"MPSGraphTensorData");
	      if (graph_class == Nil || descriptor_class == Nil || tensor_data_class == Nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      if (
	          x == 0 ||
	          w == 0 ||
	          z == 0 ||
	          bias == 0 ||
	          out == 0 ||
	          (activation_mode != 1U && activation_mode != 5U) ||
	          mode > 1U ||
	          groups == 0 ||
	          n == 0 ||
	          in_c == 0 ||
	          in_h == 0 ||
	          in_w == 0 ||
	          out_c == 0 ||
	          out_h == 0 ||
	          out_w == 0 ||
	          filter_h == 0 ||
	          filter_w == 0 ||
	          stride_h == 0 ||
	          stride_w == 0 ||
	          dilation_h == 0 ||
	          dilation_w == 0 ||
	          total == 0 ||
	          x_bytes == 0 ||
	          w_bytes == 0 ||
	          z_bytes == 0 ||
	          bias_bytes == 0 ||
	          y_bytes == 0 ||
	          gridDimX == 0 ||
	          blockDimX == 0 ||
	          in_c % groups != 0 ||
	          out_c % groups != 0 ||
	          (size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
	      }
	      channels_per_group = in_c / groups;
	      if (
	          (size_t)n > SIZE_MAX / (size_t)in_c ||
	          (size_t)n * (size_t)in_c > SIZE_MAX / (size_t)in_h ||
	          (size_t)n * (size_t)in_c * (size_t)in_h > SIZE_MAX / (size_t)in_w ||
	          (size_t)out_c > SIZE_MAX / (size_t)channels_per_group ||
	          (size_t)out_c * (size_t)channels_per_group > SIZE_MAX / (size_t)filter_h ||
	          (size_t)out_c * (size_t)channels_per_group * (size_t)filter_h > SIZE_MAX / (size_t)filter_w ||
	          (size_t)n > SIZE_MAX / (size_t)out_c ||
	          (size_t)n * (size_t)out_c > SIZE_MAX / (size_t)out_h ||
	          (size_t)n * (size_t)out_c * (size_t)out_h > SIZE_MAX / (size_t)out_w) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
	      }
	      x_total = (size_t)n * (size_t)in_c * (size_t)in_h * (size_t)in_w;
	      w_total = (size_t)out_c * (size_t)channels_per_group * (size_t)filter_h * (size_t)filter_w;
	      y_total = (size_t)n * (size_t)out_c * (size_t)out_h * (size_t)out_w;
	      if (
	          x_total > SIZE_MAX / sizeof(float) ||
	          w_total > SIZE_MAX / sizeof(float) ||
	          y_total > SIZE_MAX / sizeof(float) ||
	          w_total > UINT_MAX ||
	          x_bytes != x_total * sizeof(float) ||
	          w_bytes != w_total * sizeof(float) ||
	          z_bytes != y_total * sizeof(float) ||
	          bias_bytes != (size_t)out_c * sizeof(float) ||
	          y_bytes != y_total * sizeof(float) ||
	          (size_t)total != y_total) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
	      }
	      total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
	      threads_per_group = (NSUInteger)blockDimX;
	      if (
	          total_threads < (NSUInteger)total ||
	          psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32 == nil ||
	          psyche_cuda_metal_cudnn_convolution_bias_activation_mpsgraph_apply_f32 == nil ||
	          threads_per_group >
	              psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32.maxTotalThreadsPerThreadgroup ||
	          threads_per_group >
	              psyche_cuda_metal_cudnn_convolution_bias_activation_mpsgraph_apply_f32.maxTotalThreadsPerThreadgroup) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }

	      x_buffer = [psyche_cuda_metal_device
	          newBufferWithBytes:x
	                      length:x_bytes
	                     options:MTLResourceStorageModeShared];
	      w_buffer = [psyche_cuda_metal_device
	          newBufferWithBytes:w
	                      length:w_bytes
	                     options:MTLResourceStorageModeShared];
	      graph_w_buffer = [psyche_cuda_metal_device
	          newBufferWithLength:w_bytes
	                      options:MTLResourceStorageModeShared];
	      if (has_z != 0U) {
	        z_buffer = [psyche_cuda_metal_device
	            newBufferWithBytes:z
	                        length:z_bytes
	                       options:MTLResourceStorageModeShared];
	      } else {
	        z_stage_bytes = sizeof(float);
	        z_buffer = [psyche_cuda_metal_device
	            newBufferWithLength:z_stage_bytes
	                        options:MTLResourceStorageModeShared];
	      }
	      bias_buffer = [psyche_cuda_metal_device
	          newBufferWithBytes:bias
	                      length:bias_bytes
	                     options:MTLResourceStorageModeShared];
	      raw_buffer = [psyche_cuda_metal_device
	          newBufferWithLength:y_bytes
	                      options:MTLResourceStorageModeShared];
	      final_buffer = [psyche_cuda_metal_device
	          newBufferWithLength:y_bytes
	                      options:MTLResourceStorageModeShared];
	      if (
	          x_buffer == nil ||
	          w_buffer == nil ||
	          graph_w_buffer == nil ||
	          z_buffer == nil ||
	          bias_buffer == nil ||
	          raw_buffer == nil ||
	          final_buffer == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
	      }
	      result = psyche_cuda_metal_cudnn_mpsgraph_prepare_weights_buffer(
	          w_buffer,
	          graph_w_buffer,
	          mode,
	          out_c,
	          channels_per_group,
	          filter_h,
	          filter_w,
	          (unsigned int)w_total,
	          threads_per_group);
	      if (result != CUDA_SUCCESS) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, result);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_AFTER_PREPARE_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }

	      MPSShape *x_shape = @[ @(n), @(in_c), @(in_h), @(in_w) ];
	      MPSShape *w_shape = @[ @(out_c), @(channels_per_group), @(filter_h), @(filter_w) ];
	      MPSShape *y_shape = @[ @(n), @(out_c), @(out_h), @(out_w) ];
	      MPSGraph *graph = (MPSGraph *)[graph_class new];
	      if (graph == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
	      }
	      MPSGraphTensor *x_tensor =
	          [graph placeholderWithShape:x_shape dataType:MPSDataTypeFloat32 name:@"psyche_fused_fwd_x"];
	      MPSGraphTensor *w_tensor =
	          [graph placeholderWithShape:w_shape dataType:MPSDataTypeFloat32 name:@"psyche_fused_fwd_w"];
	      if (x_tensor == nil || w_tensor == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      MPSGraphConvolution2DOpDescriptor *descriptor =
	          psyche_cuda_metal_cudnn_mpsgraph_convolution_descriptor(
	              descriptor_class, groups, pad_h, pad_w, stride_h, stride_w, dilation_h, dilation_w);
	      if (descriptor == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      MPSGraphTensor *raw_tensor =
	          [graph convolution2DWithSourceTensor:x_tensor
	                                weightsTensor:w_tensor
	                                   descriptor:descriptor
	                                         name:@"psyche_fused_fwd_raw_y"];
	      if (raw_tensor == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      MPSGraphTensorData *x_data =
	          [[(id)tensor_data_class alloc] initWithMTLBuffer:x_buffer shape:x_shape dataType:MPSDataTypeFloat32];
	      MPSGraphTensorData *w_data =
	          [[(id)tensor_data_class alloc] initWithMTLBuffer:graph_w_buffer shape:w_shape dataType:MPSDataTypeFloat32];
	      MPSGraphTensorData *raw_data =
	          [[(id)tensor_data_class alloc] initWithMTLBuffer:raw_buffer shape:y_shape dataType:MPSDataTypeFloat32];
	      if (x_data == nil || w_data == nil || raw_data == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
	      }
	      @try {
	        [graph runWithMTLCommandQueue:psyche_cuda_metal_queue
	                                feeds:@{ x_tensor: x_data, w_tensor: w_data }
	                     targetOperations:nil
	                    resultsDictionary:@{ raw_tensor: raw_data }];
	      } @catch (NSException *exception) {
	        (void)exception;
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_AFTER_GRAPH_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      result = psyche_cuda_metal_cudnn_mpsgraph_apply_fused_bias_activation_buffer(
	          raw_buffer,
	          z_buffer,
	          bias_buffer,
	          final_buffer,
	          alpha1,
	          alpha2,
	          activation_mode,
	          out_c,
	          out_h,
	          out_w,
	          has_z,
	          total,
	          threads_per_group);
	      if (result != CUDA_SUCCESS) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, result);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_AFTER_EPILOGUE_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      memcpy(out, [final_buffer contents], y_bytes);
	      result = CUDA_SUCCESS;
	    } else {
	      result = psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	    }
	  }
	  return result;
	}

	static CUresult psyche_cuda_metal_launch_cudnn_convolution_forward_mpsgraph_f32(
	    const float *x,
	    const float *w,
	    const float *y_in,
	    float *out,
	    float alpha,
	    float beta,
	    unsigned int mode,
	    unsigned int groups,
	    unsigned int n,
	    unsigned int in_c,
	    unsigned int in_h,
	    unsigned int in_w,
	    unsigned int out_c,
	    unsigned int out_h,
	    unsigned int out_w,
	    unsigned int filter_h,
	    unsigned int filter_w,
	    unsigned int pad_h,
	    unsigned int pad_w,
	    unsigned int stride_h,
	    unsigned int stride_w,
	    unsigned int dilation_h,
	    unsigned int dilation_w,
	    unsigned int total,
	    size_t x_bytes,
	    size_t w_bytes,
	    size_t y_bytes,
	    unsigned int gridDimX,
	    unsigned int blockDimX,
	    int required) {
	  CUresult result = CUDA_SUCCESS;
	  id<MTLBuffer> x_buffer = nil;
	  id<MTLBuffer> w_buffer = nil;
	  id<MTLBuffer> graph_w_buffer = nil;
	  id<MTLBuffer> y_buffer = nil;
	  id<MTLBuffer> raw_buffer = nil;
	  id<MTLBuffer> final_buffer = nil;
	  NSUInteger total_threads = 0;
	  NSUInteger threads_per_group = 0;
	  unsigned int channels_per_group = 0;
	  size_t x_total = 0;
	  size_t w_total = 0;
	  size_t y_total = 0;
	  @autoreleasepool {
	    if (@available(macOS 11.0, *)) {
	      Class graph_class = NSClassFromString(@"MPSGraph");
	      Class descriptor_class = NSClassFromString(@"MPSGraphConvolution2DOpDescriptor");
	      Class tensor_data_class = NSClassFromString(@"MPSGraphTensorData");
	      if (graph_class == Nil || descriptor_class == Nil || tensor_data_class == Nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      if (
	          x == 0 ||
	          w == 0 ||
	          y_in == 0 ||
	          out == 0 ||
	          mode > 1U ||
	          groups == 0 ||
	          n == 0 ||
	          in_c == 0 ||
	          in_h == 0 ||
	          in_w == 0 ||
	          out_c == 0 ||
	          out_h == 0 ||
	          out_w == 0 ||
	          filter_h == 0 ||
	          filter_w == 0 ||
	          stride_h == 0 ||
	          stride_w == 0 ||
	          dilation_h == 0 ||
	          dilation_w == 0 ||
	          total == 0 ||
	          x_bytes == 0 ||
	          w_bytes == 0 ||
	          y_bytes == 0 ||
	          gridDimX == 0 ||
	          blockDimX == 0 ||
	          in_c % groups != 0 ||
	          out_c % groups != 0 ||
	          (size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
	      }
	      channels_per_group = in_c / groups;
	      if (
	          (size_t)n > SIZE_MAX / (size_t)in_c ||
	          (size_t)n * (size_t)in_c > SIZE_MAX / (size_t)in_h ||
	          (size_t)n * (size_t)in_c * (size_t)in_h > SIZE_MAX / (size_t)in_w ||
	          (size_t)out_c > SIZE_MAX / (size_t)channels_per_group ||
	          (size_t)out_c * (size_t)channels_per_group > SIZE_MAX / (size_t)filter_h ||
	          (size_t)out_c * (size_t)channels_per_group * (size_t)filter_h > SIZE_MAX / (size_t)filter_w ||
	          (size_t)n > SIZE_MAX / (size_t)out_c ||
	          (size_t)n * (size_t)out_c > SIZE_MAX / (size_t)out_h ||
	          (size_t)n * (size_t)out_c * (size_t)out_h > SIZE_MAX / (size_t)out_w) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
	      }
	      x_total = (size_t)n * (size_t)in_c * (size_t)in_h * (size_t)in_w;
	      w_total = (size_t)out_c * (size_t)channels_per_group * (size_t)filter_h * (size_t)filter_w;
	      y_total = (size_t)n * (size_t)out_c * (size_t)out_h * (size_t)out_w;
	      if (
	          x_total > SIZE_MAX / sizeof(float) ||
	          w_total > SIZE_MAX / sizeof(float) ||
	          y_total > SIZE_MAX / sizeof(float) ||
	          w_total > UINT_MAX ||
	          x_bytes != x_total * sizeof(float) ||
	          w_bytes != w_total * sizeof(float) ||
	          y_bytes != y_total * sizeof(float) ||
	          (size_t)total != y_total) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
	      }
	      total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
	      threads_per_group = (NSUInteger)blockDimX;
	      if (
	          total_threads < (NSUInteger)total ||
	          psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32 == nil ||
	          psyche_cuda_metal_cudnn_convolution_mpsgraph_apply_f32 == nil ||
	          threads_per_group >
	              psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32.maxTotalThreadsPerThreadgroup ||
	          threads_per_group > psyche_cuda_metal_cudnn_convolution_mpsgraph_apply_f32.maxTotalThreadsPerThreadgroup) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }

	      x_buffer = [psyche_cuda_metal_device
	          newBufferWithBytes:x
	                      length:x_bytes
	                     options:MTLResourceStorageModeShared];
	      w_buffer = [psyche_cuda_metal_device
	          newBufferWithBytes:w
	                      length:w_bytes
	                     options:MTLResourceStorageModeShared];
	      graph_w_buffer = [psyche_cuda_metal_device
	          newBufferWithLength:w_bytes
	                      options:MTLResourceStorageModeShared];
	      if (beta != 0.0f) {
	        y_buffer = [psyche_cuda_metal_device
	            newBufferWithBytes:y_in
	                        length:y_bytes
	                       options:MTLResourceStorageModeShared];
	      } else {
	        y_buffer = [psyche_cuda_metal_device
	            newBufferWithLength:y_bytes
	                        options:MTLResourceStorageModeShared];
	      }
	      raw_buffer = [psyche_cuda_metal_device
	          newBufferWithLength:y_bytes
	                      options:MTLResourceStorageModeShared];
	      final_buffer = [psyche_cuda_metal_device
	          newBufferWithLength:y_bytes
	                      options:MTLResourceStorageModeShared];
	      if (
	          x_buffer == nil ||
	          w_buffer == nil ||
	          graph_w_buffer == nil ||
	          y_buffer == nil ||
	          raw_buffer == nil ||
	          final_buffer == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
	      }
	      result = psyche_cuda_metal_cudnn_mpsgraph_prepare_weights_buffer(
	          w_buffer,
	          graph_w_buffer,
	          mode,
	          out_c,
	          channels_per_group,
	          filter_h,
	          filter_w,
	          (unsigned int)w_total,
	          threads_per_group);
	      if (result != CUDA_SUCCESS) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, result);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_AFTER_PREPARE_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }

	      MPSShape *x_shape = @[ @(n), @(in_c), @(in_h), @(in_w) ];
	      MPSShape *w_shape = @[ @(out_c), @(channels_per_group), @(filter_h), @(filter_w) ];
	      MPSShape *y_shape = @[ @(n), @(out_c), @(out_h), @(out_w) ];
	      MPSGraph *graph = (MPSGraph *)[graph_class new];
	      if (graph == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
	      }
	      MPSGraphTensor *x_tensor =
	          [graph placeholderWithShape:x_shape dataType:MPSDataTypeFloat32 name:@"psyche_fwd_x"];
	      MPSGraphTensor *w_tensor =
	          [graph placeholderWithShape:w_shape dataType:MPSDataTypeFloat32 name:@"psyche_fwd_w"];
	      if (x_tensor == nil || w_tensor == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      MPSGraphConvolution2DOpDescriptor *descriptor =
	          psyche_cuda_metal_cudnn_mpsgraph_convolution_descriptor(
	              descriptor_class, groups, pad_h, pad_w, stride_h, stride_w, dilation_h, dilation_w);
	      if (descriptor == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      MPSGraphTensor *raw_tensor =
	          [graph convolution2DWithSourceTensor:x_tensor
	                                weightsTensor:w_tensor
	                                   descriptor:descriptor
	                                         name:@"psyche_fwd_raw_y"];
	      if (raw_tensor == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      MPSGraphTensorData *x_data =
	          [[(id)tensor_data_class alloc] initWithMTLBuffer:x_buffer shape:x_shape dataType:MPSDataTypeFloat32];
	      MPSGraphTensorData *w_data =
	          [[(id)tensor_data_class alloc] initWithMTLBuffer:graph_w_buffer shape:w_shape dataType:MPSDataTypeFloat32];
	      MPSGraphTensorData *raw_data =
	          [[(id)tensor_data_class alloc] initWithMTLBuffer:raw_buffer shape:y_shape dataType:MPSDataTypeFloat32];
	      if (x_data == nil || w_data == nil || raw_data == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
	      }
	      @try {
	        [graph runWithMTLCommandQueue:psyche_cuda_metal_queue
	                                feeds:@{ x_tensor: x_data, w_tensor: w_data }
	                     targetOperations:nil
	                    resultsDictionary:@{ raw_tensor: raw_data }];
	      } @catch (NSException *exception) {
	        (void)exception;
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_AFTER_GRAPH_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      result = psyche_cuda_metal_cudnn_mpsgraph_apply_buffer(
	          raw_buffer, y_buffer, final_buffer, alpha, beta, total, threads_per_group);
	      if (result != CUDA_SUCCESS) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, result);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_AFTER_EPILOGUE_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      memcpy(out, [final_buffer contents], y_bytes);
	      result = CUDA_SUCCESS;
	    } else {
	      result = psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	    }
	  }
	  return result;
	}

	static CUresult psyche_cuda_metal_launch_cudnn_convolution_backward_data_mpsgraph_f32(
	    const float *w,
	    const float *dy,
	    const float *dx_in,
	    float *out,
	    float alpha,
	    float beta,
	    unsigned int mode,
	    unsigned int groups,
	    unsigned int n,
	    unsigned int in_c,
	    unsigned int in_h,
	    unsigned int in_w,
	    unsigned int out_c,
	    unsigned int out_h,
	    unsigned int out_w,
	    unsigned int filter_h,
	    unsigned int filter_w,
	    unsigned int pad_h,
	    unsigned int pad_w,
	    unsigned int stride_h,
	    unsigned int stride_w,
	    unsigned int dilation_h,
	    unsigned int dilation_w,
	    unsigned int total,
	    size_t w_bytes,
	    size_t dy_bytes,
	    size_t dx_bytes,
	    unsigned int gridDimX,
	    unsigned int blockDimX,
	    int required) {
	  CUresult result = CUDA_SUCCESS;
	  id<MTLBuffer> w_buffer = nil;
	  id<MTLBuffer> graph_w_buffer = nil;
	  id<MTLBuffer> dy_buffer = nil;
	  id<MTLBuffer> dx_buffer = nil;
	  id<MTLBuffer> raw_buffer = nil;
	  id<MTLBuffer> final_buffer = nil;
	  NSUInteger total_threads = 0;
	  NSUInteger threads_per_group = 0;
	  unsigned int channels_per_group = 0;
	  size_t w_total = 0;
	  size_t dy_total = 0;
	  size_t dx_total = 0;
	  @autoreleasepool {
	    if (@available(macOS 11.0, *)) {
	      Class graph_class = NSClassFromString(@"MPSGraph");
	      Class descriptor_class = NSClassFromString(@"MPSGraphConvolution2DOpDescriptor");
	      Class tensor_data_class = NSClassFromString(@"MPSGraphTensorData");
	      if (graph_class == Nil || descriptor_class == Nil || tensor_data_class == Nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      if (
	          w == 0 ||
	          dy == 0 ||
	          dx_in == 0 ||
	          out == 0 ||
	          mode > 1U ||
	          groups == 0 ||
	          n == 0 ||
	          in_c == 0 ||
	          in_h == 0 ||
	          in_w == 0 ||
	          out_c == 0 ||
	          out_h == 0 ||
	          out_w == 0 ||
	          filter_h == 0 ||
	          filter_w == 0 ||
	          stride_h == 0 ||
	          stride_w == 0 ||
	          dilation_h == 0 ||
	          dilation_w == 0 ||
	          total == 0 ||
	          w_bytes == 0 ||
	          dy_bytes == 0 ||
	          dx_bytes == 0 ||
	          gridDimX == 0 ||
	          blockDimX == 0 ||
	          in_c % groups != 0 ||
	          out_c % groups != 0 ||
	          (size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
	      }
	      channels_per_group = in_c / groups;
	      if (
	          (size_t)out_c > SIZE_MAX / (size_t)channels_per_group ||
	          (size_t)out_c * (size_t)channels_per_group > SIZE_MAX / (size_t)filter_h ||
	          (size_t)out_c * (size_t)channels_per_group * (size_t)filter_h > SIZE_MAX / (size_t)filter_w ||
	          (size_t)n > SIZE_MAX / (size_t)out_c ||
	          (size_t)n * (size_t)out_c > SIZE_MAX / (size_t)out_h ||
	          (size_t)n * (size_t)out_c * (size_t)out_h > SIZE_MAX / (size_t)out_w ||
	          (size_t)n > SIZE_MAX / (size_t)in_c ||
	          (size_t)n * (size_t)in_c > SIZE_MAX / (size_t)in_h ||
	          (size_t)n * (size_t)in_c * (size_t)in_h > SIZE_MAX / (size_t)in_w) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
	      }
	      w_total = (size_t)out_c * (size_t)channels_per_group * (size_t)filter_h * (size_t)filter_w;
	      dy_total = (size_t)n * (size_t)out_c * (size_t)out_h * (size_t)out_w;
	      dx_total = (size_t)n * (size_t)in_c * (size_t)in_h * (size_t)in_w;
	      if (
	          w_total > SIZE_MAX / sizeof(float) ||
	          dy_total > SIZE_MAX / sizeof(float) ||
	          dx_total > SIZE_MAX / sizeof(float) ||
	          w_total > UINT_MAX ||
	          w_bytes != w_total * sizeof(float) ||
	          dy_bytes != dy_total * sizeof(float) ||
	          dx_bytes != dx_total * sizeof(float) ||
	          (size_t)total != dx_total) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
	      }
	      total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
	      threads_per_group = (NSUInteger)blockDimX;
	      if (
	          total_threads < (NSUInteger)total ||
	          psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32 == nil ||
	          psyche_cuda_metal_cudnn_convolution_mpsgraph_apply_f32 == nil ||
	          threads_per_group >
	              psyche_cuda_metal_cudnn_convolution_mpsgraph_prepare_weights_f32.maxTotalThreadsPerThreadgroup ||
	          threads_per_group > psyche_cuda_metal_cudnn_convolution_mpsgraph_apply_f32.maxTotalThreadsPerThreadgroup) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }

	      w_buffer = [psyche_cuda_metal_device
	          newBufferWithBytes:w
	                      length:w_bytes
	                     options:MTLResourceStorageModeShared];
	      graph_w_buffer = [psyche_cuda_metal_device
	          newBufferWithLength:w_bytes
	                      options:MTLResourceStorageModeShared];
	      dy_buffer = [psyche_cuda_metal_device
	          newBufferWithBytes:dy
	                      length:dy_bytes
	                     options:MTLResourceStorageModeShared];
	      if (beta != 0.0f) {
	        dx_buffer = [psyche_cuda_metal_device
	            newBufferWithBytes:dx_in
	                        length:dx_bytes
	                       options:MTLResourceStorageModeShared];
	      } else {
	        dx_buffer = [psyche_cuda_metal_device
	            newBufferWithLength:dx_bytes
	                        options:MTLResourceStorageModeShared];
	      }
	      raw_buffer = [psyche_cuda_metal_device
	          newBufferWithLength:dx_bytes
	                      options:MTLResourceStorageModeShared];
	      final_buffer = [psyche_cuda_metal_device
	          newBufferWithLength:dx_bytes
	                      options:MTLResourceStorageModeShared];
	      if (
	          w_buffer == nil ||
	          graph_w_buffer == nil ||
	          dy_buffer == nil ||
	          dx_buffer == nil ||
	          raw_buffer == nil ||
	          final_buffer == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
	      }
	      result = psyche_cuda_metal_cudnn_mpsgraph_prepare_weights_buffer(
	          w_buffer,
	          graph_w_buffer,
	          mode,
	          out_c,
	          channels_per_group,
	          filter_h,
	          filter_w,
	          (unsigned int)w_total,
	          threads_per_group);
	      if (result != CUDA_SUCCESS) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, result);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_AFTER_PREPARE_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }

	      MPSShape *dy_shape = @[ @(n), @(out_c), @(out_h), @(out_w) ];
	      MPSShape *w_shape = @[ @(out_c), @(channels_per_group), @(filter_h), @(filter_w) ];
	      MPSShape *dx_shape = @[ @(n), @(in_c), @(in_h), @(in_w) ];
	      MPSGraph *graph = (MPSGraph *)[graph_class new];
	      if (graph == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
	      }
	      MPSGraphTensor *dy_tensor =
	          [graph placeholderWithShape:dy_shape dataType:MPSDataTypeFloat32 name:@"psyche_bwd_data_dy"];
	      MPSGraphTensor *w_tensor =
	          [graph placeholderWithShape:w_shape dataType:MPSDataTypeFloat32 name:@"psyche_bwd_data_w"];
	      if (dy_tensor == nil || w_tensor == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      MPSGraphConvolution2DOpDescriptor *descriptor =
	          psyche_cuda_metal_cudnn_mpsgraph_convolution_descriptor(
	              descriptor_class, groups, pad_h, pad_w, stride_h, stride_w, dilation_h, dilation_w);
	      if (descriptor == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      MPSGraphTensor *raw_tensor =
	          [graph convolution2DDataGradientWithIncomingGradientTensor:dy_tensor
	                                                       weightsTensor:w_tensor
	                                                         outputShape:dx_shape
	                                        forwardConvolutionDescriptor:descriptor
	                                                                name:@"psyche_bwd_data_raw_dx"];
	      if (raw_tensor == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	      }
	      MPSGraphTensorData *dy_data =
	          [[(id)tensor_data_class alloc] initWithMTLBuffer:dy_buffer shape:dy_shape dataType:MPSDataTypeFloat32];
	      MPSGraphTensorData *w_data =
	          [[(id)tensor_data_class alloc] initWithMTLBuffer:graph_w_buffer shape:w_shape dataType:MPSDataTypeFloat32];
	      MPSGraphTensorData *raw_data =
	          [[(id)tensor_data_class alloc] initWithMTLBuffer:raw_buffer shape:dx_shape dataType:MPSDataTypeFloat32];
	      if (dy_data == nil || w_data == nil || raw_data == nil) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
	      }
	      @try {
	        [graph runWithMTLCommandQueue:psyche_cuda_metal_queue
	                                feeds:@{ dy_tensor: dy_data, w_tensor: w_data }
	                     targetOperations:nil
	                    resultsDictionary:@{ raw_tensor: raw_data }];
	      } @catch (NSException *exception) {
	        (void)exception;
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_AFTER_GRAPH_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      result = psyche_cuda_metal_cudnn_mpsgraph_apply_buffer(
	          raw_buffer, dx_buffer, final_buffer, alpha, beta, total, threads_per_group);
	      if (result != CUDA_SUCCESS) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, result);
	      }
	      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
	              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_AFTER_EPILOGUE_FOR_TEST")) {
	        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
	      }
	      memcpy(out, [final_buffer contents], dx_bytes);
	      result = CUDA_SUCCESS;
	    } else {
	      result = psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
	    }
	  }
	  return result;
	}

	static CUresult psyche_cuda_metal_launch_cudnn_convolution_backward_filter_mpsgraph_f32(
	    const float *x,
    const float *dy,
    const float *dw_in,
    float *out,
    float alpha,
    float beta,
    unsigned int mode,
    unsigned int groups,
    unsigned int n,
    unsigned int in_c,
    unsigned int in_h,
    unsigned int in_w,
    unsigned int out_c,
    unsigned int out_h,
    unsigned int out_w,
    unsigned int filter_h,
    unsigned int filter_w,
    unsigned int pad_h,
    unsigned int pad_w,
    unsigned int stride_h,
    unsigned int stride_w,
    unsigned int dilation_h,
    unsigned int dilation_w,
    unsigned int total,
    size_t x_bytes,
    size_t dy_bytes,
    size_t dw_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX,
    int required) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> dy_buffer = nil;
  id<MTLBuffer> dw_buffer = nil;
  id<MTLBuffer> raw_buffer = nil;
  id<MTLBuffer> final_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  unsigned int channels_per_group = 0;
  @autoreleasepool {
    if (@available(macOS 11.0, *)) {
      Class graph_class = NSClassFromString(@"MPSGraph");
      Class descriptor_class = NSClassFromString(@"MPSGraphConvolution2DOpDescriptor");
      Class tensor_data_class = NSClassFromString(@"MPSGraphTensorData");
      if (graph_class == Nil || descriptor_class == Nil || tensor_data_class == Nil) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
      }
      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_FOR_TEST")) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
      }
      if (
          x == 0 ||
          dy == 0 ||
          dw_in == 0 ||
          out == 0 ||
          mode > 1U ||
          groups == 0 ||
          in_c % groups != 0 ||
          out_c % groups != 0 ||
          total == 0 ||
          x_bytes == 0 ||
          dy_bytes == 0 ||
          dw_bytes == 0 ||
          gridDimX == 0 ||
          blockDimX == 0 ||
          (size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
      }
      total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
      threads_per_group = (NSUInteger)blockDimX;
      if (total_threads < (NSUInteger)total) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_INVALID_VALUE);
      }
      if (
          psyche_cuda_metal_cudnn_convolution_backward_filter_mpsgraph_apply_f32 == nil ||
          threads_per_group >
              psyche_cuda_metal_cudnn_convolution_backward_filter_mpsgraph_apply_f32.maxTotalThreadsPerThreadgroup) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
      }
      channels_per_group = in_c / groups;

      x_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:x
                      length:x_bytes
                     options:MTLResourceStorageModeShared];
      dy_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:dy
                      length:dy_bytes
                     options:MTLResourceStorageModeShared];
      /*
       * Do not optimize this into an in-place MPSGraph write into user dw.
       * beta accumulation needs the caller's prior dw, true-convolution needs
       * a spatial R/S flip, and required-mode failures must leave dw untouched.
       */
      if (beta != 0.0f) {
        dw_buffer = [psyche_cuda_metal_device
            newBufferWithBytes:dw_in
                        length:dw_bytes
                       options:MTLResourceStorageModeShared];
      } else {
        dw_buffer = [psyche_cuda_metal_device
            newBufferWithLength:dw_bytes
                        options:MTLResourceStorageModeShared];
      }
      raw_buffer = [psyche_cuda_metal_device
          newBufferWithLength:dw_bytes
                      options:MTLResourceStorageModeShared];
      final_buffer = [psyche_cuda_metal_device
          newBufferWithLength:dw_bytes
                      options:MTLResourceStorageModeShared];
      if (x_buffer == nil || dy_buffer == nil || dw_buffer == nil || raw_buffer == nil || final_buffer == nil) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
      }

      MPSShape *x_shape = @[ @(n), @(in_c), @(in_h), @(in_w) ];
      MPSShape *dy_shape = @[ @(n), @(out_c), @(out_h), @(out_w) ];
      MPSShape *dw_shape = @[ @(out_c), @(channels_per_group), @(filter_h), @(filter_w) ];
      MPSGraph *graph = (MPSGraph *)[graph_class new];
      if (graph == nil) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
      }
      MPSGraphTensor *x_tensor =
          [graph placeholderWithShape:x_shape dataType:MPSDataTypeFloat32 name:@"psyche_bwd_filter_x"];
      MPSGraphTensor *dy_tensor =
          [graph placeholderWithShape:dy_shape dataType:MPSDataTypeFloat32 name:@"psyche_bwd_filter_dy"];
      if (x_tensor == nil || dy_tensor == nil) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
      }
      MPSGraphConvolution2DOpDescriptor *descriptor =
          [(id)descriptor_class descriptorWithStrideInX:(NSUInteger)stride_w
                                              strideInY:(NSUInteger)stride_h
                                        dilationRateInX:(NSUInteger)dilation_w
                                        dilationRateInY:(NSUInteger)dilation_h
                                                 groups:(NSUInteger)groups
                                            paddingLeft:(NSUInteger)pad_w
                                           paddingRight:(NSUInteger)pad_w
                                             paddingTop:(NSUInteger)pad_h
                                          paddingBottom:(NSUInteger)pad_h
                                           paddingStyle:MPSGraphPaddingStyleExplicit
                                             dataLayout:MPSGraphTensorNamedDataLayoutNCHW
                                          weightsLayout:MPSGraphTensorNamedDataLayoutOIHW];
      if (descriptor == nil) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
      }
      MPSGraphTensor *raw_tensor =
          [graph convolution2DWeightsGradientWithIncomingGradientTensor:dy_tensor
                                                           sourceTensor:x_tensor
                                                            outputShape:dw_shape
                                           forwardConvolutionDescriptor:descriptor
                                                                   name:@"psyche_bwd_filter_raw_dw"];
      if (raw_tensor == nil) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
      }

      MPSGraphTensorData *x_data =
          [[(id)tensor_data_class alloc] initWithMTLBuffer:x_buffer shape:x_shape dataType:MPSDataTypeFloat32];
      MPSGraphTensorData *dy_data =
          [[(id)tensor_data_class alloc] initWithMTLBuffer:dy_buffer shape:dy_shape dataType:MPSDataTypeFloat32];
      MPSGraphTensorData *raw_data =
          [[(id)tensor_data_class alloc] initWithMTLBuffer:raw_buffer shape:dw_shape dataType:MPSDataTypeFloat32];
      if (x_data == nil || dy_data == nil || raw_data == nil) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
      }

      @try {
        [graph runWithMTLCommandQueue:psyche_cuda_metal_queue
                                feeds:@{ x_tensor: x_data, dy_tensor: dy_data }
                     targetOperations:nil
                    resultsDictionary:@{ raw_tensor: raw_data }];
      } @catch (NSException *exception) {
        (void)exception;
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
      }
      if (psyche_cuda_metal_cudnn_mpsgraph_test_hook_enabled(
              "PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH_FAIL_AFTER_GRAPH_FOR_TEST")) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
      }

      command_buffer = [psyche_cuda_metal_queue commandBuffer];
      if (command_buffer == nil) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
      }
      encoder = [command_buffer computeCommandEncoder];
      if (encoder == nil) {
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_OUT_OF_MEMORY);
      }
      [encoder setComputePipelineState:psyche_cuda_metal_cudnn_convolution_backward_filter_mpsgraph_apply_f32];
      [encoder setBuffer:raw_buffer offset:0 atIndex:0];
      [encoder setBuffer:dw_buffer offset:0 atIndex:1];
      [encoder setBuffer:final_buffer offset:0 atIndex:2];
      [encoder setBytes:&alpha length:sizeof(alpha) atIndex:3];
      [encoder setBytes:&beta length:sizeof(beta) atIndex:4];
      [encoder setBytes:&mode length:sizeof(mode) atIndex:5];
      [encoder setBytes:&out_c length:sizeof(out_c) atIndex:6];
      [encoder setBytes:&channels_per_group length:sizeof(channels_per_group) atIndex:7];
      [encoder setBytes:&filter_h length:sizeof(filter_h) atIndex:8];
      [encoder setBytes:&filter_w length:sizeof(filter_w) atIndex:9];
      [encoder setBytes:&total length:sizeof(total) atIndex:10];
      [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
      [encoder endEncoding];
      [command_buffer commit];
      [command_buffer waitUntilCompleted];
      if (command_buffer.error != nil || command_buffer.status != MTLCommandBufferStatusCompleted) {
        NSError *command_error = command_buffer.error;
        (void)command_error;
        return psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_UNKNOWN);
      }
      memcpy(out, [final_buffer contents], dw_bytes);
      result = CUDA_SUCCESS;
    } else {
      result = psyche_cuda_metal_cudnn_mpsgraph_failure_result(required, CUDA_ERROR_NOT_SUPPORTED);
    }
  }
  return result;
}

CUresult psyche_cuda_metal_launch_cudnn_convolution_backward_filter_f32(
    const float *x,
    const float *dy,
    const float *dw_in,
    float *out,
    float alpha,
    float beta,
    unsigned int mode,
    unsigned int groups,
    unsigned int n,
    unsigned int in_c,
    unsigned int in_h,
    unsigned int in_w,
    unsigned int out_c,
    unsigned int out_h,
    unsigned int out_w,
    unsigned int filter_h,
    unsigned int filter_w,
    unsigned int pad_h,
    unsigned int pad_w,
    unsigned int stride_h,
    unsigned int stride_w,
    unsigned int dilation_h,
    unsigned int dilation_w,
    unsigned int total,
    size_t x_bytes,
    size_t dy_bytes,
    size_t dw_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> dy_buffer = nil;
  id<MTLBuffer> dw_buffer = nil;
  id<MTLBuffer> partials_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  size_t x_total = 0;
  size_t dy_total = 0;
  size_t dw_total = 0;
  size_t reduction_steps = 0;
  size_t chunk_count = 0;
  size_t partial_count = 0;
  size_t partial_bytes = 0;
  unsigned int filter_c = 0;
  unsigned int reduction_steps_u = 0;
  unsigned int chunk_span_u = PSYCHE_CUDA_METAL_BWD_FILTER_CHUNK_SPAN;
  unsigned int chunk_count_u = 0;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  NSUInteger tiled_threads_per_group = PSYCHE_CUDA_METAL_BWD_FILTER_PARTIAL_THREADS;
  int mpsgraph_required = 0;
  BOOL use_tiled = NO;
  @autoreleasepool {
    if (
        x == 0 ||
        dy == 0 ||
        dw_in == 0 ||
        out == 0 ||
        mode > 1U ||
        groups == 0 ||
        n == 0 ||
        in_c == 0 ||
        in_h == 0 ||
        in_w == 0 ||
        out_c == 0 ||
        out_h == 0 ||
        out_w == 0 ||
        filter_h == 0 ||
        filter_w == 0 ||
        stride_h == 0 ||
        stride_w == 0 ||
        dilation_h == 0 ||
        dilation_w == 0 ||
        total == 0 ||
        x_bytes == 0 ||
        dy_bytes == 0 ||
        dw_bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (in_c % groups != 0 || out_c % groups != 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    filter_c = in_c / groups;
    if (
        (size_t)n > SIZE_MAX / (size_t)in_c ||
        (size_t)n * (size_t)in_c > SIZE_MAX / (size_t)in_h ||
        (size_t)n * (size_t)in_c * (size_t)in_h > SIZE_MAX / (size_t)in_w ||
        (size_t)n > SIZE_MAX / (size_t)out_c ||
        (size_t)n * (size_t)out_c > SIZE_MAX / (size_t)out_h ||
        (size_t)n * (size_t)out_c * (size_t)out_h > SIZE_MAX / (size_t)out_w ||
        (size_t)out_c > SIZE_MAX / (size_t)filter_c ||
        (size_t)out_c * (size_t)filter_c > SIZE_MAX / (size_t)filter_h ||
        (size_t)out_c * (size_t)filter_c * (size_t)filter_h > SIZE_MAX / (size_t)filter_w) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    x_total = (size_t)n * (size_t)in_c * (size_t)in_h * (size_t)in_w;
    dy_total = (size_t)n * (size_t)out_c * (size_t)out_h * (size_t)out_w;
    dw_total = (size_t)out_c * (size_t)filter_c * (size_t)filter_h * (size_t)filter_w;
    if ((size_t)n > SIZE_MAX / (size_t)out_h || (size_t)n * (size_t)out_h > SIZE_MAX / (size_t)out_w) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    reduction_steps = (size_t)n * (size_t)out_h * (size_t)out_w;
    if (
        x_total > SIZE_MAX / sizeof(float) ||
        dy_total > SIZE_MAX / sizeof(float) ||
        dw_total > SIZE_MAX / sizeof(float) ||
        reduction_steps > UINT_MAX ||
        x_bytes != x_total * sizeof(float) ||
        dy_bytes != dy_total * sizeof(float) ||
        dw_bytes != dw_total * sizeof(float) ||
        (size_t)total != dw_total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    reduction_steps_u = (unsigned int)reduction_steps;
    if (reduction_steps > (size_t)PSYCHE_CUDA_METAL_BWD_FILTER_PARTIAL_THREADS) {
      chunk_count = (reduction_steps + (size_t)PSYCHE_CUDA_METAL_BWD_FILTER_CHUNK_SPAN - 1U) /
                    (size_t)PSYCHE_CUDA_METAL_BWD_FILTER_CHUNK_SPAN;
      if (
          chunk_count <= UINT_MAX &&
          chunk_count > 0 &&
          chunk_count <= SIZE_MAX / dw_total &&
          chunk_count * dw_total <= SIZE_MAX / sizeof(float)) {
        partial_count = chunk_count * dw_total;
        partial_bytes = partial_count * sizeof(float);
        if (
            partial_bytes > 0 &&
            partial_bytes <= (size_t)PSYCHE_CUDA_METAL_BWD_FILTER_SCRATCH_CAP_BYTES) {
          chunk_count_u = (unsigned int)chunk_count;
          use_tiled = YES;
        }
      }
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_convolution_backward_filter_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    if (
        result == CUDA_SUCCESS &&
        use_tiled &&
        (tiled_threads_per_group > psyche_cuda_metal_cudnn_convolution_backward_filter_partial_f32.maxTotalThreadsPerThreadgroup ||
         tiled_threads_per_group > psyche_cuda_metal_cudnn_convolution_backward_filter_reduce_f32.maxTotalThreadsPerThreadgroup)) {
      use_tiled = NO;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    mpsgraph_required = psyche_cuda_metal_cudnn_mpsgraph_required();
    if (psyche_cuda_metal_cudnn_mpsgraph_enabled()) {
      result = psyche_cuda_metal_launch_cudnn_convolution_backward_filter_mpsgraph_f32(
          x,
          dy,
          dw_in,
          out,
          alpha,
          beta,
          mode,
          groups,
          n,
          in_c,
          in_h,
          in_w,
          out_c,
          out_h,
          out_w,
          filter_h,
          filter_w,
          pad_h,
          pad_w,
          stride_h,
          stride_w,
          dilation_h,
          dilation_w,
          total,
          x_bytes,
          dy_bytes,
          dw_bytes,
          gridDimX,
          blockDimX,
          mpsgraph_required);
      if (result == CUDA_SUCCESS || mpsgraph_required) {
        return result;
      }
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:x_bytes
                   options:MTLResourceStorageModeShared];
    dy_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:dy
                    length:dy_bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      dw_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:dw_in
                      length:dw_bytes
                     options:MTLResourceStorageModeShared];
    } else {
      dw_buffer = [psyche_cuda_metal_device
          newBufferWithLength:dw_bytes
                      options:MTLResourceStorageModeShared];
    }
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:dw_bytes
                    options:MTLResourceStorageModeShared];
    if (use_tiled) {
      partials_buffer = [psyche_cuda_metal_device
          newBufferWithLength:(NSUInteger)partial_bytes
                      options:MTLResourceStorageModeShared];
      if (partials_buffer == nil) {
        use_tiled = NO;
      }
    }
    if (x_buffer == nil || dy_buffer == nil || dw_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    if (use_tiled) {
      encoder = [command_buffer computeCommandEncoder];
      if (encoder == nil) {
        return CUDA_ERROR_OUT_OF_MEMORY;
      }
      [encoder setComputePipelineState:psyche_cuda_metal_cudnn_convolution_backward_filter_partial_f32];
      [encoder setBuffer:x_buffer offset:0 atIndex:0];
      [encoder setBuffer:dy_buffer offset:0 atIndex:1];
      [encoder setBuffer:partials_buffer offset:0 atIndex:2];
      [encoder setBytes:&mode length:sizeof(mode) atIndex:3];
      [encoder setBytes:&groups length:sizeof(groups) atIndex:4];
      [encoder setBytes:&in_c length:sizeof(in_c) atIndex:5];
      [encoder setBytes:&in_h length:sizeof(in_h) atIndex:6];
      [encoder setBytes:&in_w length:sizeof(in_w) atIndex:7];
      [encoder setBytes:&out_c length:sizeof(out_c) atIndex:8];
      [encoder setBytes:&out_h length:sizeof(out_h) atIndex:9];
      [encoder setBytes:&out_w length:sizeof(out_w) atIndex:10];
      [encoder setBytes:&filter_h length:sizeof(filter_h) atIndex:11];
      [encoder setBytes:&filter_w length:sizeof(filter_w) atIndex:12];
      [encoder setBytes:&pad_h length:sizeof(pad_h) atIndex:13];
      [encoder setBytes:&pad_w length:sizeof(pad_w) atIndex:14];
      [encoder setBytes:&stride_h length:sizeof(stride_h) atIndex:15];
      [encoder setBytes:&stride_w length:sizeof(stride_w) atIndex:16];
      [encoder setBytes:&dilation_h length:sizeof(dilation_h) atIndex:17];
      [encoder setBytes:&dilation_w length:sizeof(dilation_w) atIndex:18];
      [encoder setBytes:&total length:sizeof(total) atIndex:19];
      [encoder setBytes:&reduction_steps_u length:sizeof(reduction_steps_u) atIndex:20];
      [encoder setBytes:&chunk_span_u length:sizeof(chunk_span_u) atIndex:21];
      [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)total, (NSUInteger)chunk_count_u, 1)
          threadsPerThreadgroup:MTLSizeMake(tiled_threads_per_group, 1, 1)];
      [encoder endEncoding];

      encoder = [command_buffer computeCommandEncoder];
      if (encoder == nil) {
        return CUDA_ERROR_OUT_OF_MEMORY;
      }
      [encoder setComputePipelineState:psyche_cuda_metal_cudnn_convolution_backward_filter_reduce_f32];
      [encoder setBuffer:partials_buffer offset:0 atIndex:0];
      [encoder setBuffer:dw_buffer offset:0 atIndex:1];
      [encoder setBuffer:out_buffer offset:0 atIndex:2];
      [encoder setBytes:&alpha length:sizeof(alpha) atIndex:3];
      [encoder setBytes:&beta length:sizeof(beta) atIndex:4];
      [encoder setBytes:&total length:sizeof(total) atIndex:5];
      [encoder setBytes:&chunk_count_u length:sizeof(chunk_count_u) atIndex:6];
      [encoder dispatchThreads:MTLSizeMake((NSUInteger)total, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(tiled_threads_per_group, 1, 1)];
      [encoder endEncoding];
    } else {
      encoder = [command_buffer computeCommandEncoder];
      if (encoder == nil) {
        return CUDA_ERROR_OUT_OF_MEMORY;
      }
      [encoder setComputePipelineState:psyche_cuda_metal_cudnn_convolution_backward_filter_f32];
      [encoder setBuffer:x_buffer offset:0 atIndex:0];
      [encoder setBuffer:dy_buffer offset:0 atIndex:1];
      [encoder setBuffer:dw_buffer offset:0 atIndex:2];
      [encoder setBuffer:out_buffer offset:0 atIndex:3];
      [encoder setBytes:&alpha length:sizeof(alpha) atIndex:4];
      [encoder setBytes:&beta length:sizeof(beta) atIndex:5];
      [encoder setBytes:&mode length:sizeof(mode) atIndex:6];
      [encoder setBytes:&groups length:sizeof(groups) atIndex:7];
      [encoder setBytes:&n length:sizeof(n) atIndex:8];
      [encoder setBytes:&in_c length:sizeof(in_c) atIndex:9];
      [encoder setBytes:&in_h length:sizeof(in_h) atIndex:10];
      [encoder setBytes:&in_w length:sizeof(in_w) atIndex:11];
      [encoder setBytes:&out_c length:sizeof(out_c) atIndex:12];
      [encoder setBytes:&out_h length:sizeof(out_h) atIndex:13];
      [encoder setBytes:&out_w length:sizeof(out_w) atIndex:14];
      [encoder setBytes:&filter_h length:sizeof(filter_h) atIndex:15];
      [encoder setBytes:&filter_w length:sizeof(filter_w) atIndex:16];
      [encoder setBytes:&pad_h length:sizeof(pad_h) atIndex:17];
      [encoder setBytes:&pad_w length:sizeof(pad_w) atIndex:18];
      [encoder setBytes:&stride_h length:sizeof(stride_h) atIndex:19];
      [encoder setBytes:&stride_w length:sizeof(stride_w) atIndex:20];
      [encoder setBytes:&dilation_h length:sizeof(dilation_h) atIndex:21];
      [encoder setBytes:&dilation_w length:sizeof(dilation_w) atIndex:22];
      [encoder setBytes:&total length:sizeof(total) atIndex:23];
      [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
      [encoder endEncoding];
    }
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], dw_bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cudnn_softmax_f32(
    const float *x,
    const float *y_in,
    float *out,
    float alpha,
    float beta,
    unsigned int algorithm,
    unsigned int mode,
    unsigned int n,
    unsigned int c,
    unsigned int h,
    unsigned int w,
    unsigned int vector_count,
    unsigned int vector_len,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (
        x == 0 ||
        y_in == 0 ||
        out == 0 ||
        algorithm > 2U ||
        mode > 1U ||
        n == 0 ||
        c == 0 ||
        h == 0 ||
        w == 0 ||
        vector_count == 0 ||
        vector_len == 0 ||
        bytes == 0 ||
        gridDimX == 0 ||
        blockDimX != PSYCHE_CUDA_METAL_REDUCTION_THREADS) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)n > SIZE_MAX / (size_t)c) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)n * (size_t)c > SIZE_MAX / (size_t)h) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)n * (size_t)c * (size_t)h > SIZE_MAX / (size_t)w) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (bytes != (size_t)n * (size_t)c * (size_t)h * (size_t)w * sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (gridDimX < vector_count) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_softmax_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:y_in
                      length:bytes
                     options:MTLResourceStorageModeShared];
    } else {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithLength:bytes
                      options:MTLResourceStorageModeShared];
    }
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:bytes
                    options:MTLResourceStorageModeShared];
    if (x_buffer == nil || y_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_softmax_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:y_buffer offset:0 atIndex:1];
    [encoder setBuffer:out_buffer offset:0 atIndex:2];
    [encoder setBytes:&alpha length:sizeof(alpha) atIndex:3];
    [encoder setBytes:&beta length:sizeof(beta) atIndex:4];
    [encoder setBytes:&algorithm length:sizeof(algorithm) atIndex:5];
    [encoder setBytes:&mode length:sizeof(mode) atIndex:6];
    [encoder setBytes:&n length:sizeof(n) atIndex:7];
    [encoder setBytes:&c length:sizeof(c) atIndex:8];
    [encoder setBytes:&h length:sizeof(h) atIndex:9];
    [encoder setBytes:&w length:sizeof(w) atIndex:10];
    [encoder setBytes:&vector_count length:sizeof(vector_count) atIndex:11];
    [encoder setBytes:&vector_len length:sizeof(vector_len) atIndex:12];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cudnn_softmax_backward_f32(
    const float *y,
    const float *dy,
    const float *dx_in,
    float *out,
    float alpha,
    float beta,
    unsigned int algorithm,
    unsigned int mode,
    unsigned int n,
    unsigned int c,
    unsigned int h,
    unsigned int w,
    unsigned int vector_count,
    unsigned int vector_len,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> dy_buffer = nil;
  id<MTLBuffer> dx_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (
        y == 0 ||
        dy == 0 ||
        dx_in == 0 ||
        out == 0 ||
        algorithm > 2U ||
        mode > 1U ||
        n == 0 ||
        c == 0 ||
        h == 0 ||
        w == 0 ||
        vector_count == 0 ||
        vector_len == 0 ||
        bytes == 0 ||
        gridDimX == 0 ||
        blockDimX != PSYCHE_CUDA_METAL_REDUCTION_THREADS) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)n > SIZE_MAX / (size_t)c) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)n * (size_t)c > SIZE_MAX / (size_t)h) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)n * (size_t)c * (size_t)h > SIZE_MAX / (size_t)w) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (bytes != (size_t)n * (size_t)c * (size_t)h * (size_t)w * sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (gridDimX < vector_count) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_softmax_backward_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    y_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:y
                    length:bytes
                   options:MTLResourceStorageModeShared];
    dy_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:dy
                    length:bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      dx_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:dx_in
                      length:bytes
                     options:MTLResourceStorageModeShared];
    } else {
      dx_buffer = [psyche_cuda_metal_device
          newBufferWithLength:bytes
                      options:MTLResourceStorageModeShared];
    }
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:bytes
                    options:MTLResourceStorageModeShared];
    if (y_buffer == nil || dy_buffer == nil || dx_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_softmax_backward_f32];
    [encoder setBuffer:y_buffer offset:0 atIndex:0];
    [encoder setBuffer:dy_buffer offset:0 atIndex:1];
    [encoder setBuffer:dx_buffer offset:0 atIndex:2];
    [encoder setBuffer:out_buffer offset:0 atIndex:3];
    [encoder setBytes:&alpha length:sizeof(alpha) atIndex:4];
    [encoder setBytes:&beta length:sizeof(beta) atIndex:5];
    [encoder setBytes:&algorithm length:sizeof(algorithm) atIndex:6];
    [encoder setBytes:&mode length:sizeof(mode) atIndex:7];
    [encoder setBytes:&n length:sizeof(n) atIndex:8];
    [encoder setBytes:&c length:sizeof(c) atIndex:9];
    [encoder setBytes:&h length:sizeof(h) atIndex:10];
    [encoder setBytes:&w length:sizeof(w) atIndex:11];
    [encoder setBytes:&vector_count length:sizeof(vector_count) atIndex:12];
    [encoder setBytes:&vector_len length:sizeof(vector_len) atIndex:13];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cudnn_pooling_f32(
    const float *x,
    const float *y_in,
    float *out,
    float alpha,
    float beta,
    unsigned int mode,
    unsigned int nan_opt,
    unsigned int n,
    unsigned int c,
    unsigned int in_h,
    unsigned int in_w,
    unsigned int out_h,
    unsigned int out_w,
    unsigned int window_h,
    unsigned int window_w,
    unsigned int pad_h,
    unsigned int pad_w,
    unsigned int stride_h,
    unsigned int stride_w,
    unsigned int total,
    size_t x_bytes,
    size_t y_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  int pad_h_i = (int)pad_h;
  int pad_w_i = (int)pad_w;
  @autoreleasepool {
    if (
        x == 0 ||
        y_in == 0 ||
        out == 0 ||
        mode > 3U ||
        nan_opt > 1U ||
        n == 0 ||
        c == 0 ||
        in_h == 0 ||
        in_w == 0 ||
        out_h == 0 ||
        out_w == 0 ||
        window_h == 0 ||
        window_w == 0 ||
        stride_h == 0 ||
        stride_w == 0 ||
        total == 0 ||
        x_bytes == 0 ||
        y_bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)total > SIZE_MAX / sizeof(float) || y_bytes != (size_t)total * sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_pooling_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:x_bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:y_in
                      length:y_bytes
                     options:MTLResourceStorageModeShared];
    } else {
      y_buffer = [psyche_cuda_metal_device
          newBufferWithLength:y_bytes
                      options:MTLResourceStorageModeShared];
    }
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:y_bytes
                    options:MTLResourceStorageModeShared];
    if (x_buffer == nil || y_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_pooling_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:y_buffer offset:0 atIndex:1];
    [encoder setBuffer:out_buffer offset:0 atIndex:2];
    [encoder setBytes:&alpha length:sizeof(alpha) atIndex:3];
    [encoder setBytes:&beta length:sizeof(beta) atIndex:4];
    [encoder setBytes:&mode length:sizeof(mode) atIndex:5];
    [encoder setBytes:&nan_opt length:sizeof(nan_opt) atIndex:6];
    [encoder setBytes:&n length:sizeof(n) atIndex:7];
    [encoder setBytes:&c length:sizeof(c) atIndex:8];
    [encoder setBytes:&in_h length:sizeof(in_h) atIndex:9];
    [encoder setBytes:&in_w length:sizeof(in_w) atIndex:10];
    [encoder setBytes:&out_h length:sizeof(out_h) atIndex:11];
    [encoder setBytes:&out_w length:sizeof(out_w) atIndex:12];
    [encoder setBytes:&window_h length:sizeof(window_h) atIndex:13];
    [encoder setBytes:&window_w length:sizeof(window_w) atIndex:14];
    [encoder setBytes:&pad_h_i length:sizeof(pad_h_i) atIndex:15];
    [encoder setBytes:&pad_w_i length:sizeof(pad_w_i) atIndex:16];
    [encoder setBytes:&stride_h length:sizeof(stride_h) atIndex:17];
    [encoder setBytes:&stride_w length:sizeof(stride_w) atIndex:18];
    [encoder setBytes:&total length:sizeof(total) atIndex:19];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], y_bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cudnn_pooling_backward_f32(
    const float *x,
    const float *dy,
    const float *dx_in,
    float *out,
    float alpha,
    float beta,
    unsigned int mode,
    unsigned int nan_opt,
    unsigned int n,
    unsigned int c,
    unsigned int in_h,
    unsigned int in_w,
    unsigned int out_h,
    unsigned int out_w,
    unsigned int window_h,
    unsigned int window_w,
    unsigned int pad_h,
    unsigned int pad_w,
    unsigned int stride_h,
    unsigned int stride_w,
    unsigned int total,
    size_t dy_bytes,
    size_t dx_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> dy_buffer = nil;
  id<MTLBuffer> dx_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  int pad_h_i = (int)pad_h;
  int pad_w_i = (int)pad_w;
  int is_average = mode == 1U || mode == 2U;
  @autoreleasepool {
    if (
        dy == 0 ||
        dx_in == 0 ||
        out == 0 ||
        (!is_average && x == 0) ||
        mode > 3U ||
        nan_opt > 1U ||
        n == 0 ||
        c == 0 ||
        in_h == 0 ||
        in_w == 0 ||
        out_h == 0 ||
        out_w == 0 ||
        window_h == 0 ||
        window_w == 0 ||
        stride_h == 0 ||
        stride_w == 0 ||
        total == 0 ||
        dy_bytes == 0 ||
        dx_bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (
        (size_t)total > SIZE_MAX / sizeof(float) ||
        dx_bytes != (size_t)total * sizeof(float) ||
        (size_t)n > SIZE_MAX / (size_t)c ||
        (size_t)n * (size_t)c > SIZE_MAX / (size_t)out_h ||
        (size_t)n * (size_t)c * (size_t)out_h > SIZE_MAX / (size_t)out_w ||
        dy_bytes != (size_t)n * (size_t)c * (size_t)out_h * (size_t)out_w * sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)total) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cudnn_pooling_backward_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    if (is_average) {
      x_buffer = [psyche_cuda_metal_device
          newBufferWithLength:dx_bytes
                      options:MTLResourceStorageModeShared];
    } else {
      x_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:x
                      length:dx_bytes
                     options:MTLResourceStorageModeShared];
    }
    dy_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:dy
                    length:dy_bytes
                   options:MTLResourceStorageModeShared];
    if (beta != 0.0f) {
      dx_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:dx_in
                      length:dx_bytes
                     options:MTLResourceStorageModeShared];
    } else {
      dx_buffer = [psyche_cuda_metal_device
          newBufferWithLength:dx_bytes
                      options:MTLResourceStorageModeShared];
    }
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:dx_bytes
                    options:MTLResourceStorageModeShared];
    if (x_buffer == nil || dy_buffer == nil || dx_buffer == nil || out_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cudnn_pooling_backward_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:dy_buffer offset:0 atIndex:1];
    [encoder setBuffer:dx_buffer offset:0 atIndex:2];
    [encoder setBuffer:out_buffer offset:0 atIndex:3];
    [encoder setBytes:&alpha length:sizeof(alpha) atIndex:4];
    [encoder setBytes:&beta length:sizeof(beta) atIndex:5];
    [encoder setBytes:&mode length:sizeof(mode) atIndex:6];
    [encoder setBytes:&nan_opt length:sizeof(nan_opt) atIndex:7];
    [encoder setBytes:&n length:sizeof(n) atIndex:8];
    [encoder setBytes:&c length:sizeof(c) atIndex:9];
    [encoder setBytes:&in_h length:sizeof(in_h) atIndex:10];
    [encoder setBytes:&in_w length:sizeof(in_w) atIndex:11];
    [encoder setBytes:&out_h length:sizeof(out_h) atIndex:12];
    [encoder setBytes:&out_w length:sizeof(out_w) atIndex:13];
    [encoder setBytes:&window_h length:sizeof(window_h) atIndex:14];
    [encoder setBytes:&window_w length:sizeof(window_w) atIndex:15];
    [encoder setBytes:&pad_h_i length:sizeof(pad_h_i) atIndex:16];
    [encoder setBytes:&pad_w_i length:sizeof(pad_w_i) atIndex:17];
    [encoder setBytes:&stride_h length:sizeof(stride_h) atIndex:18];
    [encoder setBytes:&stride_w length:sizeof(stride_w) atIndex:19];
    [encoder setBytes:&total length:sizeof(total) atIndex:20];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error != nil) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out, [out_buffer contents], dx_bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_dot_f32(
    const float *x,
    const float *y,
    float *result_out,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> partials_a = nil;
  id<MTLBuffer> partials_b = nil;
  id<MTLBuffer> n_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  id<MTLBuffer> read_partials = nil;
  id<MTLBuffer> write_partials = nil;
  NSMutableArray<id<MTLBuffer>> *count_buffers = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  NSUInteger partial_count = 0;
  NSUInteger partial_bytes = 0;
  size_t expected_bytes = 0;
  size_t expected_grid_dim_x = 0;
  @autoreleasepool {
    if (
        x == 0 ||
        y == 0 ||
        result_out == 0 ||
        n == 0 ||
        bytes == 0 ||
        gridDimX == 0 ||
        blockDimX != PSYCHE_CUDA_METAL_REDUCTION_THREADS) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / sizeof(float)) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    if ((size_t)n > SIZE_MAX / sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    expected_bytes = (size_t)n * sizeof(float);
    expected_grid_dim_x =
        ((size_t)n + (size_t)PSYCHE_CUDA_METAL_REDUCTION_THREADS - 1U) /
        (size_t)PSYCHE_CUDA_METAL_REDUCTION_THREADS;
    if (bytes != expected_bytes || (size_t)gridDimX != expected_grid_dim_x) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    partial_count = (NSUInteger)gridDimX;
    partial_bytes = (NSUInteger)((size_t)gridDimX * sizeof(float));
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        (threads_per_group > psyche_cuda_metal_dot_f32.maxTotalThreadsPerThreadgroup ||
         threads_per_group > psyche_cuda_metal_sum_f32.maxTotalThreadsPerThreadgroup)) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:expected_bytes
                   options:MTLResourceStorageModeShared];
    y_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:y
                    length:expected_bytes
                   options:MTLResourceStorageModeShared];
    partials_a = [psyche_cuda_metal_device
        newBufferWithLength:partial_bytes
                    options:MTLResourceStorageModeShared];
    partials_b = [psyche_cuda_metal_device
        newBufferWithLength:partial_bytes
                    options:MTLResourceStorageModeShared];
    n_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&n
                    length:sizeof(n)
                   options:MTLResourceStorageModeShared];
    if (x_buffer == nil || y_buffer == nil || partials_a == nil || partials_b == nil || n_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_dot_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:y_buffer offset:0 atIndex:1];
    [encoder setBuffer:partials_a offset:0 atIndex:2];
    [encoder setBuffer:n_buffer offset:0 atIndex:3];
    [encoder dispatchThreadgroups:MTLSizeMake(partial_count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    read_partials = partials_a;
    write_partials = partials_b;
    count_buffers = [NSMutableArray array];
    while (partial_count > 1) {
      unsigned int count = 0;
      NSUInteger stage_groups = (partial_count + threads_per_group - 1U) / threads_per_group;
      id<MTLBuffer> count_buffer = nil;
      id<MTLBuffer> swap = nil;
      if (partial_count > UINT_MAX) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      count = (unsigned int)partial_count;
      count_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:&count
                      length:sizeof(count)
                     options:MTLResourceStorageModeShared];
      if (count_buffer == nil) {
        return CUDA_ERROR_OUT_OF_MEMORY;
      }
      [count_buffers addObject:count_buffer];
      encoder = [command_buffer computeCommandEncoder];
      if (encoder == nil) {
        return CUDA_ERROR_OUT_OF_MEMORY;
      }
      [encoder setComputePipelineState:psyche_cuda_metal_sum_f32];
      [encoder setBuffer:read_partials offset:0 atIndex:0];
      [encoder setBuffer:write_partials offset:0 atIndex:1];
      [encoder setBuffer:count_buffer offset:0 atIndex:2];
      [encoder dispatchThreadgroups:MTLSizeMake(stage_groups, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
      [encoder endEncoding];
      partial_count = stage_groups;
      swap = read_partials;
      read_partials = write_partials;
      write_partials = swap;
    }
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    *result_out = ((float *)[read_partials contents])[0];
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_asum_f32(
    const float *x,
    float *result_out,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> partials_a = nil;
  id<MTLBuffer> partials_b = nil;
  id<MTLBuffer> n_buffer = nil;
  id<MTLBuffer> read_partials = nil;
  id<MTLBuffer> write_partials = nil;
  NSMutableArray *count_buffers = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  NSUInteger partial_count = 0;
  NSUInteger partial_bytes = 0;
  size_t expected_bytes = 0;
  size_t expected_grid_dim_x = 0;
  @autoreleasepool {
    if (
        x == 0 ||
        result_out == 0 ||
        n == 0 ||
        bytes == 0 ||
        gridDimX == 0 ||
        blockDimX != PSYCHE_CUDA_METAL_REDUCTION_THREADS) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / sizeof(float)) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    if ((size_t)n > SIZE_MAX / sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    expected_bytes = (size_t)n * sizeof(float);
    expected_grid_dim_x =
        ((size_t)n + (size_t)PSYCHE_CUDA_METAL_REDUCTION_THREADS - 1U) /
        (size_t)PSYCHE_CUDA_METAL_REDUCTION_THREADS;
    if (bytes != expected_bytes || (size_t)gridDimX != expected_grid_dim_x) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    partial_count = (NSUInteger)gridDimX;
    partial_bytes = (NSUInteger)((size_t)gridDimX * sizeof(float));
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        (threads_per_group > psyche_cuda_metal_abs_sum_f32.maxTotalThreadsPerThreadgroup ||
         threads_per_group > psyche_cuda_metal_sum_f32.maxTotalThreadsPerThreadgroup)) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:expected_bytes
                   options:MTLResourceStorageModeShared];
    partials_a = [psyche_cuda_metal_device
        newBufferWithLength:partial_bytes
                    options:MTLResourceStorageModeShared];
    partials_b = [psyche_cuda_metal_device
        newBufferWithLength:partial_bytes
                    options:MTLResourceStorageModeShared];
    n_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&n
                    length:sizeof(n)
                   options:MTLResourceStorageModeShared];
    if (x_buffer == nil || partials_a == nil || partials_b == nil || n_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_abs_sum_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:partials_a offset:0 atIndex:1];
    [encoder setBuffer:n_buffer offset:0 atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake(partial_count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    read_partials = partials_a;
    write_partials = partials_b;
    count_buffers = [NSMutableArray array];
    while (partial_count > 1) {
      unsigned int count = 0;
      NSUInteger stage_groups = (partial_count + threads_per_group - 1U) / threads_per_group;
      id<MTLBuffer> count_buffer = nil;
      id<MTLBuffer> swap = nil;
      if (partial_count > UINT_MAX) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      count = (unsigned int)partial_count;
      count_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:&count
                      length:sizeof(count)
                     options:MTLResourceStorageModeShared];
      if (count_buffer == nil) {
        return CUDA_ERROR_OUT_OF_MEMORY;
      }
      [count_buffers addObject:count_buffer];
      encoder = [command_buffer computeCommandEncoder];
      if (encoder == nil) {
        return CUDA_ERROR_OUT_OF_MEMORY;
      }
      [encoder setComputePipelineState:psyche_cuda_metal_sum_f32];
      [encoder setBuffer:read_partials offset:0 atIndex:0];
      [encoder setBuffer:write_partials offset:0 atIndex:1];
      [encoder setBuffer:count_buffer offset:0 atIndex:2];
      [encoder dispatchThreadgroups:MTLSizeMake(stage_groups, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
      [encoder endEncoding];
      partial_count = stage_groups;
      swap = read_partials;
      read_partials = write_partials;
      write_partials = swap;
    }
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    *result_out = ((float *)[read_partials contents])[0];
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_nrm2_f32(
    const float *x,
    float *result_out,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> partials_a = nil;
  id<MTLBuffer> partials_b = nil;
  id<MTLBuffer> n_buffer = nil;
  id<MTLBuffer> read_partials = nil;
  id<MTLBuffer> write_partials = nil;
  NSMutableArray *count_buffers = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  NSUInteger partial_count = 0;
  NSUInteger partial_bytes = 0;
  size_t expected_bytes = 0;
  size_t expected_grid_dim_x = 0;
  @autoreleasepool {
    if (
        x == 0 ||
        result_out == 0 ||
        n == 0 ||
        bytes == 0 ||
        gridDimX == 0 ||
        blockDimX != PSYCHE_CUDA_METAL_REDUCTION_THREADS) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (sizeof(float) * 2U)) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    if ((size_t)n > SIZE_MAX / sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    expected_bytes = (size_t)n * sizeof(float);
    expected_grid_dim_x =
        ((size_t)n + (size_t)PSYCHE_CUDA_METAL_REDUCTION_THREADS - 1U) /
        (size_t)PSYCHE_CUDA_METAL_REDUCTION_THREADS;
    if (bytes != expected_bytes || (size_t)gridDimX != expected_grid_dim_x) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    partial_count = (NSUInteger)gridDimX;
    partial_bytes = (NSUInteger)((size_t)gridDimX * sizeof(float) * 2U);
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        (threads_per_group > psyche_cuda_metal_nrm2_pair_f32.maxTotalThreadsPerThreadgroup ||
         threads_per_group > psyche_cuda_metal_nrm2_combine_f32.maxTotalThreadsPerThreadgroup)) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:expected_bytes
                   options:MTLResourceStorageModeShared];
    partials_a = [psyche_cuda_metal_device
        newBufferWithLength:partial_bytes
                    options:MTLResourceStorageModeShared];
    partials_b = [psyche_cuda_metal_device
        newBufferWithLength:partial_bytes
                    options:MTLResourceStorageModeShared];
    n_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&n
                    length:sizeof(n)
                   options:MTLResourceStorageModeShared];
    if (x_buffer == nil || partials_a == nil || partials_b == nil || n_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_nrm2_pair_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:partials_a offset:0 atIndex:1];
    [encoder setBuffer:n_buffer offset:0 atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake(partial_count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    read_partials = partials_a;
    write_partials = partials_b;
    count_buffers = [NSMutableArray array];
    while (partial_count > 1) {
      unsigned int count = 0;
      NSUInteger stage_groups = (partial_count + threads_per_group - 1U) / threads_per_group;
      id<MTLBuffer> count_buffer = nil;
      id<MTLBuffer> swap = nil;
      if (partial_count > UINT_MAX) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      count = (unsigned int)partial_count;
      count_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:&count
                      length:sizeof(count)
                     options:MTLResourceStorageModeShared];
      if (count_buffer == nil) {
        return CUDA_ERROR_OUT_OF_MEMORY;
      }
      [count_buffers addObject:count_buffer];
      encoder = [command_buffer computeCommandEncoder];
      if (encoder == nil) {
        return CUDA_ERROR_OUT_OF_MEMORY;
      }
      [encoder setComputePipelineState:psyche_cuda_metal_nrm2_combine_f32];
      [encoder setBuffer:read_partials offset:0 atIndex:0];
      [encoder setBuffer:write_partials offset:0 atIndex:1];
      [encoder setBuffer:count_buffer offset:0 atIndex:2];
      [encoder dispatchThreadgroups:MTLSizeMake(stage_groups, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
      [encoder endEncoding];
      partial_count = stage_groups;
      swap = read_partials;
      read_partials = write_partials;
      write_partials = swap;
    }
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    {
      float *final_pair = (float *)[read_partials contents];
      float scale = final_pair[0];
      float ssq = final_pair[1];
      *result_out = scale == 0.0f ? 0.0f : scale * sqrtf(ssq);
    }
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_sgemv_f32(
    const float *A,
    const float *x,
    const float *y,
    float *out_y,
    float alpha,
    float beta,
    unsigned int n,
    unsigned int lda,
    unsigned int trans,
    unsigned int input_len,
    unsigned int output_len,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  static const float dummy_value = 0.0f;
  id<MTLBuffer> a_buffer = nil;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_in_buffer = nil;
  id<MTLBuffer> y_out_buffer = nil;
  id<MTLBuffer> dummy_buffer = nil;
  id<MTLBuffer> alpha_buffer = nil;
  id<MTLBuffer> beta_buffer = nil;
  id<MTLBuffer> lda_buffer = nil;
  id<MTLBuffer> trans_buffer = nil;
  id<MTLBuffer> input_len_buffer = nil;
  id<MTLBuffer> output_len_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  size_t a_bytes = sizeof(float);
  size_t x_bytes = sizeof(float);
  size_t y_bytes = 0;
  int needs_product = alpha != 0.0f && input_len > 0;
  int needs_y_in = beta != 0.0f;
  @autoreleasepool {
    if (y == 0 || out_y == 0 || output_len == 0 || gridDimX == 0 || blockDimX == 0 || lda == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (needs_product && (A == 0 || x == 0)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)output_len > SIZE_MAX / sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    y_bytes = (size_t)output_len * sizeof(float);
    if (needs_product) {
      size_t a_elements = 0;
      if ((size_t)lda > SIZE_MAX / (size_t)n) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      a_elements = (size_t)lda * (size_t)n;
      if (a_elements > SIZE_MAX / sizeof(float) || (size_t)input_len > SIZE_MAX / sizeof(float)) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      a_bytes = a_elements * sizeof(float);
      x_bytes = (size_t)input_len * sizeof(float);
      if (a_bytes == 0 || x_bytes == 0) {
        return CUDA_ERROR_INVALID_VALUE;
      }
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)output_len) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (result == CUDA_SUCCESS && threads_per_group > psyche_cuda_metal_sgemv_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    dummy_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&dummy_value
                    length:sizeof(dummy_value)
                   options:MTLResourceStorageModeShared];
    if (dummy_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    a_buffer = needs_product
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)A
                          length:a_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    x_buffer = needs_product
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)x
                          length:x_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    y_in_buffer = needs_y_in
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)y
                          length:y_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    y_out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:y_bytes
                    options:MTLResourceStorageModeShared];
    alpha_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&alpha
                    length:sizeof(alpha)
                   options:MTLResourceStorageModeShared];
    beta_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&beta
                    length:sizeof(beta)
                   options:MTLResourceStorageModeShared];
    lda_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&lda
                    length:sizeof(lda)
                   options:MTLResourceStorageModeShared];
    trans_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&trans
                    length:sizeof(trans)
                   options:MTLResourceStorageModeShared];
    input_len_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&input_len
                    length:sizeof(input_len)
                   options:MTLResourceStorageModeShared];
    output_len_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&output_len
                    length:sizeof(output_len)
                   options:MTLResourceStorageModeShared];
    if (
        a_buffer == nil ||
        x_buffer == nil ||
        y_in_buffer == nil ||
        y_out_buffer == nil ||
        alpha_buffer == nil ||
        beta_buffer == nil ||
        lda_buffer == nil ||
        trans_buffer == nil ||
        input_len_buffer == nil ||
        output_len_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_sgemv_f32];
    [encoder setBuffer:a_buffer offset:0 atIndex:0];
    [encoder setBuffer:x_buffer offset:0 atIndex:1];
    [encoder setBuffer:y_in_buffer offset:0 atIndex:2];
    [encoder setBuffer:y_out_buffer offset:0 atIndex:3];
    [encoder setBuffer:alpha_buffer offset:0 atIndex:4];
    [encoder setBuffer:beta_buffer offset:0 atIndex:5];
    [encoder setBuffer:lda_buffer offset:0 atIndex:6];
    [encoder setBuffer:trans_buffer offset:0 atIndex:7];
    [encoder setBuffer:input_len_buffer offset:0 atIndex:8];
    [encoder setBuffer:output_len_buffer offset:0 atIndex:9];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out_y, [y_out_buffer contents], y_bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_sger_f32(
    const float *x,
    const float *y,
    float *A,
    float alpha,
    unsigned int m,
    unsigned int n,
    unsigned int lda,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> a_in_buffer = nil;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> a_out_buffer = nil;
  id<MTLBuffer> alpha_buffer = nil;
  id<MTLBuffer> m_buffer = nil;
  id<MTLBuffer> lda_buffer = nil;
  id<MTLBuffer> total_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  size_t update_elems = 0;
  size_t a_elems = 0;
  size_t x_bytes = 0;
  size_t y_bytes = 0;
  size_t a_bytes = 0;
  @autoreleasepool {
    if (x == 0 || y == 0 || A == 0 || m == 0 || n == 0 || lda == 0 || gridDimX == 0 || blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)m > SIZE_MAX / (size_t)n || (size_t)lda > SIZE_MAX / (size_t)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    update_elems = (size_t)m * (size_t)n;
    a_elems = (size_t)lda * (size_t)n;
    if (
        update_elems > UINT_MAX ||
        a_elems > UINT_MAX ||
        (size_t)m > SIZE_MAX / sizeof(float) ||
        (size_t)n > SIZE_MAX / sizeof(float) ||
        a_elems > SIZE_MAX / sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    x_bytes = (size_t)m * sizeof(float);
    y_bytes = (size_t)n * sizeof(float);
    a_bytes = a_elems * sizeof(float);
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)update_elems) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (result == CUDA_SUCCESS && threads_per_group > psyche_cuda_metal_sger_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    a_in_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:(const void *)(uintptr_t)A
                    length:a_bytes
                   options:MTLResourceStorageModeShared];
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:(const void *)(uintptr_t)x
                    length:x_bytes
                   options:MTLResourceStorageModeShared];
    y_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:(const void *)(uintptr_t)y
                    length:y_bytes
                   options:MTLResourceStorageModeShared];
    a_out_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:(const void *)(uintptr_t)A
                    length:a_bytes
                   options:MTLResourceStorageModeShared];
    alpha_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&alpha
                    length:sizeof(alpha)
                   options:MTLResourceStorageModeShared];
    m_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&m
                    length:sizeof(m)
                   options:MTLResourceStorageModeShared];
    lda_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&lda
                    length:sizeof(lda)
                   options:MTLResourceStorageModeShared];
    {
      unsigned int total = (unsigned int)update_elems;
      total_buffer = [psyche_cuda_metal_device
          newBufferWithBytes:&total
                      length:sizeof(total)
                     options:MTLResourceStorageModeShared];
    }
    if (
        a_in_buffer == nil ||
        x_buffer == nil ||
        y_buffer == nil ||
        a_out_buffer == nil ||
        alpha_buffer == nil ||
        m_buffer == nil ||
        lda_buffer == nil ||
        total_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_sger_f32];
    [encoder setBuffer:a_in_buffer offset:0 atIndex:0];
    [encoder setBuffer:x_buffer offset:0 atIndex:1];
    [encoder setBuffer:y_buffer offset:0 atIndex:2];
    [encoder setBuffer:a_out_buffer offset:0 atIndex:3];
    [encoder setBuffer:alpha_buffer offset:0 atIndex:4];
    [encoder setBuffer:m_buffer offset:0 atIndex:5];
    [encoder setBuffer:lda_buffer offset:0 atIndex:6];
    [encoder setBuffer:total_buffer offset:0 atIndex:7];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(A, [a_out_buffer contents], a_bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cusparse_spmv_csr_f32(
    const int32_t *row_offsets,
    const int32_t *col_indices,
    const float *values,
    const float *x,
    const float *y_in,
    float *out_y,
    float alpha,
    float beta,
    unsigned int rows,
    unsigned int cols,
    unsigned int nnz,
    int index_base,
    size_t row_offsets_bytes,
    size_t col_indices_bytes,
    size_t values_bytes,
    size_t x_bytes,
    size_t y_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> dummy_buffer = nil;
  id<MTLBuffer> row_offsets_buffer = nil;
  id<MTLBuffer> col_indices_buffer = nil;
  id<MTLBuffer> values_buffer = nil;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_in_buffer = nil;
  id<MTLBuffer> y_out_buffer = nil;
  id<MTLBuffer> alpha_buffer = nil;
  id<MTLBuffer> beta_buffer = nil;
  id<MTLBuffer> rows_buffer = nil;
  id<MTLBuffer> cols_buffer = nil;
  id<MTLBuffer> nnz_buffer = nil;
  id<MTLBuffer> index_base_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  int dummy_value = 0;
  @autoreleasepool {
    if (
        row_offsets == 0 ||
        out_y == 0 ||
        rows == 0 ||
        row_offsets_bytes == 0 ||
        y_bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0 ||
        (index_base != 0 && index_base != 1)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (alpha != 0.0f && nnz != 0 && (col_indices == 0 || values == 0 || x == 0)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (beta != 0.0f && y_in == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)rows) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cusparse_spmv_csr_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    dummy_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&dummy_value
                    length:sizeof(dummy_value)
                   options:MTLResourceStorageModeShared];
    if (dummy_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    row_offsets_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:(const void *)(uintptr_t)row_offsets
                    length:row_offsets_bytes
                   options:MTLResourceStorageModeShared];
    col_indices_buffer = col_indices_bytes != 0
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)col_indices
                          length:col_indices_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    values_buffer = values_bytes != 0
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)values
                          length:values_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    x_buffer = x_bytes != 0
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)x
                          length:x_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    y_in_buffer = beta != 0.0f
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)y_in
                          length:y_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    y_out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:y_bytes
                    options:MTLResourceStorageModeShared];
    alpha_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&alpha
                    length:sizeof(alpha)
                   options:MTLResourceStorageModeShared];
    beta_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&beta
                    length:sizeof(beta)
                   options:MTLResourceStorageModeShared];
    rows_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&rows
                    length:sizeof(rows)
                   options:MTLResourceStorageModeShared];
    cols_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&cols
                    length:sizeof(cols)
                   options:MTLResourceStorageModeShared];
    nnz_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&nnz
                    length:sizeof(nnz)
                   options:MTLResourceStorageModeShared];
    index_base_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&index_base
                    length:sizeof(index_base)
                   options:MTLResourceStorageModeShared];
    if (
        row_offsets_buffer == nil ||
        col_indices_buffer == nil ||
        values_buffer == nil ||
        x_buffer == nil ||
        y_in_buffer == nil ||
        y_out_buffer == nil ||
        alpha_buffer == nil ||
        beta_buffer == nil ||
        rows_buffer == nil ||
        cols_buffer == nil ||
        nnz_buffer == nil ||
        index_base_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cusparse_spmv_csr_f32];
    [encoder setBuffer:row_offsets_buffer offset:0 atIndex:0];
    [encoder setBuffer:col_indices_buffer offset:0 atIndex:1];
    [encoder setBuffer:values_buffer offset:0 atIndex:2];
    [encoder setBuffer:x_buffer offset:0 atIndex:3];
    [encoder setBuffer:y_in_buffer offset:0 atIndex:4];
    [encoder setBuffer:y_out_buffer offset:0 atIndex:5];
    [encoder setBuffer:alpha_buffer offset:0 atIndex:6];
    [encoder setBuffer:beta_buffer offset:0 atIndex:7];
    [encoder setBuffer:rows_buffer offset:0 atIndex:8];
    [encoder setBuffer:cols_buffer offset:0 atIndex:9];
    [encoder setBuffer:nnz_buffer offset:0 atIndex:10];
    [encoder setBuffer:index_base_buffer offset:0 atIndex:11];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (
        getenv("PSYCHE_CUDA_COMPAT_CUSPARSE_METAL_FAIL_AFTER_DISPATCH_FOR_TEST") != 0 &&
        strcasecmp(getenv("PSYCHE_CUDA_COMPAT_CUSPARSE_METAL_FAIL_AFTER_DISPATCH_FOR_TEST"), "0") != 0) {
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(out_y, [y_out_buffer contents], y_bytes);
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_cusparse_spmm_csr_f32(
    const int32_t *row_offsets,
    const int32_t *col_indices,
    const float *values,
    const float *b,
    const float *c_in,
    float *out_c,
    float alpha,
    float beta,
    unsigned int rows,
    unsigned int cols,
    unsigned int nnz,
    unsigned int n,
    unsigned int b_ld,
    unsigned int c_ld,
    unsigned int b_order,
    unsigned int c_order,
    int index_base,
    size_t row_offsets_bytes,
    size_t col_indices_bytes,
    size_t values_bytes,
    size_t b_bytes,
    size_t c_bytes,
    size_t output_bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> dummy_buffer = nil;
  id<MTLBuffer> row_offsets_buffer = nil;
  id<MTLBuffer> col_indices_buffer = nil;
  id<MTLBuffer> values_buffer = nil;
  id<MTLBuffer> b_buffer = nil;
  id<MTLBuffer> c_in_buffer = nil;
  id<MTLBuffer> out_buffer = nil;
  id<MTLBuffer> alpha_buffer = nil;
  id<MTLBuffer> beta_buffer = nil;
  id<MTLBuffer> rows_buffer = nil;
  id<MTLBuffer> cols_buffer = nil;
  id<MTLBuffer> nnz_buffer = nil;
  id<MTLBuffer> n_buffer = nil;
  id<MTLBuffer> b_ld_buffer = nil;
  id<MTLBuffer> c_ld_buffer = nil;
  id<MTLBuffer> b_order_buffer = nil;
  id<MTLBuffer> c_order_buffer = nil;
  id<MTLBuffer> index_base_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  size_t output_elements = 0;
  unsigned int row = 0;
  unsigned int col = 0;
  int dummy_value = 0;
  @autoreleasepool {
    if (
        row_offsets == 0 ||
        out_c == 0 ||
        rows == 0 ||
        n == 0 ||
        row_offsets_bytes == 0 ||
        output_bytes == 0 ||
        gridDimX == 0 ||
        blockDimX == 0 ||
        (index_base != 0 && index_base != 1) ||
        (b_order != CUSPARSE_ORDER_COL && b_order != CUSPARSE_ORDER_ROW) ||
        (c_order != CUSPARSE_ORDER_COL && c_order != CUSPARSE_ORDER_ROW)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)rows > SIZE_MAX / (size_t)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    output_elements = (size_t)rows * (size_t)n;
    if (output_elements > UINT_MAX || output_bytes != output_elements * sizeof(float)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (alpha != 0.0f && nnz != 0 && (col_indices == 0 || values == 0 || b == 0)) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (beta != 0.0f && c_in == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)output_elements) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (
        result == CUDA_SUCCESS &&
        threads_per_group > psyche_cuda_metal_cusparse_spmm_csr_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    dummy_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&dummy_value
                    length:sizeof(dummy_value)
                   options:MTLResourceStorageModeShared];
    if (dummy_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    row_offsets_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:(const void *)(uintptr_t)row_offsets
                    length:row_offsets_bytes
                   options:MTLResourceStorageModeShared];
    col_indices_buffer = col_indices_bytes != 0
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)col_indices
                          length:col_indices_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    values_buffer = values_bytes != 0
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)values
                          length:values_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    b_buffer = b_bytes != 0
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)b
                          length:b_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    /* Host pointer mode gives us the scalar value here: -0.0 skips C, while NaN reads and propagates prior C. */
    c_in_buffer = beta != 0.0f
        ? [psyche_cuda_metal_device
              newBufferWithBytes:(const void *)(uintptr_t)c_in
                          length:c_bytes
                         options:MTLResourceStorageModeShared]
        : dummy_buffer;
    out_buffer = [psyche_cuda_metal_device
        newBufferWithLength:output_bytes
                    options:MTLResourceStorageModeShared];
    alpha_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&alpha
                    length:sizeof(alpha)
                   options:MTLResourceStorageModeShared];
    beta_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&beta
                    length:sizeof(beta)
                   options:MTLResourceStorageModeShared];
    rows_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&rows
                    length:sizeof(rows)
                   options:MTLResourceStorageModeShared];
    cols_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&cols
                    length:sizeof(cols)
                   options:MTLResourceStorageModeShared];
    nnz_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&nnz
                    length:sizeof(nnz)
                   options:MTLResourceStorageModeShared];
    n_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&n
                    length:sizeof(n)
                   options:MTLResourceStorageModeShared];
    b_ld_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&b_ld
                    length:sizeof(b_ld)
                   options:MTLResourceStorageModeShared];
    c_ld_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&c_ld
                    length:sizeof(c_ld)
                   options:MTLResourceStorageModeShared];
    b_order_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&b_order
                    length:sizeof(b_order)
                   options:MTLResourceStorageModeShared];
    c_order_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&c_order
                    length:sizeof(c_order)
                   options:MTLResourceStorageModeShared];
    index_base_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&index_base
                    length:sizeof(index_base)
                   options:MTLResourceStorageModeShared];
    if (
        row_offsets_buffer == nil ||
        col_indices_buffer == nil ||
        values_buffer == nil ||
        b_buffer == nil ||
        c_in_buffer == nil ||
        out_buffer == nil ||
        alpha_buffer == nil ||
        beta_buffer == nil ||
        rows_buffer == nil ||
        cols_buffer == nil ||
        nnz_buffer == nil ||
        n_buffer == nil ||
        b_ld_buffer == nil ||
        c_ld_buffer == nil ||
        b_order_buffer == nil ||
        c_order_buffer == nil ||
        index_base_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_cusparse_spmm_csr_f32];
    [encoder setBuffer:row_offsets_buffer offset:0 atIndex:0];
    [encoder setBuffer:col_indices_buffer offset:0 atIndex:1];
    [encoder setBuffer:values_buffer offset:0 atIndex:2];
    [encoder setBuffer:b_buffer offset:0 atIndex:3];
    [encoder setBuffer:c_in_buffer offset:0 atIndex:4];
    [encoder setBuffer:out_buffer offset:0 atIndex:5];
    [encoder setBuffer:alpha_buffer offset:0 atIndex:6];
    [encoder setBuffer:beta_buffer offset:0 atIndex:7];
    [encoder setBuffer:rows_buffer offset:0 atIndex:8];
    [encoder setBuffer:cols_buffer offset:0 atIndex:9];
    [encoder setBuffer:nnz_buffer offset:0 atIndex:10];
    [encoder setBuffer:n_buffer offset:0 atIndex:11];
    [encoder setBuffer:b_ld_buffer offset:0 atIndex:12];
    [encoder setBuffer:c_ld_buffer offset:0 atIndex:13];
    [encoder setBuffer:b_order_buffer offset:0 atIndex:14];
    [encoder setBuffer:c_order_buffer offset:0 atIndex:15];
    [encoder setBuffer:index_base_buffer offset:0 atIndex:16];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    if (
        getenv("PSYCHE_CUDA_COMPAT_CUSPARSE_METAL_FAIL_AFTER_DISPATCH_FOR_TEST") != 0 &&
        strcasecmp(getenv("PSYCHE_CUDA_COMPAT_CUSPARSE_METAL_FAIL_AFTER_DISPATCH_FOR_TEST"), "0") != 0) {
      return CUDA_ERROR_UNKNOWN;
    }
    const float *logical = (const float *)[out_buffer contents];
    for (row = 0; row < rows; row++) {
      for (col = 0; col < n; col++) {
        size_t c_index = c_order == CUSPARSE_ORDER_COL
            ? (size_t)row + (size_t)col * (size_t)c_ld
            : (size_t)row * (size_t)c_ld + (size_t)col;
        out_c[c_index] = logical[(size_t)row * (size_t)n + (size_t)col];
      }
    }
  }
  return CUDA_SUCCESS;
}

CUresult psyche_cuda_metal_launch_axpby_f32(
    float *x,
    const float *y,
    float alpha,
    float beta,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX) {
  CUresult result = CUDA_SUCCESS;
  id<MTLBuffer> x_buffer = nil;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> alpha_buffer = nil;
  id<MTLBuffer> beta_buffer = nil;
  id<MTLBuffer> n_buffer = nil;
  id<MTLCommandBuffer> command_buffer = nil;
  id<MTLComputeCommandEncoder> encoder = nil;
  NSUInteger total_threads = 0;
  NSUInteger threads_per_group = 0;
  @autoreleasepool {
    if (x == 0 || y == 0 || n == 0 || bytes == 0 || gridDimX == 0 || blockDimX == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    total_threads = (NSUInteger)((size_t)gridDimX * (size_t)blockDimX);
    threads_per_group = (NSUInteger)blockDimX;
    if (total_threads < (NSUInteger)n) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_lock(&psyche_cuda_metal_mutex);
    result = psyche_cuda_metal_init_locked();
    if (result == CUDA_SUCCESS && threads_per_group > psyche_cuda_metal_axpby_f32.maxTotalThreadsPerThreadgroup) {
      result = CUDA_ERROR_INVALID_VALUE;
    }
    pthread_mutex_unlock(&psyche_cuda_metal_mutex);
    if (result != CUDA_SUCCESS) {
      return result;
    }
    x_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:x
                    length:bytes
                   options:MTLResourceStorageModeShared];
    y_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:(const void *)(uintptr_t)y
                    length:bytes
                   options:MTLResourceStorageModeShared];
    alpha_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&alpha
                    length:sizeof(alpha)
                   options:MTLResourceStorageModeShared];
    beta_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&beta
                    length:sizeof(beta)
                   options:MTLResourceStorageModeShared];
    n_buffer = [psyche_cuda_metal_device
        newBufferWithBytes:&n
                    length:sizeof(n)
                   options:MTLResourceStorageModeShared];
    if (x_buffer == nil || y_buffer == nil || alpha_buffer == nil || beta_buffer == nil || n_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    command_buffer = [psyche_cuda_metal_queue commandBuffer];
    if (command_buffer == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return CUDA_ERROR_OUT_OF_MEMORY;
    }
    [encoder setComputePipelineState:psyche_cuda_metal_axpby_f32];
    [encoder setBuffer:x_buffer offset:0 atIndex:0];
    [encoder setBuffer:y_buffer offset:0 atIndex:1];
    [encoder setBuffer:alpha_buffer offset:0 atIndex:2];
    [encoder setBuffer:beta_buffer offset:0 atIndex:3];
    [encoder setBuffer:n_buffer offset:0 atIndex:4];
    [encoder dispatchThreads:MTLSizeMake(total_threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      NSError *command_error = command_buffer.error;
      (void)command_error;
      return CUDA_ERROR_UNKNOWN;
    }
    memcpy(x, [x_buffer contents], bytes);
  }
  return CUDA_SUCCESS;
}
