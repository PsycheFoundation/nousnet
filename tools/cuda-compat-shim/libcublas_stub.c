#define _POSIX_C_SOURCE 200809L

#include "cuda_compat_stub.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(__APPLE__)
#define ACCELERATE_NEW_LAPACK 1
#include <Accelerate/Accelerate.h>
_Static_assert(sizeof(cuComplex) == sizeof(__LAPACK_float_complex), "cuComplex must match Accelerate float-complex ABI size");
_Static_assert(sizeof(cuDoubleComplex) == sizeof(__LAPACK_double_complex), "cuDoubleComplex must match Accelerate double-complex ABI size");
_Static_assert(_Alignof(cuComplex) == _Alignof(__LAPACK_float_complex), "cuComplex must match Accelerate float-complex ABI alignment");
_Static_assert(_Alignof(cuDoubleComplex) == _Alignof(__LAPACK_double_complex), "cuDoubleComplex must match Accelerate double-complex ABI alignment");
#endif

#define PSYCHE_CUBLAS_HANDLE_MAGIC 0x70737963626c6173ULL
#define PSYCHE_CUBLAS_VERSION 0

typedef struct PsycheCublasContext {
  uint64_t magic;
  cudaStream_t stream;
  cublasPointerMode_t pointer_mode;
  cublasMath_t math_mode;
  cublasAtomicsMode_t atomics_mode;
  struct PsycheCublasContext *next;
} PsycheCublasContext;

static pthread_mutex_t psyche_cublas_handle_mutex = PTHREAD_MUTEX_INITIALIZER;
static PsycheCublasContext *psyche_cublas_handles = 0;

#if defined(__APPLE__)
CUresult psyche_cuda_metal_launch_saxpy_f32(
    const float *x,
    float *y,
    float alpha,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX);
CUresult psyche_cuda_metal_launch_scale_f32(
    float *x,
    float alpha,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX);
CUresult psyche_cuda_metal_launch_copy_f32(
    const float *x,
    float *y,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX);
CUresult psyche_cuda_metal_launch_dot_f32(
    const float *x,
    const float *y,
    float *result_out,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX);
CUresult psyche_cuda_metal_launch_asum_f32(
    const float *x,
    float *result_out,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX);
CUresult psyche_cuda_metal_launch_nrm2_f32(
    const float *x,
    float *result_out,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX);
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
    unsigned int blockDimX);
CUresult psyche_cuda_metal_launch_sger_f32(
    const float *x,
    const float *y,
    float *A,
    float alpha,
    unsigned int m,
    unsigned int n,
    unsigned int lda,
    unsigned int gridDimX,
    unsigned int blockDimX);
#endif

static int psyche_cublas_env_truthy(const char *value) {
  if (value == 0 || value[0] == '\0') {
    return 0;
  }
  if (
      strcmp(value, "1") == 0 ||
      strcasecmp(value, "true") == 0 ||
      strcasecmp(value, "yes") == 0 ||
      strcasecmp(value, "on") == 0) {
    return 1;
  }
  return 0;
}

static int psyche_cublas_env_required(const char *value) {
  return value != 0 && strcasecmp(value, "required") == 0;
}

static int psyche_cublas_simulated_memory_enabled(void) {
  return psyche_cublas_env_truthy(getenv("PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY"));
}

static int psyche_cublas_metal_enabled(void) {
  const char *value = getenv("PSYCHE_CUDA_COMPAT_CUBLAS_METAL");
  return psyche_cublas_env_truthy(value) || psyche_cublas_env_required(value);
}

static int psyche_cublas_metal_required(void) {
  return psyche_cublas_env_required(getenv("PSYCHE_CUDA_COMPAT_CUBLAS_METAL"));
}

static cublasStatus_t psyche_cublas_status_from_cuda_result(CUresult result) {
  switch (result) {
  case CUDA_SUCCESS:
    return CUBLAS_STATUS_SUCCESS;
  case CUDA_ERROR_INVALID_VALUE:
  case CUDA_ERROR_INVALID_HANDLE:
    return CUBLAS_STATUS_INVALID_VALUE;
  case CUDA_ERROR_OUT_OF_MEMORY:
    return CUBLAS_STATUS_ALLOC_FAILED;
  case CUDA_ERROR_NOT_INITIALIZED:
  case CUDA_ERROR_NO_DEVICE:
  case CUDA_ERROR_INVALID_DEVICE:
  case CUDA_ERROR_NOT_SUPPORTED:
    return CUBLAS_STATUS_NOT_SUPPORTED;
  case CUDA_ERROR_UNKNOWN:
    return CUBLAS_STATUS_EXECUTION_FAILED;
  default:
    return CUBLAS_STATUS_INTERNAL_ERROR;
  }
}

static int psyche_cublas_metal_preferred_can_fallback(CUresult result) {
  return
      result == CUDA_ERROR_NOT_SUPPORTED ||
      result == CUDA_ERROR_NO_DEVICE ||
      result == CUDA_ERROR_NOT_INITIALIZED;
}

#if defined(__APPLE__)
static enum CBLAS_SIDE psyche_cublas_accelerate_side(cublasSideMode_t side) {
  return side == CUBLAS_SIDE_LEFT ? CblasLeft : CblasRight;
}

static enum CBLAS_UPLO psyche_cublas_accelerate_uplo(cublasFillMode_t uplo) {
  return uplo == CUBLAS_FILL_MODE_LOWER ? CblasLower : CblasUpper;
}

/* cuBLAS OP_T is non-conjugating transpose; OP_C is conjugate transpose. */
static enum CBLAS_TRANSPOSE psyche_cublas_accelerate_trans(cublasOperation_t trans) {
  switch (trans) {
  case CUBLAS_OP_N:
    return CblasNoTrans;
  case CUBLAS_OP_T:
    return CblasTrans;
  case CUBLAS_OP_C:
    return CblasConjTrans;
  default:
    return CblasNoTrans;
  }
}

static enum CBLAS_DIAG psyche_cublas_accelerate_diag(cublasDiagType_t diag) {
  return diag == CUBLAS_DIAG_UNIT ? CblasUnit : CblasNonUnit;
}
#endif

static cublasStatus_t psyche_cublas_contiguous_f32_launch_shape(
    int n,
    unsigned int block_dim_x,
    size_t *bytes,
    unsigned int *grid_dim_x) {
  size_t grid_dim_x_size = 0;
  if (n <= 0 || block_dim_x == 0 || bytes == 0 || grid_dim_x == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if ((size_t)n > SIZE_MAX / sizeof(float)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *bytes = (size_t)n * sizeof(float);
  grid_dim_x_size = ((size_t)n + (size_t)block_dim_x - 1u) / (size_t)block_dim_x;
  if (grid_dim_x_size > UINT_MAX) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *grid_dim_x = (unsigned int)grid_dim_x_size;
  return CUBLAS_STATUS_SUCCESS;
}

static int psyche_cublas_f32_ranges_partially_overlap(
    const float *x,
    const float *y,
    size_t bytes) {
  uintptr_t x_begin = 0;
  uintptr_t y_begin = 0;
  uintptr_t x_end = 0;
  uintptr_t y_end = 0;
  if (x == y || bytes == 0) {
    return 0;
  }
  x_begin = (uintptr_t)x;
  y_begin = (uintptr_t)y;
  if (x_begin > UINTPTR_MAX - bytes || y_begin > UINTPTR_MAX - bytes) {
    return 1;
  }
  x_end = x_begin + bytes;
  y_end = y_begin + bytes;
  return x_begin < y_end && y_begin < x_end;
}

static PsycheCublasContext *psyche_cublas_context(cublasHandle_t handle) {
  PsycheCublasContext *record = 0;
  pthread_mutex_lock(&psyche_cublas_handle_mutex);
  record = psyche_cublas_handles;
  while (record != 0) {
    if ((cublasHandle_t)record == handle) {
      break;
    }
    record = record->next;
  }
  if (record != 0 && record->magic != PSYCHE_CUBLAS_HANDLE_MAGIC) {
    record = 0;
  }
  pthread_mutex_unlock(&psyche_cublas_handle_mutex);
  return record;
}

static void psyche_cublas_register_context(PsycheCublasContext *ctx) {
  pthread_mutex_lock(&psyche_cublas_handle_mutex);
  ctx->next = psyche_cublas_handles;
  psyche_cublas_handles = ctx;
  pthread_mutex_unlock(&psyche_cublas_handle_mutex);
}

static PsycheCublasContext *psyche_cublas_unregister_context(cublasHandle_t handle) {
  PsycheCublasContext **link = 0;
  PsycheCublasContext *record = 0;
  pthread_mutex_lock(&psyche_cublas_handle_mutex);
  link = &psyche_cublas_handles;
  while (*link != 0 && (cublasHandle_t)(*link) != handle) {
    link = &(*link)->next;
  }
  if (*link != 0) {
    record = *link;
    *link = record->next;
    record->next = 0;
  }
  pthread_mutex_unlock(&psyche_cublas_handle_mutex);
  return record;
}

static int psyche_cublas_valid_op(cublasOperation_t op) {
  return op == CUBLAS_OP_N || op == CUBLAS_OP_T || op == CUBLAS_OP_C;
}

static int psyche_cublas_valid_uplo(cublasFillMode_t uplo) {
  return uplo == CUBLAS_FILL_MODE_LOWER || uplo == CUBLAS_FILL_MODE_UPPER;
}

static int psyche_cublas_valid_side(cublasSideMode_t side) {
  return side == CUBLAS_SIDE_LEFT || side == CUBLAS_SIDE_RIGHT;
}

static int psyche_cublas_valid_diag(cublasDiagType_t diag) {
  return diag == CUBLAS_DIAG_NON_UNIT || diag == CUBLAS_DIAG_UNIT;
}

static int psyche_cublas_max_int(int a, int b) {
  return a > b ? a : b;
}

static int psyche_cublas_math_mode_known(cublasMath_t mode) {
  int base = (int)mode & ~CUBLAS_MATH_DISALLOW_REDUCED_PRECISION_REDUCTION;
  return
      base == CUBLAS_DEFAULT_MATH ||
      base == CUBLAS_TENSOR_OP_MATH ||
      base == CUBLAS_PEDANTIC_MATH ||
      base == CUBLAS_TF32_TENSOR_OP_MATH;
}

static int psyche_cublas_math_mode_supported_for_gemm(cublasMath_t mode) {
  int base = (int)mode & ~CUBLAS_MATH_DISALLOW_REDUCED_PRECISION_REDUCTION;
  return base == CUBLAS_DEFAULT_MATH || base == CUBLAS_PEDANTIC_MATH;
}

static cublasStatus_t psyche_cublas_validate_handle(
    cublasHandle_t handle,
    int require_host_pointer_mode) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (require_host_pointer_mode && ctx->pointer_mode != CUBLAS_POINTER_MODE_HOST) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_positive_stride(int n, int inc) {
  if (n < 0 || (n > 0 && inc <= 0)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_abs_stride(int inc, size_t *abs_inc) {
  int64_t magnitude = 0;
  if (abs_inc == 0 || inc == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  magnitude = inc > 0 ? (int64_t)inc : -(int64_t)inc;
  if ((uint64_t)magnitude > (uint64_t)SIZE_MAX) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *abs_inc = (size_t)magnitude;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_signed_stride(int n, int inc) {
  size_t abs_inc = 0;
  if (n < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return psyche_cublas_abs_stride(inc, &abs_inc);
}

static cublasStatus_t psyche_cublas_temp_bytes(
    int m,
    int n,
    size_t element_size,
    size_t *bytes) {
  size_t count = 0;
  if (bytes == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *bytes = 0;
  if (m < 0 || n < 0 || element_size == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (m == 0 || n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if ((size_t)n > SIZE_MAX / (size_t)m) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  count = (size_t)m * (size_t)n;
  if (count > SIZE_MAX / element_size) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  *bytes = count * element_size;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_strided_span(int n, int inc) {
  if (n <= 1) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if ((size_t)(n - 1) > (SIZE_MAX - 1U) / (size_t)inc) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_signed_strided_span(int n, int inc) {
  size_t abs_inc = 0;
  cublasStatus_t status = psyche_cublas_abs_stride(inc, &abs_inc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n <= 1) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if ((size_t)(n - 1) > (SIZE_MAX - 1U) / abs_inc) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static size_t psyche_cublas_signed_stride_index(int len, int inc, int i) {
  size_t abs_inc = 0;
  cublasStatus_t status = psyche_cublas_abs_stride(inc, &abs_inc);
  assert(status == CUBLAS_STATUS_SUCCESS);
  assert(i >= 0 && i < len);
  (void)status;
  /* For inc < 0, x points at storage[0] and logical element 0 lives at the high end. */
  return inc > 0
      ? (size_t)i * abs_inc
      : (size_t)(len - 1 - i) * abs_inc;
}

static cublasStatus_t psyche_cublas_validate_vector_byte_span(int n, int elemSize, int inc) {
  size_t stride_bytes = 0;
  if (n <= 1) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if ((size_t)inc > SIZE_MAX / (size_t)elemSize) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  stride_bytes = (size_t)inc * (size_t)elemSize;
  if ((size_t)(n - 1) > (SIZE_MAX - ((size_t)elemSize - 1U)) / stride_bytes) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_matrix_span(int rows, int cols, int ld) {
  size_t last_row = 0;
  size_t last_col = 0;
  if (rows <= 0 || cols <= 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  last_row = (size_t)(rows - 1);
  last_col = (size_t)(cols - 1);
  if (last_col > (SIZE_MAX - last_row) / (size_t)ld) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_matrix_byte_span(int rows, int cols, int elemSize, int ld) {
  cublasStatus_t status = psyche_cublas_validate_matrix_span(rows, cols, ld);
  size_t last_index = 0;
  if (status != CUBLAS_STATUS_SUCCESS || rows <= 0 || cols <= 0) {
    return status;
  }
  last_index = (size_t)(rows - 1) + (size_t)(cols - 1) * (size_t)ld;
  if (last_index > (SIZE_MAX - ((size_t)elemSize - 1U)) / (size_t)elemSize) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_transfer_env(void) {
  return psyche_cublas_simulated_memory_enabled() ? CUBLAS_STATUS_SUCCESS : CUBLAS_STATUS_NOT_INITIALIZED;
}

static cublasStatus_t psyche_cublas_copy_vector_bytes(
    int n,
    int elemSize,
    const void *x,
    int incx,
    void *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_transfer_env();
  unsigned char *tmp = 0;
  const unsigned char *src = (const unsigned char *)x;
  unsigned char *dst = (unsigned char *)y;
  size_t tmp_bytes = 0;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n < 0 || elemSize <= 0 || incx <= 0 || incy <= 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_vector_byte_span(n, elemSize, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_vector_byte_span(n, elemSize, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 1, (size_t)elemSize, &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (unsigned char *)malloc(tmp_bytes);
  if (tmp == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  /* Stage into a packed buffer so overlapping host pointers are deterministic. */
  for (i = 0; i < n; i++) {
    memcpy(
        &tmp[(size_t)i * (size_t)elemSize],
        &src[(size_t)i * (size_t)incx * (size_t)elemSize],
        (size_t)elemSize);
  }
  for (i = 0; i < n; i++) {
    memcpy(
        &dst[(size_t)i * (size_t)incy * (size_t)elemSize],
        &tmp[(size_t)i * (size_t)elemSize],
        (size_t)elemSize);
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_copy_matrix_bytes(
    int rows,
    int cols,
    int elemSize,
    const void *A,
    int lda,
    void *B,
    int ldb) {
  cublasStatus_t status = psyche_cublas_validate_transfer_env();
  unsigned char *tmp = 0;
  const unsigned char *src = (const unsigned char *)A;
  unsigned char *dst = (unsigned char *)B;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (rows < 0 || cols < 0 || elemSize <= 0 || lda <= 0 || ldb <= 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (rows > 0 && (lda < rows || ldb < rows)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_matrix_byte_span(rows, cols, elemSize, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_byte_span(rows, cols, elemSize, ldb);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (rows == 0 || cols == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (A == 0 || B == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(rows, cols, (size_t)elemSize, &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (unsigned char *)malloc(tmp_bytes);
  if (tmp == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  /* Stage into a packed column-major tile so overlapping host pointers are deterministic. */
  for (col = 0; col < cols; col++) {
    for (row = 0; row < rows; row++) {
      memcpy(
          &tmp[((size_t)row + (size_t)col * (size_t)rows) * (size_t)elemSize],
          &src[((size_t)row + (size_t)col * (size_t)lda) * (size_t)elemSize],
          (size_t)elemSize);
    }
  }
  for (col = 0; col < cols; col++) {
    for (row = 0; row < rows; row++) {
      memcpy(
          &dst[((size_t)row + (size_t)col * (size_t)ldb) * (size_t)elemSize],
          &tmp[((size_t)row + (size_t)col * (size_t)rows) * (size_t)elemSize],
          (size_t)elemSize);
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static void psyche_cublas_szero_matrix(int rows, int cols, float *matrix, int ld) {
  int row = 0;
  int col = 0;
  for (col = 0; col < cols; col++) {
    for (row = 0; row < rows; row++) {
      matrix[(size_t)row + (size_t)col * (size_t)ld] = 0.0f;
    }
  }
}

static void psyche_cublas_dzero_matrix(int rows, int cols, double *matrix, int ld) {
  int row = 0;
  int col = 0;
  for (col = 0; col < cols; col++) {
    for (row = 0; row < rows; row++) {
      matrix[(size_t)row + (size_t)col * (size_t)ld] = 0.0;
    }
  }
}

static cublasStatus_t psyche_cublas_validate_batch_stride(int batchCount, long long stride, int allow_zero) {
  if (stride < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (stride == 0) {
    return allow_zero || batchCount <= 1 ? CUBLAS_STATUS_SUCCESS : CUBLAS_STATUS_INVALID_VALUE;
  }
  if (batchCount <= 1) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if ((unsigned long long)stride > (unsigned long long)PTRDIFF_MAX / (unsigned long long)(batchCount - 1)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_gemm_args(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const void *alpha,
    const void *A,
    int lda,
    const void *B,
    int ldb,
    const void *beta,
    void *C,
    int ldc) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  int min_lda = 0;
  int min_ldb = 0;
  int min_ldc = 0;
  (void)A;
  (void)B;
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (ctx->pointer_mode != CUBLAS_POINTER_MODE_HOST) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cublas_math_mode_supported_for_gemm(ctx->math_mode)) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cublas_valid_op(transa) || !psyche_cublas_valid_op(transb)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (m < 0 || n < 0 || k < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  min_lda = transa == CUBLAS_OP_N ? psyche_cublas_max_int(1, m) : psyche_cublas_max_int(1, k);
  min_ldb = transb == CUBLAS_OP_N ? psyche_cublas_max_int(1, k) : psyche_cublas_max_int(1, n);
  min_ldc = psyche_cublas_max_int(1, m);
  if (lda < min_lda || ldb < min_ldb || ldc < min_ldc) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha == NULL || beta == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (m == 0 || n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (C == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_gemm_strided_batched_args(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const void *alpha,
    const void *A,
    int lda,
    long long strideA,
    const void *B,
    int ldb,
    long long strideB,
    const void *beta,
    void *C,
    int ldc,
    long long strideC,
    int batchCount) {
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  void *validation_C = C;
  if (batchCount < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (batchCount == 0 && C == NULL) {
    validation_C = (void *)1;
  }
  status = psyche_cublas_validate_gemm_args(
      handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, validation_C, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_batch_stride(batchCount, strideA, 1);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_batch_stride(batchCount, strideB, 1);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  return psyche_cublas_validate_batch_stride(batchCount, strideC, 0);
}

static cublasStatus_t psyche_cublas_validate_gemm_batched_args(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const void *alpha,
    int lda,
    int ldb,
    const void *beta,
    const void *Carray,
    int ldc,
    int batchCount) {
  void *validation_C = (void *)Carray;
  if (batchCount < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (batchCount == 0 && Carray == NULL) {
    /* Validate handle, modes, dimensions, and scalars without requiring work buffers for a zero-batch no-op. */
    validation_C = (void *)1;
  }
  return psyche_cublas_validate_gemm_args(
      handle, transa, transb, m, n, k, alpha, NULL, lda, NULL, ldb, beta, validation_C, ldc);
}

static cublasStatus_t psyche_cublas_validate_symm_args(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    int m,
    int n,
    const void *alpha,
    const void *A,
    int lda,
    const void *B,
    int ldb,
    const void *beta,
    void *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  int a_dim = 0;
  (void)A;
  (void)B;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!psyche_cublas_valid_side(side) || !psyche_cublas_valid_uplo(uplo) || m < 0 || n < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  a_dim = side == CUBLAS_SIDE_LEFT ? m : n;
  if (
      lda < psyche_cublas_max_int(1, a_dim) ||
      ldb < psyche_cublas_max_int(1, m) ||
      ldc < psyche_cublas_max_int(1, m) ||
      alpha == NULL ||
      beta == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_matrix_span(a_dim, a_dim, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(m, n, ldb);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(m, n, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (m > 0 && n > 0 && C == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_syrk_args(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const void *alpha,
    const void *A,
    int lda,
    const void *beta,
    void *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  int a_rows = 0;
  int a_cols = 0;
  (void)A;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!psyche_cublas_valid_uplo(uplo) || !psyche_cublas_valid_op(trans) || n < 0 || k < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  a_rows = trans == CUBLAS_OP_N ? n : k;
  a_cols = trans == CUBLAS_OP_N ? k : n;
  if (
      lda < psyche_cublas_max_int(1, a_rows) ||
      ldc < psyche_cublas_max_int(1, n) ||
      alpha == NULL ||
      beta == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_matrix_span(a_rows, a_cols, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(n, n, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n > 0 && C == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_syr2k_args(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const void *alpha,
    const void *A,
    int lda,
    const void *B,
    int ldb,
    const void *beta,
    void *C,
    int ldc) {
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  int rows = 0;
  int cols = 0;
  (void)A;
  (void)B;
  status = psyche_cublas_validate_syrk_args(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  rows = trans == CUBLAS_OP_N ? n : k;
  cols = trans == CUBLAS_OP_N ? k : n;
  if (ldb < psyche_cublas_max_int(1, rows)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return psyche_cublas_validate_matrix_span(rows, cols, ldb);
}

static cublasStatus_t psyche_cublas_validate_herk_args(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const void *alpha,
    const void *A,
    int lda,
    const void *beta,
    void *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  int a_rows = 0;
  int a_cols = 0;
  (void)A;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!psyche_cublas_valid_uplo(uplo) || !psyche_cublas_valid_op(trans) || n < 0 || k < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  a_rows = trans == CUBLAS_OP_N ? n : k;
  a_cols = trans == CUBLAS_OP_N ? k : n;
  if (
      lda < psyche_cublas_max_int(1, a_rows) ||
      ldc < psyche_cublas_max_int(1, n) ||
      alpha == NULL ||
      beta == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_matrix_span(a_rows, a_cols, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(n, n, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n > 0 && C == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_her2k_args(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const void *alpha,
    const void *A,
    int lda,
    const void *B,
    int ldb,
    const void *beta,
    void *C,
    int ldc) {
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  int rows = 0;
  int cols = 0;
  (void)B;
  status = psyche_cublas_validate_herk_args(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  rows = trans == CUBLAS_OP_N ? n : k;
  cols = trans == CUBLAS_OP_N ? k : n;
  if (ldb < psyche_cublas_max_int(1, rows)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return psyche_cublas_validate_matrix_span(rows, cols, ldb);
}

static cublasStatus_t psyche_cublas_validate_gemv_args(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const void *alpha,
    const void *A,
    int lda,
    const void *x,
    int incx,
    const void *beta,
    void *y,
    int incy,
    int *input_len,
    int *output_len) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  int local_input_len = 0;
  int local_output_len = 0;
  (void)A;
  (void)x;
  if (input_len == 0 || output_len == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *input_len = 0;
  *output_len = 0;
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (ctx->pointer_mode != CUBLAS_POINTER_MODE_HOST) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cublas_valid_op(trans)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (m < 0 || n < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (lda < psyche_cublas_max_int(1, m)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha == NULL || beta == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  local_input_len = trans == CUBLAS_OP_N ? n : m;
  local_output_len = trans == CUBLAS_OP_N ? m : n;
  status = psyche_cublas_validate_signed_stride(local_input_len, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_stride(local_output_len, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_strided_span(local_input_len, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_strided_span(local_output_len, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(m, n, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  *input_len = local_input_len;
  *output_len = local_output_len;
  if (local_output_len == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (y == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_ger_args(
    cublasHandle_t handle,
    int m,
    int n,
    const void *alpha,
    int incx,
    int incy,
    int lda) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (ctx == NULL) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (ctx->pointer_mode != CUBLAS_POINTER_MODE_HOST) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (m < 0 || n < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (lda < psyche_cublas_max_int(1, m)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_signed_stride(m, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_strided_span(m, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_strided_span(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  return psyche_cublas_validate_matrix_span(m, n, lda);
}

static cublasStatus_t psyche_cublas_validate_symv_args(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const void *alpha,
    const void *A,
    int lda,
    const void *x,
    int incx,
    const void *beta,
    void *y,
    int incy) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  (void)A;
  (void)x;
  if (ctx == NULL) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (ctx->pointer_mode != CUBLAS_POINTER_MODE_HOST) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cublas_valid_uplo(uplo) || n < 0 || lda < psyche_cublas_max_int(1, n)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha == NULL || beta == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_signed_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_strided_span(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_strided_span(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(n, n, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n > 0 && y == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_syr_args(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const void *alpha,
    int incx,
    void *A,
    int lda) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (ctx == NULL) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (ctx->pointer_mode != CUBLAS_POINTER_MODE_HOST) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cublas_valid_uplo(uplo) || n < 0 || lda < psyche_cublas_max_int(1, n)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_signed_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_strided_span(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(n, n, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n > 0 && A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_syr2_args(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const void *alpha,
    int incx,
    int incy,
    void *A,
    int lda) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (ctx == NULL) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (ctx->pointer_mode != CUBLAS_POINTER_MODE_HOST) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cublas_valid_uplo(uplo) || n < 0 || lda < psyche_cublas_max_int(1, n)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_signed_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_strided_span(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_strided_span(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(n, n, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n > 0 && A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_trmv_trsv_args(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const void *A,
    int lda,
    void *x,
    int incx) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 0);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (
      !psyche_cublas_valid_uplo(uplo) ||
      !psyche_cublas_valid_op(trans) ||
      !psyche_cublas_valid_diag(diag) ||
      n < 0 ||
      lda < psyche_cublas_max_int(1, n) ||
      incx == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_signed_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_signed_strided_span(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(n, n, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n > 0 && (A == NULL || x == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_trmm_args(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const void *alpha,
    const void *A,
    int lda,
    const void *B,
    int ldb,
    void *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  int a_dim = side == CUBLAS_SIDE_LEFT ? m : n;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (
      !psyche_cublas_valid_side(side) ||
      !psyche_cublas_valid_uplo(uplo) ||
      !psyche_cublas_valid_op(trans) ||
      !psyche_cublas_valid_diag(diag) ||
      m < 0 ||
      n < 0 ||
      lda < psyche_cublas_max_int(1, a_dim) ||
      ldb < psyche_cublas_max_int(1, m) ||
      ldc < psyche_cublas_max_int(1, m) ||
      alpha == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_matrix_span(a_dim, a_dim, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(m, n, ldb);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(m, n, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (m > 0 && n > 0 && C == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  (void)A;
  (void)B;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_trsm_args(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const void *alpha,
    const void *A,
    int lda,
    void *B,
    int ldb) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  int a_dim = side == CUBLAS_SIDE_LEFT ? m : n;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (
      !psyche_cublas_valid_side(side) ||
      !psyche_cublas_valid_uplo(uplo) ||
      !psyche_cublas_valid_op(trans) ||
      !psyche_cublas_valid_diag(diag) ||
      m < 0 ||
      n < 0 ||
      lda < psyche_cublas_max_int(1, a_dim) ||
      ldb < psyche_cublas_max_int(1, m) ||
      alpha == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_validate_matrix_span(a_dim, a_dim, lda);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_matrix_span(m, n, ldb);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (m > 0 && n > 0 && B == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  (void)A;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_validate_reduction_args(
    cublasHandle_t handle,
    int n,
    int incx,
    void *result,
    int *should_compute) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  if (should_compute == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *should_compute = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (result == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (n <= 0 || incx <= 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  status = psyche_cublas_validate_strided_span(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  *should_compute = 1;
  return CUBLAS_STATUS_SUCCESS;
}

static float psyche_cublas_sgemm_a(
    const float *A,
    cublasOperation_t transa,
    int row,
    int inner,
    int lda) {
  return transa == CUBLAS_OP_N ? A[row + inner * lda] : A[inner + row * lda];
}

static float psyche_cublas_sgemm_b(
    const float *B,
    cublasOperation_t transb,
    int inner,
    int col,
    int ldb) {
  return transb == CUBLAS_OP_N ? B[inner + col * ldb] : B[col + inner * ldb];
}

static double psyche_cublas_dgemm_a(
    const double *A,
    cublasOperation_t transa,
    int row,
    int inner,
    int lda) {
  return transa == CUBLAS_OP_N ? A[row + inner * lda] : A[inner + row * lda];
}

static double psyche_cublas_dgemm_b(
    const double *B,
    cublasOperation_t transb,
    int inner,
    int col,
    int ldb) {
  return transb == CUBLAS_OP_N ? B[inner + col * ldb] : B[col + inner * ldb];
}

static size_t psyche_cublas_symmetric_index(
    cublasFillMode_t uplo,
    int row,
    int col,
    int lda) {
  if (uplo == CUBLAS_FILL_MODE_LOWER) {
    return row >= col
        ? (size_t)row + (size_t)col * (size_t)lda
        : (size_t)col + (size_t)row * (size_t)lda;
  }
  return row <= col
      ? (size_t)row + (size_t)col * (size_t)lda
      : (size_t)col + (size_t)row * (size_t)lda;
}

static int psyche_cublas_symmetric_element_is_stored(cublasFillMode_t uplo, int row, int col) {
  return uplo == CUBLAS_FILL_MODE_LOWER ? row >= col : row <= col;
}

static float psyche_cublas_ssymmetric_value(
    const float *A,
    cublasFillMode_t uplo,
    int row,
    int col,
    int lda) {
  return A[psyche_cublas_symmetric_index(uplo, row, col, lda)];
}

static double psyche_cublas_dsymmetric_value(
    const double *A,
    cublasFillMode_t uplo,
    int row,
    int col,
    int lda) {
  return A[psyche_cublas_symmetric_index(uplo, row, col, lda)];
}

static float psyche_cublas_sop_value(
    const float *A,
    cublasOperation_t trans,
    int row,
    int col,
    int lda) {
  return trans == CUBLAS_OP_N ? A[(size_t)row + (size_t)col * (size_t)lda] : A[(size_t)col + (size_t)row * (size_t)lda];
}

static double psyche_cublas_dop_value(
    const double *A,
    cublasOperation_t trans,
    int row,
    int col,
    int lda) {
  return trans == CUBLAS_OP_N ? A[(size_t)row + (size_t)col * (size_t)lda] : A[(size_t)col + (size_t)row * (size_t)lda];
}

static int psyche_cublas_triangular_element_is_stored(
    cublasFillMode_t uplo,
    int row,
    int col) {
  return uplo == CUBLAS_FILL_MODE_LOWER ? row >= col : row <= col;
}

static float psyche_cublas_striangular_value(
    const float *A,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int row,
    int col,
    int lda) {
  int source_row = trans == CUBLAS_OP_N ? row : col;
  int source_col = trans == CUBLAS_OP_N ? col : row;
  if (!psyche_cublas_triangular_element_is_stored(uplo, source_row, source_col)) {
    return 0.0f;
  }
  if (source_row == source_col && diag == CUBLAS_DIAG_UNIT) {
    return 1.0f;
  }
  return A[(size_t)source_row + (size_t)source_col * (size_t)lda];
}

static double psyche_cublas_dtriangular_value(
    const double *A,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int row,
    int col,
    int lda) {
  int source_row = trans == CUBLAS_OP_N ? row : col;
  int source_col = trans == CUBLAS_OP_N ? col : row;
  if (!psyche_cublas_triangular_element_is_stored(uplo, source_row, source_col)) {
    return 0.0;
  }
  if (source_row == source_col && diag == CUBLAS_DIAG_UNIT) {
    return 1.0;
  }
  return A[(size_t)source_row + (size_t)source_col * (size_t)lda];
}

static cublasStatus_t psyche_cublas_sgemm_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    const float *beta,
    float *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_gemm_args(
      handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
  float *tmp = 0;
  float alpha_value = 0.0f;
  float beta_value = 0.0f;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0f && k > 0 && (A == 0 || B == 0)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
#if defined(__APPLE__)
  if (alpha_value != 0.0f && k > 0) {
    if (beta_value != 0.0f) {
      for (col = 0; col < n; col++) {
        memcpy(&tmp[(size_t)col * (size_t)m], &C[(size_t)col * (size_t)ldc], (size_t)m * sizeof(*tmp));
      }
    } else {
      memset(tmp, 0, tmp_bytes);
    }
    cblas_sgemm(
        CblasColMajor,
        psyche_cublas_accelerate_trans(transa),
        psyche_cublas_accelerate_trans(transb),
        m,
        n,
        k,
        alpha_value,
        A,
        lda,
        B,
        ldb,
        beta_value,
        tmp,
        m);
    for (col = 0; col < n; col++) {
      memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
    }
    free(tmp);
    return CUBLAS_STATUS_SUCCESS;
  }
#endif
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      float acc = 0.0f;
      size_t tmp_index = (size_t)row + (size_t)col * (size_t)m;
      size_t c_index = (size_t)row + (size_t)col * (size_t)ldc;
      if (alpha_value != 0.0f) {
        for (inner = 0; inner < k; inner++) {
          acc += psyche_cublas_sgemm_a(A, transa, row, inner, lda) *
                 psyche_cublas_sgemm_b(B, transb, inner, col, ldb);
        }
      }
      tmp[tmp_index] =
          (alpha_value * acc) +
          (beta_value == 0.0f ? 0.0f : beta_value * C[c_index]);
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dgemm_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    const double *beta,
    double *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_gemm_args(
      handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
  double *tmp = 0;
  double alpha_value = 0.0;
  double beta_value = 0.0;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0 && k > 0 && (A == 0 || B == 0)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
#if defined(__APPLE__)
  if (alpha_value != 0.0 && k > 0) {
    if (beta_value != 0.0) {
      for (col = 0; col < n; col++) {
        memcpy(&tmp[(size_t)col * (size_t)m], &C[(size_t)col * (size_t)ldc], (size_t)m * sizeof(*tmp));
      }
    } else {
      memset(tmp, 0, tmp_bytes);
    }
    cblas_dgemm(
        CblasColMajor,
        psyche_cublas_accelerate_trans(transa),
        psyche_cublas_accelerate_trans(transb),
        m,
        n,
        k,
        alpha_value,
        A,
        lda,
        B,
        ldb,
        beta_value,
        tmp,
        m);
    for (col = 0; col < n; col++) {
      memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
    }
    free(tmp);
    return CUBLAS_STATUS_SUCCESS;
  }
#endif
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      double acc = 0.0;
      size_t tmp_index = (size_t)row + (size_t)col * (size_t)m;
      size_t c_index = (size_t)row + (size_t)col * (size_t)ldc;
      if (alpha_value != 0.0) {
        for (inner = 0; inner < k; inner++) {
          acc += psyche_cublas_dgemm_a(A, transa, row, inner, lda) *
                 psyche_cublas_dgemm_b(B, transb, inner, col, ldb);
        }
      }
      tmp[tmp_index] =
          (alpha_value * acc) +
          (beta_value == 0.0 ? 0.0 : beta_value * C[c_index]);
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ssymm_impl(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    const float *beta,
    float *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_symm_args(handle, side, uplo, m, n, alpha, A, lda, B, ldb, beta, C, ldc);
  float alpha_value = 0.0f;
  float beta_value = 0.0f;
  float *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0f && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      float acc = 0.0f;
      if (alpha_value != 0.0f) {
        if (side == CUBLAS_SIDE_LEFT) {
          for (inner = 0; inner < m; inner++) {
            acc += psyche_cublas_ssymmetric_value(A, uplo, row, inner, lda) *
                   B[(size_t)inner + (size_t)col * (size_t)ldb];
          }
        } else {
          for (inner = 0; inner < n; inner++) {
            acc += B[(size_t)row + (size_t)inner * (size_t)ldb] *
                   psyche_cublas_ssymmetric_value(A, uplo, inner, col, lda);
          }
        }
      }
      tmp[(size_t)row + (size_t)col * (size_t)m] =
          (alpha_value * acc) +
          (beta_value == 0.0f ? 0.0f : beta_value * C[(size_t)row + (size_t)col * (size_t)ldc]);
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dsymm_impl(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    const double *beta,
    double *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_symm_args(handle, side, uplo, m, n, alpha, A, lda, B, ldb, beta, C, ldc);
  double alpha_value = 0.0;
  double beta_value = 0.0;
  double *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0 && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      double acc = 0.0;
      if (alpha_value != 0.0) {
        if (side == CUBLAS_SIDE_LEFT) {
          for (inner = 0; inner < m; inner++) {
            acc += psyche_cublas_dsymmetric_value(A, uplo, row, inner, lda) *
                   B[(size_t)inner + (size_t)col * (size_t)ldb];
          }
        } else {
          for (inner = 0; inner < n; inner++) {
            acc += B[(size_t)row + (size_t)inner * (size_t)ldb] *
                   psyche_cublas_dsymmetric_value(A, uplo, inner, col, lda);
          }
        }
      }
      tmp[(size_t)row + (size_t)col * (size_t)m] =
          (alpha_value * acc) +
          (beta_value == 0.0 ? 0.0 : beta_value * C[(size_t)row + (size_t)col * (size_t)ldc]);
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ssyrk_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    const float *beta,
    float *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_syrk_args(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
  float alpha_value = 0.0f;
  float beta_value = 0.0f;
  float *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0f && k > 0 && A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t index = (size_t)row + (size_t)col * (size_t)n;
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        tmp[index] = 0.0f;
        continue;
      }
      tmp[index] = beta_value == 0.0f ? 0.0f : beta_value * C[(size_t)row + (size_t)col * (size_t)ldc];
      if (alpha_value != 0.0f) {
        float acc = 0.0f;
        for (inner = 0; inner < k; inner++) {
          acc += psyche_cublas_sop_value(A, trans, row, inner, lda) *
                 psyche_cublas_sop_value(A, trans, col, inner, lda);
        }
        tmp[index] += alpha_value * acc;
      }
    }
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      if (psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        C[(size_t)row + (size_t)col * (size_t)ldc] = tmp[(size_t)row + (size_t)col * (size_t)n];
      }
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dsyrk_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    const double *beta,
    double *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_syrk_args(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
  double alpha_value = 0.0;
  double beta_value = 0.0;
  double *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0 && k > 0 && A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t index = (size_t)row + (size_t)col * (size_t)n;
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        tmp[index] = 0.0;
        continue;
      }
      tmp[index] = beta_value == 0.0 ? 0.0 : beta_value * C[(size_t)row + (size_t)col * (size_t)ldc];
      if (alpha_value != 0.0) {
        double acc = 0.0;
        for (inner = 0; inner < k; inner++) {
          acc += psyche_cublas_dop_value(A, trans, row, inner, lda) *
                 psyche_cublas_dop_value(A, trans, col, inner, lda);
        }
        tmp[index] += alpha_value * acc;
      }
    }
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      if (psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        C[(size_t)row + (size_t)col * (size_t)ldc] = tmp[(size_t)row + (size_t)col * (size_t)n];
      }
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ssyr2k_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    const float *beta,
    float *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_syr2k_args(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
  float alpha_value = 0.0f;
  float beta_value = 0.0f;
  float *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0f && k > 0 && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t index = (size_t)row + (size_t)col * (size_t)n;
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        tmp[index] = 0.0f;
        continue;
      }
      tmp[index] = beta_value == 0.0f ? 0.0f : beta_value * C[(size_t)row + (size_t)col * (size_t)ldc];
      if (alpha_value != 0.0f) {
        float acc = 0.0f;
        for (inner = 0; inner < k; inner++) {
          acc += psyche_cublas_sop_value(A, trans, row, inner, lda) *
                 psyche_cublas_sop_value(B, trans, col, inner, ldb);
          acc += psyche_cublas_sop_value(B, trans, row, inner, ldb) *
                 psyche_cublas_sop_value(A, trans, col, inner, lda);
        }
        tmp[index] += alpha_value * acc;
      }
    }
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      if (psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        C[(size_t)row + (size_t)col * (size_t)ldc] = tmp[(size_t)row + (size_t)col * (size_t)n];
      }
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dsyr2k_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    const double *beta,
    double *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_syr2k_args(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
  double alpha_value = 0.0;
  double beta_value = 0.0;
  double *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0 && k > 0 && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t index = (size_t)row + (size_t)col * (size_t)n;
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        tmp[index] = 0.0;
        continue;
      }
      tmp[index] = beta_value == 0.0 ? 0.0 : beta_value * C[(size_t)row + (size_t)col * (size_t)ldc];
      if (alpha_value != 0.0) {
        double acc = 0.0;
        for (inner = 0; inner < k; inner++) {
          acc += psyche_cublas_dop_value(A, trans, row, inner, lda) *
                 psyche_cublas_dop_value(B, trans, col, inner, ldb);
          acc += psyche_cublas_dop_value(B, trans, row, inner, ldb) *
                 psyche_cublas_dop_value(A, trans, col, inner, lda);
        }
        tmp[index] += alpha_value * acc;
      }
    }
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      if (psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        C[(size_t)row + (size_t)col * (size_t)ldc] = tmp[(size_t)row + (size_t)col * (size_t)n];
      }
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_sgemm_strided_batched_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    long long strideA,
    const float *B,
    int ldb,
    long long strideB,
    const float *beta,
    float *C,
    int ldc,
    long long strideC,
    int batchCount) {
  cublasStatus_t status = psyche_cublas_validate_gemm_strided_batched_args(
      handle, transa, transb, m, n, k, alpha, A, lda, strideA, B, ldb, strideB, beta, C, ldc, strideC, batchCount);
  int batch = 0;
  if (status != CUBLAS_STATUS_SUCCESS || batchCount == 0 || m == 0 || n == 0) {
    return status;
  }
  for (batch = 0; batch < batchCount; batch++) {
    ptrdiff_t a_offset = (ptrdiff_t)batch * (ptrdiff_t)strideA;
    ptrdiff_t b_offset = (ptrdiff_t)batch * (ptrdiff_t)strideB;
    ptrdiff_t c_offset = (ptrdiff_t)batch * (ptrdiff_t)strideC;
    const float *batch_A = A == NULL ? NULL : A + a_offset;
    const float *batch_B = B == NULL ? NULL : B + b_offset;
    float *batch_C = C + c_offset;
    status = psyche_cublas_sgemm_impl(
        handle, transa, transb, m, n, k, alpha, batch_A, lda, batch_B, ldb, beta, batch_C, ldc);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_sgemm_batched_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const float *alpha,
    const float *const Aarray[],
    int lda,
    const float *const Barray[],
    int ldb,
    const float *beta,
    float *const Carray[],
    int ldc,
    int batchCount) {
  cublasStatus_t status = psyche_cublas_validate_gemm_batched_args(
      handle, transa, transb, m, n, k, alpha, lda, ldb, beta, Carray, ldc, batchCount);
  float alpha_value = 0.0f;
  int product_required = 0;
  int batch = 0;
  if (status != CUBLAS_STATUS_SUCCESS || batchCount == 0 || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  product_required = alpha_value != 0.0f && k > 0;
  if (Carray == NULL || (product_required && (Aarray == NULL || Barray == NULL))) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (batch = 0; batch < batchCount; batch++) {
    if (Carray[batch] == NULL) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (product_required && (Aarray[batch] == NULL || Barray[batch] == NULL)) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
  }
  for (batch = 0; batch < batchCount; batch++) {
    const float *batch_A = product_required ? Aarray[batch] : NULL;
    const float *batch_B = product_required ? Barray[batch] : NULL;
    status = psyche_cublas_sgemm_impl(
        handle, transa, transb, m, n, k, alpha, batch_A, lda, batch_B, ldb, beta, Carray[batch], ldc);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dgemm_strided_batched_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    long long strideA,
    const double *B,
    int ldb,
    long long strideB,
    const double *beta,
    double *C,
    int ldc,
    long long strideC,
    int batchCount) {
  cublasStatus_t status = psyche_cublas_validate_gemm_strided_batched_args(
      handle, transa, transb, m, n, k, alpha, A, lda, strideA, B, ldb, strideB, beta, C, ldc, strideC, batchCount);
  int batch = 0;
  if (status != CUBLAS_STATUS_SUCCESS || batchCount == 0 || m == 0 || n == 0) {
    return status;
  }
  for (batch = 0; batch < batchCount; batch++) {
    ptrdiff_t a_offset = (ptrdiff_t)batch * (ptrdiff_t)strideA;
    ptrdiff_t b_offset = (ptrdiff_t)batch * (ptrdiff_t)strideB;
    ptrdiff_t c_offset = (ptrdiff_t)batch * (ptrdiff_t)strideC;
    const double *batch_A = A == NULL ? NULL : A + a_offset;
    const double *batch_B = B == NULL ? NULL : B + b_offset;
    double *batch_C = C + c_offset;
    status = psyche_cublas_dgemm_impl(
        handle, transa, transb, m, n, k, alpha, batch_A, lda, batch_B, ldb, beta, batch_C, ldc);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dgemm_batched_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const double *alpha,
    const double *const Aarray[],
    int lda,
    const double *const Barray[],
    int ldb,
    const double *beta,
    double *const Carray[],
    int ldc,
    int batchCount) {
  cublasStatus_t status = psyche_cublas_validate_gemm_batched_args(
      handle, transa, transb, m, n, k, alpha, lda, ldb, beta, Carray, ldc, batchCount);
  double alpha_value = 0.0;
  int product_required = 0;
  int batch = 0;
  if (status != CUBLAS_STATUS_SUCCESS || batchCount == 0 || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  product_required = alpha_value != 0.0 && k > 0;
  if (Carray == NULL || (product_required && (Aarray == NULL || Barray == NULL))) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (batch = 0; batch < batchCount; batch++) {
    if (Carray[batch] == NULL) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (product_required && (Aarray[batch] == NULL || Barray[batch] == NULL)) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
  }
  for (batch = 0; batch < batchCount; batch++) {
    const double *batch_A = product_required ? Aarray[batch] : NULL;
    const double *batch_B = product_required ? Barray[batch] : NULL;
    status = psyche_cublas_dgemm_impl(
        handle, transa, transb, m, n, k, alpha, batch_A, lda, batch_B, ldb, beta, Carray[batch], ldc);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_sgemv_impl(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *x,
    int incx,
    const float *beta,
    float *y,
    int incy) {
  int input_len = 0;
  int output_len = 0;
  cublasStatus_t status = psyche_cublas_validate_gemv_args(
      handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy, &input_len, &output_len);
  float *tmp = 0;
  float *x_tmp = 0;
  float *y_tmp = 0;
  float alpha_value = 0.0f;
  float beta_value = 0.0f;
  size_t tmp_bytes = 0;
  size_t x_tmp_bytes = 0;
  size_t y_tmp_bytes = 0;
  int out = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || output_len == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0f && input_len > 0 && (A == 0 || x == 0)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
#if defined(__APPLE__)
  if (psyche_cublas_metal_enabled()) {
    const unsigned int block_dim_x = 256;
    unsigned int grid_dim_x = 0;
    size_t output_bytes = 0;
    int product_required = alpha_value != 0.0f && input_len > 0;
    const float *metal_x = x;
    const float *metal_y = y;
    float *metal_out_y = y;
    CUresult metal_result = CUDA_ERROR_INVALID_VALUE;
    status = psyche_cublas_contiguous_f32_launch_shape(output_len, block_dim_x, &output_bytes, &grid_dim_x);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    (void)output_bytes;
    if (product_required && incx != 1) {
      status = psyche_cublas_temp_bytes(input_len, 1, sizeof(*x_tmp), &x_tmp_bytes);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      x_tmp = (float *)malloc(x_tmp_bytes);
      if (x_tmp == 0) {
        return CUBLAS_STATUS_ALLOC_FAILED;
      }
      for (inner = 0; inner < input_len; inner++) {
        x_tmp[inner] = x[psyche_cublas_signed_stride_index(input_len, incx, inner)];
      }
      metal_x = x_tmp;
    }
    if (incy != 1) {
      status = psyche_cublas_temp_bytes(output_len, 1, sizeof(*y_tmp), &y_tmp_bytes);
      if (status != CUBLAS_STATUS_SUCCESS) {
        free(x_tmp);
        return status;
      }
      y_tmp = (float *)malloc(y_tmp_bytes);
      if (y_tmp == 0) {
        free(x_tmp);
        return CUBLAS_STATUS_ALLOC_FAILED;
      }
      if (beta_value != 0.0f) {
        for (out = 0; out < output_len; out++) {
          y_tmp[out] = y[psyche_cublas_signed_stride_index(output_len, incy, out)];
        }
      } else {
        memset(y_tmp, 0, y_tmp_bytes);
      }
      metal_y = y_tmp;
      metal_out_y = y_tmp;
    }
    metal_result = psyche_cuda_metal_launch_sgemv_f32(
        A,
        metal_x,
        metal_y,
        metal_out_y,
        alpha_value,
        beta_value,
        (unsigned int)n,
        (unsigned int)lda,
        (unsigned int)trans,
        (unsigned int)input_len,
        (unsigned int)output_len,
        grid_dim_x,
        block_dim_x);
    if (metal_result == CUDA_SUCCESS) {
      if (incy != 1) {
        for (out = 0; out < output_len; out++) {
          y[psyche_cublas_signed_stride_index(output_len, incy, out)] = y_tmp[out];
        }
      }
      free(x_tmp);
      free(y_tmp);
      return CUBLAS_STATUS_SUCCESS;
    }
    if (
        psyche_cublas_metal_required() ||
        !psyche_cublas_metal_preferred_can_fallback(metal_result)) {
      free(x_tmp);
      free(y_tmp);
      return psyche_cublas_status_from_cuda_result(metal_result);
    }
    free(x_tmp);
    free(y_tmp);
    x_tmp = 0;
    y_tmp = 0;
  }
#else
  if (psyche_cublas_metal_required()) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
#endif
  status = psyche_cublas_temp_bytes(output_len, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (out = 0; out < output_len; out++) {
    float acc = 0.0f;
    if (alpha_value != 0.0f) {
      for (inner = 0; inner < input_len; inner++) {
        size_t a_index = trans == CUBLAS_OP_N
            ? (size_t)out + (size_t)inner * (size_t)lda
            : (size_t)inner + (size_t)out * (size_t)lda;
        size_t x_index = psyche_cublas_signed_stride_index(input_len, incx, inner);
        acc += A[a_index] * x[x_index];
      }
    }
    tmp[out] =
        (alpha_value * acc) +
        (beta_value == 0.0f ? 0.0f : beta_value * y[psyche_cublas_signed_stride_index(output_len, incy, out)]);
  }
  for (out = 0; out < output_len; out++) {
    y[psyche_cublas_signed_stride_index(output_len, incy, out)] = tmp[out];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dgemv_impl(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *x,
    int incx,
    const double *beta,
    double *y,
    int incy) {
  int input_len = 0;
  int output_len = 0;
  cublasStatus_t status = psyche_cublas_validate_gemv_args(
      handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy, &input_len, &output_len);
  double *tmp = 0;
  double alpha_value = 0.0;
  double beta_value = 0.0;
  size_t tmp_bytes = 0;
  int out = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || output_len == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0 && input_len > 0 && (A == 0 || x == 0)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(output_len, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (out = 0; out < output_len; out++) {
    double acc = 0.0;
    if (alpha_value != 0.0) {
      for (inner = 0; inner < input_len; inner++) {
        size_t a_index = trans == CUBLAS_OP_N
            ? (size_t)out + (size_t)inner * (size_t)lda
            : (size_t)inner + (size_t)out * (size_t)lda;
        size_t x_index = psyche_cublas_signed_stride_index(input_len, incx, inner);
        acc += A[a_index] * x[x_index];
      }
    }
    tmp[out] =
        (alpha_value * acc) +
        (beta_value == 0.0 ? 0.0 : beta_value * y[psyche_cublas_signed_stride_index(output_len, incy, out)]);
  }
  for (out = 0; out < output_len; out++) {
    y[psyche_cublas_signed_stride_index(output_len, incy, out)] = tmp[out];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_sasum_impl(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    float *result) {
  int should_compute = 0;
  cublasStatus_t status = psyche_cublas_validate_reduction_args(handle, n, incx, result, &should_compute);
  double acc = 0.0;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!should_compute) {
    *result = 0.0f;
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
#if defined(__APPLE__)
  if (psyche_cublas_metal_enabled()) {
    if (incx == 1) {
      const unsigned int block_dim_x = 256;
      unsigned int grid_dim_x = 0;
      size_t bytes = 0;
      CUresult metal_result = CUDA_ERROR_INVALID_VALUE;
      status = psyche_cublas_contiguous_f32_launch_shape(n, block_dim_x, &bytes, &grid_dim_x);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      metal_result = psyche_cuda_metal_launch_asum_f32(
          x,
          result,
          (unsigned int)n,
          bytes,
          grid_dim_x,
          block_dim_x);
      if (
          metal_result == CUDA_SUCCESS ||
          psyche_cublas_metal_required() ||
          !psyche_cublas_metal_preferred_can_fallback(metal_result)) {
        return psyche_cublas_status_from_cuda_result(metal_result);
      }
    } else if (psyche_cublas_metal_required()) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
  }
#else
  if (psyche_cublas_metal_required()) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
#endif
  for (i = 0; i < n; i++) {
    acc += (double)fabsf(x[(size_t)i * (size_t)incx]);
  }
  *result = (float)acc;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dasum_impl(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    double *result) {
  int should_compute = 0;
  cublasStatus_t status = psyche_cublas_validate_reduction_args(handle, n, incx, result, &should_compute);
  double acc = 0.0;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!should_compute) {
    *result = 0.0;
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    acc += fabs(x[(size_t)i * (size_t)incx]);
  }
  *result = acc;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_snrm2_impl(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    float *result) {
  int should_compute = 0;
  cublasStatus_t status = psyche_cublas_validate_reduction_args(handle, n, incx, result, &should_compute);
  double scale = 0.0;
  double ssq = 1.0;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!should_compute) {
    *result = 0.0f;
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
#if defined(__APPLE__)
  if (psyche_cublas_metal_enabled()) {
    if (incx == 1) {
      const unsigned int block_dim_x = 256;
      unsigned int grid_dim_x = 0;
      size_t bytes = 0;
      CUresult metal_result = CUDA_ERROR_INVALID_VALUE;
      status = psyche_cublas_contiguous_f32_launch_shape(n, block_dim_x, &bytes, &grid_dim_x);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      metal_result = psyche_cuda_metal_launch_nrm2_f32(
          x,
          result,
          (unsigned int)n,
          bytes,
          grid_dim_x,
          block_dim_x);
      if (
          metal_result == CUDA_SUCCESS ||
          psyche_cublas_metal_required() ||
          !psyche_cublas_metal_preferred_can_fallback(metal_result)) {
        return psyche_cublas_status_from_cuda_result(metal_result);
      }
    } else if (psyche_cublas_metal_required()) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
  }
#else
  if (psyche_cublas_metal_required()) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
#endif
  for (i = 0; i < n; i++) {
    double ax = fabs((double)x[(size_t)i * (size_t)incx]);
    if (ax != 0.0) {
      if (scale < ax) {
        double ratio = scale / ax;
        ssq = 1.0 + ssq * ratio * ratio;
        scale = ax;
      } else {
        double ratio = ax / scale;
        ssq += ratio * ratio;
      }
    }
  }
  *result = (float)(scale == 0.0 ? 0.0 : scale * sqrt(ssq));
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dnrm2_impl(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    double *result) {
  int should_compute = 0;
  cublasStatus_t status = psyche_cublas_validate_reduction_args(handle, n, incx, result, &should_compute);
  double scale = 0.0;
  double ssq = 1.0;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!should_compute) {
    *result = 0.0;
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    double ax = fabs(x[(size_t)i * (size_t)incx]);
    if (ax != 0.0) {
      if (scale < ax) {
        double ratio = scale / ax;
        ssq = 1.0 + ssq * ratio * ratio;
        scale = ax;
      } else {
        double ratio = ax / scale;
        ssq += ratio * ratio;
      }
    }
  }
  *result = scale == 0.0 ? 0.0 : scale * sqrt(ssq);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_isamax_impl(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    int *result) {
  int should_compute = 0;
  cublasStatus_t status = psyche_cublas_validate_reduction_args(handle, n, incx, result, &should_compute);
  float best = 0.0f;
  int best_index = 1;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!should_compute) {
    *result = 0;
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  best = fabsf(x[0]);
  for (i = 1; i < n; i++) {
    float value = fabsf(x[(size_t)i * (size_t)incx]);
    if (value > best) {
      best = value;
      best_index = i + 1;
    }
  }
  *result = best_index;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_idamax_impl(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    int *result) {
  int should_compute = 0;
  cublasStatus_t status = psyche_cublas_validate_reduction_args(handle, n, incx, result, &should_compute);
  double best = 0.0;
  int best_index = 1;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!should_compute) {
    *result = 0;
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  best = fabs(x[0]);
  for (i = 1; i < n; i++) {
    double value = fabs(x[(size_t)i * (size_t)incx]);
    if (value > best) {
      best = value;
      best_index = i + 1;
    }
  }
  *result = best_index;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_isamin_impl(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    int *result) {
  int should_compute = 0;
  cublasStatus_t status = psyche_cublas_validate_reduction_args(handle, n, incx, result, &should_compute);
  float best = 0.0f;
  int best_index = 1;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!should_compute) {
    *result = 0;
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  best = fabsf(x[0]);
  for (i = 1; i < n; i++) {
    float value = fabsf(x[(size_t)i * (size_t)incx]);
    if (value < best) {
      best = value;
      best_index = i + 1;
    }
  }
  *result = best_index;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_idamin_impl(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    int *result) {
  int should_compute = 0;
  cublasStatus_t status = psyche_cublas_validate_reduction_args(handle, n, incx, result, &should_compute);
  double best = 0.0;
  int best_index = 1;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!should_compute) {
    *result = 0;
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  best = fabs(x[0]);
  for (i = 1; i < n; i++) {
    double value = fabs(x[(size_t)i * (size_t)incx]);
    if (value < best) {
      best = value;
      best_index = i + 1;
    }
  }
  *result = best_index;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_sger_impl(
    cublasHandle_t handle,
    int m,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    const float *y,
    int incy,
    float *A,
    int lda) {
  cublasStatus_t status = psyche_cublas_validate_ger_args(handle, m, n, alpha, incx, incy, lda);
  float alpha_value = 0.0f;
  float *x_tmp = NULL;
  float *y_tmp = NULL;
  size_t x_bytes = 0;
  size_t y_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  /*
   * cuBLAS permits alpha-zero GER to avoid reading x/y, but A is still the
   * positive-work output. Zero-work cases return above before requiring A.
   */
  alpha_value = *alpha;
  if (A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha_value == 0.0f) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
#if defined(__APPLE__)
  if (psyche_cublas_metal_enabled()) {
    const unsigned int block_dim_x = 256;
    unsigned int grid_dim_x = 0;
    size_t update_elems = 0;
    size_t a_elems = 0;
    const float *metal_x = x;
    const float *metal_y = y;
    CUresult metal_result = CUDA_ERROR_INVALID_VALUE;
    if ((size_t)m > SIZE_MAX / (size_t)n || (size_t)lda > SIZE_MAX / (size_t)n) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    update_elems = (size_t)m * (size_t)n;
    a_elems = (size_t)lda * (size_t)n;
    if (update_elems > UINT_MAX || a_elems > UINT_MAX) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    grid_dim_x = (unsigned int)((update_elems + (size_t)block_dim_x - 1U) / (size_t)block_dim_x);
    if (grid_dim_x == 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (incx != 1) {
      status = psyche_cublas_temp_bytes(m, 1, sizeof(*x_tmp), &x_bytes);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      x_tmp = (float *)malloc(x_bytes);
      if (x_tmp == NULL) {
        return CUBLAS_STATUS_ALLOC_FAILED;
      }
      for (row = 0; row < m; row++) {
        x_tmp[row] = x[psyche_cublas_signed_stride_index(m, incx, row)];
      }
      metal_x = x_tmp;
    }
    if (incy != 1) {
      status = psyche_cublas_temp_bytes(n, 1, sizeof(*y_tmp), &y_bytes);
      if (status != CUBLAS_STATUS_SUCCESS) {
        free(x_tmp);
        x_tmp = NULL;
        return status;
      }
      y_tmp = (float *)malloc(y_bytes);
      if (y_tmp == NULL) {
        free(x_tmp);
        x_tmp = NULL;
        return CUBLAS_STATUS_ALLOC_FAILED;
      }
      for (col = 0; col < n; col++) {
        y_tmp[col] = y[psyche_cublas_signed_stride_index(n, incy, col)];
      }
      metal_y = y_tmp;
    }
    metal_result = psyche_cuda_metal_launch_sger_f32(
        metal_x,
        metal_y,
        A,
        alpha_value,
        (unsigned int)m,
        (unsigned int)n,
        (unsigned int)lda,
        grid_dim_x,
        block_dim_x);
    if (
        metal_result == CUDA_SUCCESS ||
        psyche_cublas_metal_required() ||
        !psyche_cublas_metal_preferred_can_fallback(metal_result)) {
      free(x_tmp);
      free(y_tmp);
      return psyche_cublas_status_from_cuda_result(metal_result);
    }
    free(x_tmp);
    free(y_tmp);
    x_tmp = NULL;
    y_tmp = NULL;
    x_bytes = 0;
    y_bytes = 0;
  }
#else
  if (psyche_cublas_metal_required()) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
#endif
  status = psyche_cublas_temp_bytes(m, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*y_tmp), &y_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (float *)malloc(x_bytes);
  y_tmp = (float *)malloc(y_bytes);
  if (x_tmp == NULL || y_tmp == NULL) {
    free(x_tmp);
    free(y_tmp);
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  /* Stage inputs before writing A so overlapping host buffers stay deterministic. */
  for (row = 0; row < m; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(m, incx, row)];
  }
  for (col = 0; col < n; col++) {
    y_tmp[col] = y[psyche_cublas_signed_stride_index(n, incy, col)];
  }
  for (col = 0; col < n; col++) {
    float scaled_y = alpha_value * y_tmp[col];
    for (row = 0; row < m; row++) {
      A[(size_t)row + (size_t)col * (size_t)lda] += x_tmp[row] * scaled_y;
    }
  }
  free(x_tmp);
  free(y_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dger_impl(
    cublasHandle_t handle,
    int m,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    const double *y,
    int incy,
    double *A,
    int lda) {
  cublasStatus_t status = psyche_cublas_validate_ger_args(handle, m, n, alpha, incx, incy, lda);
  double alpha_value = 0.0;
  double *x_tmp = NULL;
  double *y_tmp = NULL;
  size_t x_bytes = 0;
  size_t y_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha_value == 0.0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(m, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*y_tmp), &y_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (double *)malloc(x_bytes);
  y_tmp = (double *)malloc(y_bytes);
  if (x_tmp == NULL || y_tmp == NULL) {
    free(x_tmp);
    free(y_tmp);
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  /* Stage inputs before writing A so overlapping host buffers stay deterministic. */
  for (row = 0; row < m; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(m, incx, row)];
  }
  for (col = 0; col < n; col++) {
    y_tmp[col] = y[psyche_cublas_signed_stride_index(n, incy, col)];
  }
  for (col = 0; col < n; col++) {
    double scaled_y = alpha_value * y_tmp[col];
    for (row = 0; row < m; row++) {
      A[(size_t)row + (size_t)col * (size_t)lda] += x_tmp[row] * scaled_y;
    }
  }
  free(x_tmp);
  free(y_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ssymv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *x,
    int incx,
    const float *beta,
    float *y,
    int incy) {
  cublasStatus_t status =
      psyche_cublas_validate_symv_args(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
  float alpha_value = 0.0f;
  float beta_value = 0.0f;
  float *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0f && (A == NULL || x == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    float acc = 0.0f;
    if (alpha_value != 0.0f) {
      for (col = 0; col < n; col++) {
        acc += A[psyche_cublas_symmetric_index(uplo, row, col, lda)] *
               x[psyche_cublas_signed_stride_index(n, incx, col)];
      }
    }
    tmp[row] =
        (alpha_value * acc) +
        (beta_value == 0.0f ? 0.0f : beta_value * y[psyche_cublas_signed_stride_index(n, incy, row)]);
  }
  for (row = 0; row < n; row++) {
    y[psyche_cublas_signed_stride_index(n, incy, row)] = tmp[row];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dsymv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *x,
    int incx,
    const double *beta,
    double *y,
    int incy) {
  cublasStatus_t status =
      psyche_cublas_validate_symv_args(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
  double alpha_value = 0.0;
  double beta_value = 0.0;
  double *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (alpha_value != 0.0 && (A == NULL || x == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    double acc = 0.0;
    if (alpha_value != 0.0) {
      for (col = 0; col < n; col++) {
        acc += A[psyche_cublas_symmetric_index(uplo, row, col, lda)] *
               x[psyche_cublas_signed_stride_index(n, incx, col)];
      }
    }
    tmp[row] =
        (alpha_value * acc) +
        (beta_value == 0.0 ? 0.0 : beta_value * y[psyche_cublas_signed_stride_index(n, incy, row)]);
  }
  for (row = 0; row < n; row++) {
    y[psyche_cublas_signed_stride_index(n, incy, row)] = tmp[row];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ssyr_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    float *A,
    int lda) {
  cublasStatus_t status = psyche_cublas_validate_syr_args(handle, uplo, n, alpha, incx, A, lda);
  float alpha_value = 0.0f;
  float *x_tmp = NULL;
  size_t x_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (alpha_value == 0.0f) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (float *)malloc(x_bytes);
  if (x_tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  /* Stage x before writing A so overlapping host buffers stay deterministic. */
  for (row = 0; row < n; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
  }
  for (col = 0; col < n; col++) {
    int row_begin = uplo == CUBLAS_FILL_MODE_UPPER ? 0 : col;
    int row_end = uplo == CUBLAS_FILL_MODE_UPPER ? col : n - 1;
    for (row = row_begin; row <= row_end; row++) {
      A[(size_t)row + (size_t)col * (size_t)lda] += alpha_value * x_tmp[row] * x_tmp[col];
    }
  }
  free(x_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dsyr_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    double *A,
    int lda) {
  cublasStatus_t status = psyche_cublas_validate_syr_args(handle, uplo, n, alpha, incx, A, lda);
  double alpha_value = 0.0;
  double *x_tmp = NULL;
  size_t x_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (alpha_value == 0.0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (double *)malloc(x_bytes);
  if (x_tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  /* Stage x before writing A so overlapping host buffers stay deterministic. */
  for (row = 0; row < n; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
  }
  for (col = 0; col < n; col++) {
    int row_begin = uplo == CUBLAS_FILL_MODE_UPPER ? 0 : col;
    int row_end = uplo == CUBLAS_FILL_MODE_UPPER ? col : n - 1;
    for (row = row_begin; row <= row_end; row++) {
      A[(size_t)row + (size_t)col * (size_t)lda] += alpha_value * x_tmp[row] * x_tmp[col];
    }
  }
  free(x_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ssyr2_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    const float *y,
    int incy,
    float *A,
    int lda) {
  cublasStatus_t status = psyche_cublas_validate_syr2_args(handle, uplo, n, alpha, incx, incy, A, lda);
  float alpha_value = 0.0f;
  float *x_tmp = NULL;
  float *y_tmp = NULL;
  size_t x_bytes = 0;
  size_t y_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (alpha_value == 0.0f) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*y_tmp), &y_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (float *)malloc(x_bytes);
  y_tmp = (float *)malloc(y_bytes);
  if (x_tmp == NULL || y_tmp == NULL) {
    free(x_tmp);
    free(y_tmp);
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  /* Stage inputs before writing A so overlapping host buffers stay deterministic. */
  for (row = 0; row < n; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
    y_tmp[row] = y[psyche_cublas_signed_stride_index(n, incy, row)];
  }
  for (col = 0; col < n; col++) {
    int row_begin = uplo == CUBLAS_FILL_MODE_UPPER ? 0 : col;
    int row_end = uplo == CUBLAS_FILL_MODE_UPPER ? col : n - 1;
    for (row = row_begin; row <= row_end; row++) {
      A[(size_t)row + (size_t)col * (size_t)lda] +=
          alpha_value * ((x_tmp[row] * y_tmp[col]) + (y_tmp[row] * x_tmp[col]));
    }
  }
  free(x_tmp);
  free(y_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dsyr2_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    const double *y,
    int incy,
    double *A,
    int lda) {
  cublasStatus_t status = psyche_cublas_validate_syr2_args(handle, uplo, n, alpha, incx, incy, A, lda);
  double alpha_value = 0.0;
  double *x_tmp = NULL;
  double *y_tmp = NULL;
  size_t x_bytes = 0;
  size_t y_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (alpha_value == 0.0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*y_tmp), &y_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (double *)malloc(x_bytes);
  y_tmp = (double *)malloc(y_bytes);
  if (x_tmp == NULL || y_tmp == NULL) {
    free(x_tmp);
    free(y_tmp);
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  /* Stage inputs before writing A so overlapping host buffers stay deterministic. */
  for (row = 0; row < n; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
    y_tmp[row] = y[psyche_cublas_signed_stride_index(n, incy, row)];
  }
  for (col = 0; col < n; col++) {
    int row_begin = uplo == CUBLAS_FILL_MODE_UPPER ? 0 : col;
    int row_end = uplo == CUBLAS_FILL_MODE_UPPER ? col : n - 1;
    for (row = row_begin; row <= row_end; row++) {
      A[(size_t)row + (size_t)col * (size_t)lda] +=
          alpha_value * ((x_tmp[row] * y_tmp[col]) + (y_tmp[row] * x_tmp[col]));
    }
  }
  free(x_tmp);
  free(y_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_strmv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const float *A,
    int lda,
    float *x,
    int incx) {
  cublasStatus_t status =
      psyche_cublas_validate_trmv_trsv_args(handle, uplo, trans, diag, n, A, lda, x, incx);
  float *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    float acc = 0.0f;
    for (col = 0; col < n; col++) {
      acc += psyche_cublas_striangular_value(A, uplo, trans, diag, row, col, lda) *
             x[psyche_cublas_signed_stride_index(n, incx, col)];
    }
    tmp[row] = acc;
  }
  for (row = 0; row < n; row++) {
    x[psyche_cublas_signed_stride_index(n, incx, row)] = tmp[row];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dtrmv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const double *A,
    int lda,
    double *x,
    int incx) {
  cublasStatus_t status =
      psyche_cublas_validate_trmv_trsv_args(handle, uplo, trans, diag, n, A, lda, x, incx);
  double *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    double acc = 0.0;
    for (col = 0; col < n; col++) {
      acc += psyche_cublas_dtriangular_value(A, uplo, trans, diag, row, col, lda) *
             x[psyche_cublas_signed_stride_index(n, incx, col)];
    }
    tmp[row] = acc;
  }
  for (row = 0; row < n; row++) {
    x[psyche_cublas_signed_stride_index(n, incx, row)] = tmp[row];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_strsv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const float *A,
    int lda,
    float *x,
    int incx) {
  cublasStatus_t status =
      psyche_cublas_validate_trmv_trsv_args(handle, uplo, trans, diag, n, A, lda, x, incx);
  float *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int op_is_lower = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
  }
  op_is_lower = trans == CUBLAS_OP_N
      ? uplo == CUBLAS_FILL_MODE_LOWER
      : uplo == CUBLAS_FILL_MODE_UPPER;
  if (op_is_lower) {
    for (row = 0; row < n; row++) {
      float value = tmp[row];
      for (col = 0; col < row; col++) {
        value -= psyche_cublas_striangular_value(A, uplo, trans, diag, row, col, lda) * tmp[col];
      }
      if (diag == CUBLAS_DIAG_NON_UNIT) {
        value /= psyche_cublas_striangular_value(A, uplo, trans, diag, row, row, lda);
      }
      tmp[row] = value;
    }
  } else {
    for (row = n - 1; row >= 0; row--) {
      float value = tmp[row];
      for (col = row + 1; col < n; col++) {
        value -= psyche_cublas_striangular_value(A, uplo, trans, diag, row, col, lda) * tmp[col];
      }
      if (diag == CUBLAS_DIAG_NON_UNIT) {
        value /= psyche_cublas_striangular_value(A, uplo, trans, diag, row, row, lda);
      }
      tmp[row] = value;
    }
  }
  for (row = 0; row < n; row++) {
    x[psyche_cublas_signed_stride_index(n, incx, row)] = tmp[row];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dtrsv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const double *A,
    int lda,
    double *x,
    int incx) {
  cublasStatus_t status =
      psyche_cublas_validate_trmv_trsv_args(handle, uplo, trans, diag, n, A, lda, x, incx);
  double *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int op_is_lower = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
  }
  op_is_lower = trans == CUBLAS_OP_N
      ? uplo == CUBLAS_FILL_MODE_LOWER
      : uplo == CUBLAS_FILL_MODE_UPPER;
  if (op_is_lower) {
    for (row = 0; row < n; row++) {
      double value = tmp[row];
      for (col = 0; col < row; col++) {
        value -= psyche_cublas_dtriangular_value(A, uplo, trans, diag, row, col, lda) * tmp[col];
      }
      if (diag == CUBLAS_DIAG_NON_UNIT) {
        value /= psyche_cublas_dtriangular_value(A, uplo, trans, diag, row, row, lda);
      }
      tmp[row] = value;
    }
  } else {
    for (row = n - 1; row >= 0; row--) {
      double value = tmp[row];
      for (col = row + 1; col < n; col++) {
        value -= psyche_cublas_dtriangular_value(A, uplo, trans, diag, row, col, lda) * tmp[col];
      }
      if (diag == CUBLAS_DIAG_NON_UNIT) {
        value /= psyche_cublas_dtriangular_value(A, uplo, trans, diag, row, row, lda);
      }
      tmp[row] = value;
    }
  }
  for (row = 0; row < n; row++) {
    x[psyche_cublas_signed_stride_index(n, incx, row)] = tmp[row];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_strmm_impl(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    float *C,
    int ldc) {
  cublasStatus_t status =
      psyche_cublas_validate_trmm_args(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
  float alpha_value = 0.0f;
  float *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (alpha_value != 0.0f && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha_value == 0.0f) {
    /* cuBLAS does not reference A/B for alpha zero, but C is still output storage. */
    psyche_cublas_szero_matrix(m, n, C, ldc);
    return CUBLAS_STATUS_SUCCESS;
  }
#if defined(__APPLE__)
  status = psyche_cublas_copy_matrix_bytes(m, n, (int)sizeof(*B), B, ldb, C, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  cblas_strmm(
      CblasColMajor,
      psyche_cublas_accelerate_side(side),
      psyche_cublas_accelerate_uplo(uplo),
      psyche_cublas_accelerate_trans(trans),
      psyche_cublas_accelerate_diag(diag),
      m,
      n,
      alpha_value,
      A,
      lda,
      C,
      ldc);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      float acc = 0.0f;
      if (side == CUBLAS_SIDE_LEFT) {
        for (inner = 0; inner < m; inner++) {
          acc += psyche_cublas_striangular_value(A, uplo, trans, diag, row, inner, lda) *
                 B[(size_t)inner + (size_t)col * (size_t)ldb];
        }
      } else {
        for (inner = 0; inner < n; inner++) {
          acc += B[(size_t)row + (size_t)inner * (size_t)ldb] *
                 psyche_cublas_striangular_value(A, uplo, trans, diag, inner, col, lda);
        }
      }
      tmp[(size_t)row + (size_t)col * (size_t)m] = alpha_value * acc;
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dtrmm_impl(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    double *C,
    int ldc) {
  cublasStatus_t status =
      psyche_cublas_validate_trmm_args(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
  double alpha_value = 0.0;
  double *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (alpha_value != 0.0 && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha_value == 0.0) {
    /* cuBLAS does not reference A/B for alpha zero, but C is still output storage. */
    psyche_cublas_dzero_matrix(m, n, C, ldc);
    return CUBLAS_STATUS_SUCCESS;
  }
#if defined(__APPLE__)
  status = psyche_cublas_copy_matrix_bytes(m, n, (int)sizeof(*B), B, ldb, C, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  cblas_dtrmm(
      CblasColMajor,
      psyche_cublas_accelerate_side(side),
      psyche_cublas_accelerate_uplo(uplo),
      psyche_cublas_accelerate_trans(trans),
      psyche_cublas_accelerate_diag(diag),
      m,
      n,
      alpha_value,
      A,
      lda,
      C,
      ldc);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      double acc = 0.0;
      if (side == CUBLAS_SIDE_LEFT) {
        for (inner = 0; inner < m; inner++) {
          acc += psyche_cublas_dtriangular_value(A, uplo, trans, diag, row, inner, lda) *
                 B[(size_t)inner + (size_t)col * (size_t)ldb];
        }
      } else {
        for (inner = 0; inner < n; inner++) {
          acc += B[(size_t)row + (size_t)inner * (size_t)ldb] *
                 psyche_cublas_dtriangular_value(A, uplo, trans, diag, inner, col, lda);
        }
      }
      tmp[(size_t)row + (size_t)col * (size_t)m] = alpha_value * acc;
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_strsm_impl(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    float *B,
    int ldb) {
  cublasStatus_t status =
      psyche_cublas_validate_trsm_args(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
  float alpha_value = 0.0f;
  float *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  int op_is_lower = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (alpha_value != 0.0f && A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha_value == 0.0f) {
    /* cuBLAS does not reference A or read B for alpha zero; B is only output storage. */
    psyche_cublas_szero_matrix(m, n, B, ldb);
    return CUBLAS_STATUS_SUCCESS;
  }
#if defined(__APPLE__)
  cblas_strsm(
      CblasColMajor,
      psyche_cublas_accelerate_side(side),
      psyche_cublas_accelerate_uplo(uplo),
      psyche_cublas_accelerate_trans(trans),
      psyche_cublas_accelerate_diag(diag),
      m,
      n,
      alpha_value,
      A,
      lda,
      B,
      ldb);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      tmp[(size_t)row + (size_t)col * (size_t)m] =
          alpha_value * B[(size_t)row + (size_t)col * (size_t)ldb];
    }
  }
  /* Match cuBLAS behavior: do not pre-test triangular singularity. */
  if (alpha_value != 0.0f) {
    op_is_lower = trans == CUBLAS_OP_N
        ? uplo == CUBLAS_FILL_MODE_LOWER
        : uplo == CUBLAS_FILL_MODE_UPPER;
    if (side == CUBLAS_SIDE_LEFT) {
      if (op_is_lower) {
        for (col = 0; col < n; col++) {
          for (row = 0; row < m; row++) {
            float value = tmp[(size_t)row + (size_t)col * (size_t)m];
            for (inner = 0; inner < row; inner++) {
              value -= psyche_cublas_striangular_value(A, uplo, trans, diag, row, inner, lda) *
                       tmp[(size_t)inner + (size_t)col * (size_t)m];
            }
            if (diag == CUBLAS_DIAG_NON_UNIT) {
              value /= psyche_cublas_striangular_value(A, uplo, trans, diag, row, row, lda);
            }
            tmp[(size_t)row + (size_t)col * (size_t)m] = value;
          }
        }
      } else {
        for (col = 0; col < n; col++) {
          for (row = m - 1; row >= 0; row--) {
            float value = tmp[(size_t)row + (size_t)col * (size_t)m];
            for (inner = row + 1; inner < m; inner++) {
              value -= psyche_cublas_striangular_value(A, uplo, trans, diag, row, inner, lda) *
                       tmp[(size_t)inner + (size_t)col * (size_t)m];
            }
            if (diag == CUBLAS_DIAG_NON_UNIT) {
              value /= psyche_cublas_striangular_value(A, uplo, trans, diag, row, row, lda);
            }
            tmp[(size_t)row + (size_t)col * (size_t)m] = value;
          }
        }
      }
    } else {
      for (row = 0; row < m; row++) {
        if (op_is_lower) {
          for (col = n - 1; col >= 0; col--) {
            float value = tmp[(size_t)row + (size_t)col * (size_t)m];
            for (inner = col + 1; inner < n; inner++) {
              value -= tmp[(size_t)row + (size_t)inner * (size_t)m] *
                       psyche_cublas_striangular_value(A, uplo, trans, diag, inner, col, lda);
            }
            if (diag == CUBLAS_DIAG_NON_UNIT) {
              value /= psyche_cublas_striangular_value(A, uplo, trans, diag, col, col, lda);
            }
            tmp[(size_t)row + (size_t)col * (size_t)m] = value;
          }
        } else {
          for (col = 0; col < n; col++) {
            float value = tmp[(size_t)row + (size_t)col * (size_t)m];
            for (inner = 0; inner < col; inner++) {
              value -= tmp[(size_t)row + (size_t)inner * (size_t)m] *
                       psyche_cublas_striangular_value(A, uplo, trans, diag, inner, col, lda);
            }
            if (diag == CUBLAS_DIAG_NON_UNIT) {
              value /= psyche_cublas_striangular_value(A, uplo, trans, diag, col, col, lda);
            }
            tmp[(size_t)row + (size_t)col * (size_t)m] = value;
          }
        }
      }
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&B[(size_t)col * (size_t)ldb], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dtrsm_impl(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    double *B,
    int ldb) {
  cublasStatus_t status =
      psyche_cublas_validate_trsm_args(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
  double alpha_value = 0.0;
  double *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  int op_is_lower = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (alpha_value != 0.0 && A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (alpha_value == 0.0) {
    /* cuBLAS does not reference A or read B for alpha zero; B is only output storage. */
    psyche_cublas_dzero_matrix(m, n, B, ldb);
    return CUBLAS_STATUS_SUCCESS;
  }
#if defined(__APPLE__)
  cblas_dtrsm(
      CblasColMajor,
      psyche_cublas_accelerate_side(side),
      psyche_cublas_accelerate_uplo(uplo),
      psyche_cublas_accelerate_trans(trans),
      psyche_cublas_accelerate_diag(diag),
      m,
      n,
      alpha_value,
      A,
      lda,
      B,
      ldb);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      tmp[(size_t)row + (size_t)col * (size_t)m] =
          alpha_value * B[(size_t)row + (size_t)col * (size_t)ldb];
    }
  }
  /* Match cuBLAS behavior: do not pre-test triangular singularity. */
  if (alpha_value != 0.0) {
    op_is_lower = trans == CUBLAS_OP_N
        ? uplo == CUBLAS_FILL_MODE_LOWER
        : uplo == CUBLAS_FILL_MODE_UPPER;
    if (side == CUBLAS_SIDE_LEFT) {
      if (op_is_lower) {
        for (col = 0; col < n; col++) {
          for (row = 0; row < m; row++) {
            double value = tmp[(size_t)row + (size_t)col * (size_t)m];
            for (inner = 0; inner < row; inner++) {
              value -= psyche_cublas_dtriangular_value(A, uplo, trans, diag, row, inner, lda) *
                       tmp[(size_t)inner + (size_t)col * (size_t)m];
            }
            if (diag == CUBLAS_DIAG_NON_UNIT) {
              value /= psyche_cublas_dtriangular_value(A, uplo, trans, diag, row, row, lda);
            }
            tmp[(size_t)row + (size_t)col * (size_t)m] = value;
          }
        }
      } else {
        for (col = 0; col < n; col++) {
          for (row = m - 1; row >= 0; row--) {
            double value = tmp[(size_t)row + (size_t)col * (size_t)m];
            for (inner = row + 1; inner < m; inner++) {
              value -= psyche_cublas_dtriangular_value(A, uplo, trans, diag, row, inner, lda) *
                       tmp[(size_t)inner + (size_t)col * (size_t)m];
            }
            if (diag == CUBLAS_DIAG_NON_UNIT) {
              value /= psyche_cublas_dtriangular_value(A, uplo, trans, diag, row, row, lda);
            }
            tmp[(size_t)row + (size_t)col * (size_t)m] = value;
          }
        }
      }
    } else {
      for (row = 0; row < m; row++) {
        if (op_is_lower) {
          for (col = n - 1; col >= 0; col--) {
            double value = tmp[(size_t)row + (size_t)col * (size_t)m];
            for (inner = col + 1; inner < n; inner++) {
              value -= tmp[(size_t)row + (size_t)inner * (size_t)m] *
                       psyche_cublas_dtriangular_value(A, uplo, trans, diag, inner, col, lda);
            }
            if (diag == CUBLAS_DIAG_NON_UNIT) {
              value /= psyche_cublas_dtriangular_value(A, uplo, trans, diag, col, col, lda);
            }
            tmp[(size_t)row + (size_t)col * (size_t)m] = value;
          }
        } else {
          for (col = 0; col < n; col++) {
            double value = tmp[(size_t)row + (size_t)col * (size_t)m];
            for (inner = 0; inner < col; inner++) {
              value -= tmp[(size_t)row + (size_t)inner * (size_t)m] *
                       psyche_cublas_dtriangular_value(A, uplo, trans, diag, inner, col, lda);
            }
            if (diag == CUBLAS_DIAG_NON_UNIT) {
              value /= psyche_cublas_dtriangular_value(A, uplo, trans, diag, col, col, lda);
            }
            tmp[(size_t)row + (size_t)col * (size_t)m] = value;
          }
        }
      }
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&B[(size_t)col * (size_t)ldb], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cuComplex psyche_cublas_cadd(cuComplex a, cuComplex b) {
  cuComplex result;
  result.x = a.x + b.x;
  result.y = a.y + b.y;
  return result;
}

static cuComplex psyche_cublas_csub(cuComplex a, cuComplex b) {
  cuComplex result;
  result.x = a.x - b.x;
  result.y = a.y - b.y;
  return result;
}

static cuComplex psyche_cublas_cmul(cuComplex a, cuComplex b) {
  cuComplex result;
  result.x = a.x * b.x - a.y * b.y;
  result.y = a.x * b.y + a.y * b.x;
  return result;
}

static cuComplex psyche_cublas_cdiv(cuComplex a, cuComplex b) {
  float denom = b.x * b.x + b.y * b.y;
  cuComplex result;
  result.x = (a.x * b.x + a.y * b.y) / denom;
  result.y = (a.y * b.x - a.x * b.y) / denom;
  return result;
}

static cuComplex psyche_cublas_cconj(cuComplex value) {
  cuComplex result;
  result.x = value.x;
  result.y = -value.y;
  return result;
}

static cuComplex psyche_cublas_czero(void) {
  cuComplex result;
  result.x = 0.0f;
  result.y = 0.0f;
  return result;
}

static cuComplex psyche_cublas_cone(void) {
  cuComplex result;
  result.x = 1.0f;
  result.y = 0.0f;
  return result;
}

static int psyche_cublas_cis_zero(cuComplex value) {
  return value.x == 0.0f && value.y == 0.0f;
}

static cuDoubleComplex psyche_cublas_zadd(cuDoubleComplex a, cuDoubleComplex b) {
  cuDoubleComplex result;
  result.x = a.x + b.x;
  result.y = a.y + b.y;
  return result;
}

static cuDoubleComplex psyche_cublas_zsub(cuDoubleComplex a, cuDoubleComplex b) {
  cuDoubleComplex result;
  result.x = a.x - b.x;
  result.y = a.y - b.y;
  return result;
}

static cuDoubleComplex psyche_cublas_zmul(cuDoubleComplex a, cuDoubleComplex b) {
  cuDoubleComplex result;
  result.x = a.x * b.x - a.y * b.y;
  result.y = a.x * b.y + a.y * b.x;
  return result;
}

static cuDoubleComplex psyche_cublas_zdiv(cuDoubleComplex a, cuDoubleComplex b) {
  double denom = b.x * b.x + b.y * b.y;
  cuDoubleComplex result;
  result.x = (a.x * b.x + a.y * b.y) / denom;
  result.y = (a.y * b.x - a.x * b.y) / denom;
  return result;
}

static cuDoubleComplex psyche_cublas_zconj(cuDoubleComplex value) {
  cuDoubleComplex result;
  result.x = value.x;
  result.y = -value.y;
  return result;
}

static cuDoubleComplex psyche_cublas_zzero(void) {
  cuDoubleComplex result;
  result.x = 0.0;
  result.y = 0.0;
  return result;
}

static cuDoubleComplex psyche_cublas_zone(void) {
  cuDoubleComplex result;
  result.x = 1.0;
  result.y = 0.0;
  return result;
}

static int psyche_cublas_zis_zero(cuDoubleComplex value) {
  return value.x == 0.0 && value.y == 0.0;
}

static void psyche_cublas_czero_matrix(int rows, int cols, cuComplex *matrix, int ld) {
  int row = 0;
  int col = 0;
  cuComplex zero = psyche_cublas_czero();
  for (col = 0; col < cols; col++) {
    for (row = 0; row < rows; row++) {
      matrix[(size_t)row + (size_t)col * (size_t)ld] = zero;
    }
  }
}

static void psyche_cublas_zzero_matrix(int rows, int cols, cuDoubleComplex *matrix, int ld) {
  int row = 0;
  int col = 0;
  cuDoubleComplex zero = psyche_cublas_zzero();
  for (col = 0; col < cols; col++) {
    for (row = 0; row < rows; row++) {
      matrix[(size_t)row + (size_t)col * (size_t)ld] = zero;
    }
  }
}

static cuComplex psyche_cublas_cgemm_a(
    const cuComplex *A,
    cublasOperation_t transa,
    int row,
    int inner,
    int lda) {
  cuComplex value = transa == CUBLAS_OP_N
      ? A[(size_t)row + (size_t)inner * (size_t)lda]
      : A[(size_t)inner + (size_t)row * (size_t)lda];
  return transa == CUBLAS_OP_C ? psyche_cublas_cconj(value) : value;
}

static cuComplex psyche_cublas_cgemm_b(
    const cuComplex *B,
    cublasOperation_t transb,
    int inner,
    int col,
    int ldb) {
  cuComplex value = transb == CUBLAS_OP_N
      ? B[(size_t)inner + (size_t)col * (size_t)ldb]
      : B[(size_t)col + (size_t)inner * (size_t)ldb];
  return transb == CUBLAS_OP_C ? psyche_cublas_cconj(value) : value;
}

static cuDoubleComplex psyche_cublas_zgemm_a(
    const cuDoubleComplex *A,
    cublasOperation_t transa,
    int row,
    int inner,
    int lda) {
  cuDoubleComplex value = transa == CUBLAS_OP_N
      ? A[(size_t)row + (size_t)inner * (size_t)lda]
      : A[(size_t)inner + (size_t)row * (size_t)lda];
  return transa == CUBLAS_OP_C ? psyche_cublas_zconj(value) : value;
}

static cuDoubleComplex psyche_cublas_zgemm_b(
    const cuDoubleComplex *B,
    cublasOperation_t transb,
    int inner,
    int col,
    int ldb) {
  cuDoubleComplex value = transb == CUBLAS_OP_N
      ? B[(size_t)inner + (size_t)col * (size_t)ldb]
      : B[(size_t)col + (size_t)inner * (size_t)ldb];
  return transb == CUBLAS_OP_C ? psyche_cublas_zconj(value) : value;
}

static cuComplex psyche_cublas_cgemv_a(
    const cuComplex *A,
    cublasOperation_t trans,
    int out,
    int inner,
    int lda) {
  cuComplex value = trans == CUBLAS_OP_N
      ? A[(size_t)out + (size_t)inner * (size_t)lda]
      : A[(size_t)inner + (size_t)out * (size_t)lda];
  return trans == CUBLAS_OP_C ? psyche_cublas_cconj(value) : value;
}

static cuDoubleComplex psyche_cublas_zgemv_a(
    const cuDoubleComplex *A,
    cublasOperation_t trans,
    int out,
    int inner,
    int lda) {
  cuDoubleComplex value = trans == CUBLAS_OP_N
      ? A[(size_t)out + (size_t)inner * (size_t)lda]
      : A[(size_t)inner + (size_t)out * (size_t)lda];
  return trans == CUBLAS_OP_C ? psyche_cublas_zconj(value) : value;
}

static cuComplex psyche_cublas_cscale_real(cuComplex value, float scale) {
  cuComplex result;
  result.x = value.x * scale;
  result.y = value.y * scale;
  return result;
}

static cuDoubleComplex psyche_cublas_zscale_real(cuDoubleComplex value, double scale) {
  cuDoubleComplex result;
  result.x = value.x * scale;
  result.y = value.y * scale;
  return result;
}

static cublasStatus_t psyche_cublas_cherk_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const float *alpha,
    const cuComplex *A,
    int lda,
    const float *beta,
    cuComplex *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_herk_args(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
  float alpha_value = 0.0f;
  float beta_value = 0.0f;
  cuComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int product_required = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  product_required = alpha_value != 0.0f && k > 0;
  if (product_required && A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t tmp_index = (size_t)row + (size_t)col * (size_t)n;
      size_t c_index = (size_t)row + (size_t)col * (size_t)ldc;
      cuComplex product = psyche_cublas_czero();
      cuComplex existing = psyche_cublas_czero();
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        tmp[tmp_index] = psyche_cublas_czero();
        continue;
      }
      if (product_required) {
        cuComplex acc = psyche_cublas_czero();
        for (inner = 0; inner < k; inner++) {
          acc = psyche_cublas_cadd(
              acc,
              psyche_cublas_cmul(
                  psyche_cublas_cgemm_a(A, trans, row, inner, lda),
                  psyche_cublas_cconj(psyche_cublas_cgemm_a(A, trans, col, inner, lda))));
        }
        product = psyche_cublas_cscale_real(acc, alpha_value);
      }
      if (beta_value != 0.0f) {
        if (row == col) {
          existing.x = beta_value * C[c_index].x;
          existing.y = 0.0f;
        } else {
          existing = psyche_cublas_cscale_real(C[c_index], beta_value);
        }
      }
      tmp[tmp_index] = psyche_cublas_cadd(product, existing);
      if (row == col) {
        tmp[tmp_index].y = 0.0f;
      }
    }
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      if (psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        C[(size_t)row + (size_t)col * (size_t)ldc] = tmp[(size_t)row + (size_t)col * (size_t)n];
      }
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zherk_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const double *alpha,
    const cuDoubleComplex *A,
    int lda,
    const double *beta,
    cuDoubleComplex *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_herk_args(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
  double alpha_value = 0.0;
  double beta_value = 0.0;
  cuDoubleComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int product_required = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  product_required = alpha_value != 0.0 && k > 0;
  if (product_required && A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t tmp_index = (size_t)row + (size_t)col * (size_t)n;
      size_t c_index = (size_t)row + (size_t)col * (size_t)ldc;
      cuDoubleComplex product = psyche_cublas_zzero();
      cuDoubleComplex existing = psyche_cublas_zzero();
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        tmp[tmp_index] = psyche_cublas_zzero();
        continue;
      }
      if (product_required) {
        cuDoubleComplex acc = psyche_cublas_zzero();
        for (inner = 0; inner < k; inner++) {
          acc = psyche_cublas_zadd(
              acc,
              psyche_cublas_zmul(
                  psyche_cublas_zgemm_a(A, trans, row, inner, lda),
                  psyche_cublas_zconj(psyche_cublas_zgemm_a(A, trans, col, inner, lda))));
        }
        product = psyche_cublas_zscale_real(acc, alpha_value);
      }
      if (beta_value != 0.0) {
        if (row == col) {
          existing.x = beta_value * C[c_index].x;
          existing.y = 0.0;
        } else {
          existing = psyche_cublas_zscale_real(C[c_index], beta_value);
        }
      }
      tmp[tmp_index] = psyche_cublas_zadd(product, existing);
      if (row == col) {
        tmp[tmp_index].y = 0.0;
      }
    }
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      if (psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        C[(size_t)row + (size_t)col * (size_t)ldc] = tmp[(size_t)row + (size_t)col * (size_t)n];
      }
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_cher2k_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    const float *beta,
    cuComplex *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_her2k_args(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
  cuComplex alpha_value = psyche_cublas_czero();
  cuComplex alpha_conj = psyche_cublas_czero();
  float beta_value = 0.0f;
  cuComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int product_required = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  alpha_conj = psyche_cublas_cconj(alpha_value);
  beta_value = *beta;
  product_required = !psyche_cublas_cis_zero(alpha_value) && k > 0;
  if (product_required && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t tmp_index = (size_t)row + (size_t)col * (size_t)n;
      size_t c_index = (size_t)row + (size_t)col * (size_t)ldc;
      cuComplex product = psyche_cublas_czero();
      cuComplex existing = psyche_cublas_czero();
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        tmp[tmp_index] = psyche_cublas_czero();
        continue;
      }
      if (product_required) {
        cuComplex acc_ab = psyche_cublas_czero();
        cuComplex acc_ba = psyche_cublas_czero();
        for (inner = 0; inner < k; inner++) {
          acc_ab = psyche_cublas_cadd(
              acc_ab,
              psyche_cublas_cmul(
                  psyche_cublas_cgemm_a(A, trans, row, inner, lda),
                  psyche_cublas_cconj(psyche_cublas_cgemm_a(B, trans, col, inner, ldb))));
          acc_ba = psyche_cublas_cadd(
              acc_ba,
              psyche_cublas_cmul(
                  psyche_cublas_cgemm_a(B, trans, row, inner, ldb),
                  psyche_cublas_cconj(psyche_cublas_cgemm_a(A, trans, col, inner, lda))));
        }
        product = psyche_cublas_cadd(
            psyche_cublas_cmul(alpha_value, acc_ab),
            psyche_cublas_cmul(alpha_conj, acc_ba));
      }
      if (beta_value != 0.0f) {
        if (row == col) {
          existing.x = beta_value * C[c_index].x;
          existing.y = 0.0f;
        } else {
          existing = psyche_cublas_cscale_real(C[c_index], beta_value);
        }
      }
      tmp[tmp_index] = psyche_cublas_cadd(product, existing);
      if (row == col) {
        tmp[tmp_index].y = 0.0f;
      }
    }
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      if (psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        C[(size_t)row + (size_t)col * (size_t)ldc] = tmp[(size_t)row + (size_t)col * (size_t)n];
      }
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zher2k_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    const double *beta,
    cuDoubleComplex *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_her2k_args(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
  cuDoubleComplex alpha_value = psyche_cublas_zzero();
  cuDoubleComplex alpha_conj = psyche_cublas_zzero();
  double beta_value = 0.0;
  cuDoubleComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int product_required = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  alpha_conj = psyche_cublas_zconj(alpha_value);
  beta_value = *beta;
  product_required = !psyche_cublas_zis_zero(alpha_value) && k > 0;
  if (product_required && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t tmp_index = (size_t)row + (size_t)col * (size_t)n;
      size_t c_index = (size_t)row + (size_t)col * (size_t)ldc;
      cuDoubleComplex product = psyche_cublas_zzero();
      cuDoubleComplex existing = psyche_cublas_zzero();
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        tmp[tmp_index] = psyche_cublas_zzero();
        continue;
      }
      if (product_required) {
        cuDoubleComplex acc_ab = psyche_cublas_zzero();
        cuDoubleComplex acc_ba = psyche_cublas_zzero();
        for (inner = 0; inner < k; inner++) {
          acc_ab = psyche_cublas_zadd(
              acc_ab,
              psyche_cublas_zmul(
                  psyche_cublas_zgemm_a(A, trans, row, inner, lda),
                  psyche_cublas_zconj(psyche_cublas_zgemm_a(B, trans, col, inner, ldb))));
          acc_ba = psyche_cublas_zadd(
              acc_ba,
              psyche_cublas_zmul(
                  psyche_cublas_zgemm_a(B, trans, row, inner, ldb),
                  psyche_cublas_zconj(psyche_cublas_zgemm_a(A, trans, col, inner, lda))));
        }
        product = psyche_cublas_zadd(
            psyche_cublas_zmul(alpha_value, acc_ab),
            psyche_cublas_zmul(alpha_conj, acc_ba));
      }
      if (beta_value != 0.0) {
        if (row == col) {
          existing.x = beta_value * C[c_index].x;
          existing.y = 0.0;
        } else {
          existing = psyche_cublas_zscale_real(C[c_index], beta_value);
        }
      }
      tmp[tmp_index] = psyche_cublas_zadd(product, existing);
      if (row == col) {
        tmp[tmp_index].y = 0.0;
      }
    }
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      if (psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        C[(size_t)row + (size_t)col * (size_t)ldc] = tmp[(size_t)row + (size_t)col * (size_t)n];
      }
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static float psyche_cublas_cabs_squared(cuComplex value) {
  return value.x * value.x + value.y * value.y;
}

static double psyche_cublas_zabs_squared(cuDoubleComplex value) {
  return value.x * value.x + value.y * value.y;
}

static cuComplex psyche_cublas_chermitian_value(
    const cuComplex *A,
    cublasFillMode_t uplo,
    int row,
    int col,
    int lda) {
  cuComplex value;
  if (row == col) {
    value = A[(size_t)row + (size_t)col * (size_t)lda];
    value.y = 0.0f;
    return value;
  }
  if (psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
    return A[(size_t)row + (size_t)col * (size_t)lda];
  }
  return psyche_cublas_cconj(A[(size_t)col + (size_t)row * (size_t)lda]);
}

static cuDoubleComplex psyche_cublas_zhermitian_value(
    const cuDoubleComplex *A,
    cublasFillMode_t uplo,
    int row,
    int col,
    int lda) {
  cuDoubleComplex value;
  if (row == col) {
    value = A[(size_t)row + (size_t)col * (size_t)lda];
    value.y = 0.0;
    return value;
  }
  if (psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
    return A[(size_t)row + (size_t)col * (size_t)lda];
  }
  return psyche_cublas_zconj(A[(size_t)col + (size_t)row * (size_t)lda]);
}

static cuComplex psyche_cublas_ctriangular_value(
    const cuComplex *A,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int row,
    int col,
    int lda) {
  int source_row = trans == CUBLAS_OP_N ? row : col;
  int source_col = trans == CUBLAS_OP_N ? col : row;
  cuComplex value;
  if (!psyche_cublas_triangular_element_is_stored(uplo, source_row, source_col)) {
    return psyche_cublas_czero();
  }
  if (source_row == source_col && diag == CUBLAS_DIAG_UNIT) {
    return psyche_cublas_cone();
  }
  value = A[(size_t)source_row + (size_t)source_col * (size_t)lda];
  return trans == CUBLAS_OP_C ? psyche_cublas_cconj(value) : value;
}

static cuDoubleComplex psyche_cublas_ztriangular_value(
    const cuDoubleComplex *A,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int row,
    int col,
    int lda) {
  int source_row = trans == CUBLAS_OP_N ? row : col;
  int source_col = trans == CUBLAS_OP_N ? col : row;
  cuDoubleComplex value;
  if (!psyche_cublas_triangular_element_is_stored(uplo, source_row, source_col)) {
    return psyche_cublas_zzero();
  }
  if (source_row == source_col && diag == CUBLAS_DIAG_UNIT) {
    return psyche_cublas_zone();
  }
  value = A[(size_t)source_row + (size_t)source_col * (size_t)lda];
  return trans == CUBLAS_OP_C ? psyche_cublas_zconj(value) : value;
}

static cublasStatus_t psyche_cublas_ctrmv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuComplex *A,
    int lda,
    cuComplex *x,
    int incx) {
  cublasStatus_t status =
      psyche_cublas_validate_trmv_trsv_args(handle, uplo, trans, diag, n, A, lda, x, incx);
  cuComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    cuComplex acc = psyche_cublas_czero();
    for (col = 0; col < n; col++) {
      acc = psyche_cublas_cadd(
          acc,
          psyche_cublas_cmul(
              psyche_cublas_ctriangular_value(A, uplo, trans, diag, row, col, lda),
              x[psyche_cublas_signed_stride_index(n, incx, col)]));
    }
    tmp[row] = acc;
  }
  for (row = 0; row < n; row++) {
    x[psyche_cublas_signed_stride_index(n, incx, row)] = tmp[row];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ztrmv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuDoubleComplex *A,
    int lda,
    cuDoubleComplex *x,
    int incx) {
  cublasStatus_t status =
      psyche_cublas_validate_trmv_trsv_args(handle, uplo, trans, diag, n, A, lda, x, incx);
  cuDoubleComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    cuDoubleComplex acc = psyche_cublas_zzero();
    for (col = 0; col < n; col++) {
      acc = psyche_cublas_zadd(
          acc,
          psyche_cublas_zmul(
              psyche_cublas_ztriangular_value(A, uplo, trans, diag, row, col, lda),
              x[psyche_cublas_signed_stride_index(n, incx, col)]));
    }
    tmp[row] = acc;
  }
  for (row = 0; row < n; row++) {
    x[psyche_cublas_signed_stride_index(n, incx, row)] = tmp[row];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ctrsv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuComplex *A,
    int lda,
    cuComplex *x,
    int incx) {
  cublasStatus_t status =
      psyche_cublas_validate_trmv_trsv_args(handle, uplo, trans, diag, n, A, lda, x, incx);
  cuComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int op_is_lower = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
  }
  op_is_lower = trans == CUBLAS_OP_N
      ? uplo == CUBLAS_FILL_MODE_LOWER
      : uplo == CUBLAS_FILL_MODE_UPPER;
  if (op_is_lower) {
    for (row = 0; row < n; row++) {
      cuComplex value = tmp[row];
      for (col = 0; col < row; col++) {
        value = psyche_cublas_csub(
            value,
            psyche_cublas_cmul(
                psyche_cublas_ctriangular_value(A, uplo, trans, diag, row, col, lda),
                tmp[col]));
      }
      if (diag == CUBLAS_DIAG_NON_UNIT) {
        value = psyche_cublas_cdiv(
            value,
            psyche_cublas_ctriangular_value(A, uplo, trans, diag, row, row, lda));
      }
      tmp[row] = value;
    }
  } else {
    for (row = n - 1; row >= 0; row--) {
      cuComplex value = tmp[row];
      for (col = row + 1; col < n; col++) {
        value = psyche_cublas_csub(
            value,
            psyche_cublas_cmul(
                psyche_cublas_ctriangular_value(A, uplo, trans, diag, row, col, lda),
                tmp[col]));
      }
      if (diag == CUBLAS_DIAG_NON_UNIT) {
        value = psyche_cublas_cdiv(
            value,
            psyche_cublas_ctriangular_value(A, uplo, trans, diag, row, row, lda));
      }
      tmp[row] = value;
    }
  }
  for (row = 0; row < n; row++) {
    x[psyche_cublas_signed_stride_index(n, incx, row)] = tmp[row];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ztrsv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuDoubleComplex *A,
    int lda,
    cuDoubleComplex *x,
    int incx) {
  cublasStatus_t status =
      psyche_cublas_validate_trmv_trsv_args(handle, uplo, trans, diag, n, A, lda, x, incx);
  cuDoubleComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int op_is_lower = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
  }
  op_is_lower = trans == CUBLAS_OP_N
      ? uplo == CUBLAS_FILL_MODE_LOWER
      : uplo == CUBLAS_FILL_MODE_UPPER;
  if (op_is_lower) {
    for (row = 0; row < n; row++) {
      cuDoubleComplex value = tmp[row];
      for (col = 0; col < row; col++) {
        value = psyche_cublas_zsub(
            value,
            psyche_cublas_zmul(
                psyche_cublas_ztriangular_value(A, uplo, trans, diag, row, col, lda),
                tmp[col]));
      }
      if (diag == CUBLAS_DIAG_NON_UNIT) {
        value = psyche_cublas_zdiv(
            value,
            psyche_cublas_ztriangular_value(A, uplo, trans, diag, row, row, lda));
      }
      tmp[row] = value;
    }
  } else {
    for (row = n - 1; row >= 0; row--) {
      cuDoubleComplex value = tmp[row];
      for (col = row + 1; col < n; col++) {
        value = psyche_cublas_zsub(
            value,
            psyche_cublas_zmul(
                psyche_cublas_ztriangular_value(A, uplo, trans, diag, row, col, lda),
                tmp[col]));
      }
      if (diag == CUBLAS_DIAG_NON_UNIT) {
        value = psyche_cublas_zdiv(
            value,
            psyche_cublas_ztriangular_value(A, uplo, trans, diag, row, row, lda));
      }
      tmp[row] = value;
    }
  }
  for (row = 0; row < n; row++) {
    x[psyche_cublas_signed_stride_index(n, incx, row)] = tmp[row];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ctrmm_impl(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    cuComplex *C,
    int ldc) {
  cublasStatus_t status =
      psyche_cublas_validate_trmm_args(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
  cuComplex alpha_value = psyche_cublas_czero();
  cuComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (!psyche_cublas_cis_zero(alpha_value) && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublas_cis_zero(alpha_value)) {
    /* cuBLAS does not reference A/B for alpha zero, but C is still output storage. */
    psyche_cublas_czero_matrix(m, n, C, ldc);
    return CUBLAS_STATUS_SUCCESS;
  }
#if defined(__APPLE__)
  status = psyche_cublas_copy_matrix_bytes(m, n, (int)sizeof(*B), B, ldb, C, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  cblas_ctrmm(
      CblasColMajor,
      psyche_cublas_accelerate_side(side),
      psyche_cublas_accelerate_uplo(uplo),
      psyche_cublas_accelerate_trans(trans),
      psyche_cublas_accelerate_diag(diag),
      m,
      n,
      (const __LAPACK_float_complex *)alpha,
      (const __LAPACK_float_complex *)A,
      lda,
      (__LAPACK_float_complex *)C,
      ldc);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      cuComplex acc = psyche_cublas_czero();
      if (side == CUBLAS_SIDE_LEFT) {
        for (inner = 0; inner < m; inner++) {
          acc = psyche_cublas_cadd(
              acc,
              psyche_cublas_cmul(
                  psyche_cublas_ctriangular_value(A, uplo, trans, diag, row, inner, lda),
                  B[(size_t)inner + (size_t)col * (size_t)ldb]));
        }
      } else {
        for (inner = 0; inner < n; inner++) {
          acc = psyche_cublas_cadd(
              acc,
              psyche_cublas_cmul(
                  B[(size_t)row + (size_t)inner * (size_t)ldb],
                  psyche_cublas_ctriangular_value(A, uplo, trans, diag, inner, col, lda)));
        }
      }
      tmp[(size_t)row + (size_t)col * (size_t)m] = psyche_cublas_cmul(alpha_value, acc);
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ztrmm_impl(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    cuDoubleComplex *C,
    int ldc) {
  cublasStatus_t status =
      psyche_cublas_validate_trmm_args(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
  cuDoubleComplex alpha_value = psyche_cublas_zzero();
  cuDoubleComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (!psyche_cublas_zis_zero(alpha_value) && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublas_zis_zero(alpha_value)) {
    /* cuBLAS does not reference A/B for alpha zero, but C is still output storage. */
    psyche_cublas_zzero_matrix(m, n, C, ldc);
    return CUBLAS_STATUS_SUCCESS;
  }
#if defined(__APPLE__)
  status = psyche_cublas_copy_matrix_bytes(m, n, (int)sizeof(*B), B, ldb, C, ldc);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  cblas_ztrmm(
      CblasColMajor,
      psyche_cublas_accelerate_side(side),
      psyche_cublas_accelerate_uplo(uplo),
      psyche_cublas_accelerate_trans(trans),
      psyche_cublas_accelerate_diag(diag),
      m,
      n,
      (const __LAPACK_double_complex *)alpha,
      (const __LAPACK_double_complex *)A,
      lda,
      (__LAPACK_double_complex *)C,
      ldc);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      cuDoubleComplex acc = psyche_cublas_zzero();
      if (side == CUBLAS_SIDE_LEFT) {
        for (inner = 0; inner < m; inner++) {
          acc = psyche_cublas_zadd(
              acc,
              psyche_cublas_zmul(
                  psyche_cublas_ztriangular_value(A, uplo, trans, diag, row, inner, lda),
                  B[(size_t)inner + (size_t)col * (size_t)ldb]));
        }
      } else {
        for (inner = 0; inner < n; inner++) {
          acc = psyche_cublas_zadd(
              acc,
              psyche_cublas_zmul(
                  B[(size_t)row + (size_t)inner * (size_t)ldb],
                  psyche_cublas_ztriangular_value(A, uplo, trans, diag, inner, col, lda)));
        }
      }
      tmp[(size_t)row + (size_t)col * (size_t)m] = psyche_cublas_zmul(alpha_value, acc);
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ctrsm_impl(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    cuComplex *B,
    int ldb) {
  cublasStatus_t status =
      psyche_cublas_validate_trsm_args(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
  cuComplex alpha_value = psyche_cublas_czero();
  cuComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  int op_is_lower = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (!psyche_cublas_cis_zero(alpha_value) && A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublas_cis_zero(alpha_value)) {
    /* cuBLAS does not reference A or read B for alpha zero; B is only output storage. */
    psyche_cublas_czero_matrix(m, n, B, ldb);
    return CUBLAS_STATUS_SUCCESS;
  }
#if defined(__APPLE__)
  cblas_ctrsm(
      CblasColMajor,
      psyche_cublas_accelerate_side(side),
      psyche_cublas_accelerate_uplo(uplo),
      psyche_cublas_accelerate_trans(trans),
      psyche_cublas_accelerate_diag(diag),
      m,
      n,
      (const __LAPACK_float_complex *)alpha,
      (const __LAPACK_float_complex *)A,
      lda,
      (__LAPACK_float_complex *)B,
      ldb);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      tmp[(size_t)row + (size_t)col * (size_t)m] =
          psyche_cublas_cmul(alpha_value, B[(size_t)row + (size_t)col * (size_t)ldb]);
    }
  }
  /* Match cuBLAS behavior: do not pre-test triangular singularity. */
  op_is_lower = trans == CUBLAS_OP_N
      ? uplo == CUBLAS_FILL_MODE_LOWER
      : uplo == CUBLAS_FILL_MODE_UPPER;
  if (side == CUBLAS_SIDE_LEFT) {
    if (op_is_lower) {
      for (col = 0; col < n; col++) {
        for (row = 0; row < m; row++) {
          cuComplex value = tmp[(size_t)row + (size_t)col * (size_t)m];
          for (inner = 0; inner < row; inner++) {
            value = psyche_cublas_csub(
                value,
                psyche_cublas_cmul(
                    psyche_cublas_ctriangular_value(A, uplo, trans, diag, row, inner, lda),
                    tmp[(size_t)inner + (size_t)col * (size_t)m]));
          }
          if (diag == CUBLAS_DIAG_NON_UNIT) {
            value = psyche_cublas_cdiv(
                value,
                psyche_cublas_ctriangular_value(A, uplo, trans, diag, row, row, lda));
          }
          tmp[(size_t)row + (size_t)col * (size_t)m] = value;
        }
      }
    } else {
      for (col = 0; col < n; col++) {
        for (row = m - 1; row >= 0; row--) {
          cuComplex value = tmp[(size_t)row + (size_t)col * (size_t)m];
          for (inner = row + 1; inner < m; inner++) {
            value = psyche_cublas_csub(
                value,
                psyche_cublas_cmul(
                    psyche_cublas_ctriangular_value(A, uplo, trans, diag, row, inner, lda),
                    tmp[(size_t)inner + (size_t)col * (size_t)m]));
          }
          if (diag == CUBLAS_DIAG_NON_UNIT) {
            value = psyche_cublas_cdiv(
                value,
                psyche_cublas_ctriangular_value(A, uplo, trans, diag, row, row, lda));
          }
          tmp[(size_t)row + (size_t)col * (size_t)m] = value;
        }
      }
    }
  } else {
    for (row = 0; row < m; row++) {
      if (op_is_lower) {
        for (col = n - 1; col >= 0; col--) {
          cuComplex value = tmp[(size_t)row + (size_t)col * (size_t)m];
          for (inner = col + 1; inner < n; inner++) {
            value = psyche_cublas_csub(
                value,
                psyche_cublas_cmul(
                    tmp[(size_t)row + (size_t)inner * (size_t)m],
                    psyche_cublas_ctriangular_value(A, uplo, trans, diag, inner, col, lda)));
          }
          if (diag == CUBLAS_DIAG_NON_UNIT) {
            value = psyche_cublas_cdiv(
                value,
                psyche_cublas_ctriangular_value(A, uplo, trans, diag, col, col, lda));
          }
          tmp[(size_t)row + (size_t)col * (size_t)m] = value;
        }
      } else {
        for (col = 0; col < n; col++) {
          cuComplex value = tmp[(size_t)row + (size_t)col * (size_t)m];
          for (inner = 0; inner < col; inner++) {
            value = psyche_cublas_csub(
                value,
                psyche_cublas_cmul(
                    tmp[(size_t)row + (size_t)inner * (size_t)m],
                    psyche_cublas_ctriangular_value(A, uplo, trans, diag, inner, col, lda)));
          }
          if (diag == CUBLAS_DIAG_NON_UNIT) {
            value = psyche_cublas_cdiv(
                value,
                psyche_cublas_ctriangular_value(A, uplo, trans, diag, col, col, lda));
          }
          tmp[(size_t)row + (size_t)col * (size_t)m] = value;
        }
      }
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&B[(size_t)col * (size_t)ldb], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ztrsm_impl(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    cuDoubleComplex *B,
    int ldb) {
  cublasStatus_t status =
      psyche_cublas_validate_trsm_args(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
  cuDoubleComplex alpha_value = psyche_cublas_zzero();
  cuDoubleComplex *tmp = NULL;
  size_t tmp_bytes = 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  int op_is_lower = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (!psyche_cublas_zis_zero(alpha_value) && A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublas_zis_zero(alpha_value)) {
    /* cuBLAS does not reference A or read B for alpha zero; B is only output storage. */
    psyche_cublas_zzero_matrix(m, n, B, ldb);
    return CUBLAS_STATUS_SUCCESS;
  }
#if defined(__APPLE__)
  cblas_ztrsm(
      CblasColMajor,
      psyche_cublas_accelerate_side(side),
      psyche_cublas_accelerate_uplo(uplo),
      psyche_cublas_accelerate_trans(trans),
      psyche_cublas_accelerate_diag(diag),
      m,
      n,
      (const __LAPACK_double_complex *)alpha,
      (const __LAPACK_double_complex *)A,
      lda,
      (__LAPACK_double_complex *)B,
      ldb);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      tmp[(size_t)row + (size_t)col * (size_t)m] =
          psyche_cublas_zmul(alpha_value, B[(size_t)row + (size_t)col * (size_t)ldb]);
    }
  }
  /* Match cuBLAS behavior: do not pre-test triangular singularity. */
  op_is_lower = trans == CUBLAS_OP_N
      ? uplo == CUBLAS_FILL_MODE_LOWER
      : uplo == CUBLAS_FILL_MODE_UPPER;
  if (side == CUBLAS_SIDE_LEFT) {
    if (op_is_lower) {
      for (col = 0; col < n; col++) {
        for (row = 0; row < m; row++) {
          cuDoubleComplex value = tmp[(size_t)row + (size_t)col * (size_t)m];
          for (inner = 0; inner < row; inner++) {
            value = psyche_cublas_zsub(
                value,
                psyche_cublas_zmul(
                    psyche_cublas_ztriangular_value(A, uplo, trans, diag, row, inner, lda),
                    tmp[(size_t)inner + (size_t)col * (size_t)m]));
          }
          if (diag == CUBLAS_DIAG_NON_UNIT) {
            value = psyche_cublas_zdiv(
                value,
                psyche_cublas_ztriangular_value(A, uplo, trans, diag, row, row, lda));
          }
          tmp[(size_t)row + (size_t)col * (size_t)m] = value;
        }
      }
    } else {
      for (col = 0; col < n; col++) {
        for (row = m - 1; row >= 0; row--) {
          cuDoubleComplex value = tmp[(size_t)row + (size_t)col * (size_t)m];
          for (inner = row + 1; inner < m; inner++) {
            value = psyche_cublas_zsub(
                value,
                psyche_cublas_zmul(
                    psyche_cublas_ztriangular_value(A, uplo, trans, diag, row, inner, lda),
                    tmp[(size_t)inner + (size_t)col * (size_t)m]));
          }
          if (diag == CUBLAS_DIAG_NON_UNIT) {
            value = psyche_cublas_zdiv(
                value,
                psyche_cublas_ztriangular_value(A, uplo, trans, diag, row, row, lda));
          }
          tmp[(size_t)row + (size_t)col * (size_t)m] = value;
        }
      }
    }
  } else {
    for (row = 0; row < m; row++) {
      if (op_is_lower) {
        for (col = n - 1; col >= 0; col--) {
          cuDoubleComplex value = tmp[(size_t)row + (size_t)col * (size_t)m];
          for (inner = col + 1; inner < n; inner++) {
            value = psyche_cublas_zsub(
                value,
                psyche_cublas_zmul(
                    tmp[(size_t)row + (size_t)inner * (size_t)m],
                    psyche_cublas_ztriangular_value(A, uplo, trans, diag, inner, col, lda)));
          }
          if (diag == CUBLAS_DIAG_NON_UNIT) {
            value = psyche_cublas_zdiv(
                value,
                psyche_cublas_ztriangular_value(A, uplo, trans, diag, col, col, lda));
          }
          tmp[(size_t)row + (size_t)col * (size_t)m] = value;
        }
      } else {
        for (col = 0; col < n; col++) {
          cuDoubleComplex value = tmp[(size_t)row + (size_t)col * (size_t)m];
          for (inner = 0; inner < col; inner++) {
            value = psyche_cublas_zsub(
                value,
                psyche_cublas_zmul(
                    tmp[(size_t)row + (size_t)inner * (size_t)m],
                    psyche_cublas_ztriangular_value(A, uplo, trans, diag, inner, col, lda)));
          }
          if (diag == CUBLAS_DIAG_NON_UNIT) {
            value = psyche_cublas_zdiv(
                value,
                psyche_cublas_ztriangular_value(A, uplo, trans, diag, col, col, lda));
          }
          tmp[(size_t)row + (size_t)col * (size_t)m] = value;
        }
      }
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&B[(size_t)col * (size_t)ldb], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static void psyche_cublas_cgemm_compute_with_tmp(
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    cuComplex alpha_value,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    cuComplex beta_value,
    cuComplex *C,
    int ldc,
    cuComplex *tmp) {
  int product_required = !psyche_cublas_cis_zero(alpha_value) && k > 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      cuComplex acc = psyche_cublas_czero();
      cuComplex product = psyche_cublas_czero();
      cuComplex existing = psyche_cublas_czero();
      size_t tmp_index = (size_t)row + (size_t)col * (size_t)m;
      size_t c_index = (size_t)row + (size_t)col * (size_t)ldc;
      if (product_required) {
        for (inner = 0; inner < k; inner++) {
          acc = psyche_cublas_cadd(
              acc,
              psyche_cublas_cmul(
                  psyche_cublas_cgemm_a(A, transa, row, inner, lda),
                  psyche_cublas_cgemm_b(B, transb, inner, col, ldb)));
        }
        product = psyche_cublas_cmul(alpha_value, acc);
      }
      if (!psyche_cublas_cis_zero(beta_value)) {
        existing = psyche_cublas_cmul(beta_value, C[c_index]);
      }
      tmp[tmp_index] = psyche_cublas_cadd(product, existing);
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
}

static void psyche_cublas_zgemm_compute_with_tmp(
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    cuDoubleComplex alpha_value,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    cuDoubleComplex beta_value,
    cuDoubleComplex *C,
    int ldc,
    cuDoubleComplex *tmp) {
  int product_required = !psyche_cublas_zis_zero(alpha_value) && k > 0;
  int row = 0;
  int col = 0;
  int inner = 0;
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      cuDoubleComplex acc = psyche_cublas_zzero();
      cuDoubleComplex product = psyche_cublas_zzero();
      cuDoubleComplex existing = psyche_cublas_zzero();
      size_t tmp_index = (size_t)row + (size_t)col * (size_t)m;
      size_t c_index = (size_t)row + (size_t)col * (size_t)ldc;
      if (product_required) {
        for (inner = 0; inner < k; inner++) {
          acc = psyche_cublas_zadd(
              acc,
              psyche_cublas_zmul(
                  psyche_cublas_zgemm_a(A, transa, row, inner, lda),
                  psyche_cublas_zgemm_b(B, transb, inner, col, ldb)));
        }
        product = psyche_cublas_zmul(alpha_value, acc);
      }
      if (!psyche_cublas_zis_zero(beta_value)) {
        existing = psyche_cublas_zmul(beta_value, C[c_index]);
      }
      tmp[tmp_index] = psyche_cublas_zadd(product, existing);
    }
  }
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
}

#if defined(__APPLE__)
static void psyche_cublas_cgemm_accelerate_with_tmp(
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    cuComplex beta_value,
    const cuComplex *beta,
    cuComplex *C,
    int ldc,
    cuComplex *tmp,
    size_t tmp_bytes) {
  int col = 0;
  if (!psyche_cublas_cis_zero(beta_value)) {
    for (col = 0; col < n; col++) {
      memcpy(&tmp[(size_t)col * (size_t)m], &C[(size_t)col * (size_t)ldc], (size_t)m * sizeof(*tmp));
    }
  } else {
    memset(tmp, 0, tmp_bytes);
  }
  cblas_cgemm(
      CblasColMajor,
      psyche_cublas_accelerate_trans(transa),
      psyche_cublas_accelerate_trans(transb),
      m,
      n,
      k,
      (const __LAPACK_float_complex *)alpha,
      (const __LAPACK_float_complex *)A,
      lda,
      (const __LAPACK_float_complex *)B,
      ldb,
      (const __LAPACK_float_complex *)beta,
      (__LAPACK_float_complex *)tmp,
      m);
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
}

static void psyche_cublas_zgemm_accelerate_with_tmp(
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    cuDoubleComplex beta_value,
    const cuDoubleComplex *beta,
    cuDoubleComplex *C,
    int ldc,
    cuDoubleComplex *tmp,
    size_t tmp_bytes) {
  int col = 0;
  if (!psyche_cublas_zis_zero(beta_value)) {
    for (col = 0; col < n; col++) {
      memcpy(&tmp[(size_t)col * (size_t)m], &C[(size_t)col * (size_t)ldc], (size_t)m * sizeof(*tmp));
    }
  } else {
    memset(tmp, 0, tmp_bytes);
  }
  cblas_zgemm(
      CblasColMajor,
      psyche_cublas_accelerate_trans(transa),
      psyche_cublas_accelerate_trans(transb),
      m,
      n,
      k,
      (const __LAPACK_double_complex *)alpha,
      (const __LAPACK_double_complex *)A,
      lda,
      (const __LAPACK_double_complex *)B,
      ldb,
      (const __LAPACK_double_complex *)beta,
      (__LAPACK_double_complex *)tmp,
      m);
  for (col = 0; col < n; col++) {
    memcpy(&C[(size_t)col * (size_t)ldc], &tmp[(size_t)col * (size_t)m], (size_t)m * sizeof(*tmp));
  }
}
#endif

static cublasStatus_t psyche_cublas_cgemm_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    const cuComplex *beta,
    cuComplex *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_gemm_args(
      handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
  cuComplex *tmp = NULL;
  cuComplex alpha_value = psyche_cublas_czero();
  cuComplex beta_value = psyche_cublas_czero();
  size_t tmp_bytes = 0;
  int product_required = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  product_required = !psyche_cublas_cis_zero(alpha_value) && k > 0;
  if (product_required && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
#if defined(__APPLE__)
  if (product_required) {
    psyche_cublas_cgemm_accelerate_with_tmp(
        transa, transb, m, n, k, alpha, A, lda, B, ldb, beta_value, beta, C, ldc, tmp, tmp_bytes);
    free(tmp);
    return CUBLAS_STATUS_SUCCESS;
  }
#endif
  psyche_cublas_cgemm_compute_with_tmp(
      transa, transb, m, n, k, alpha_value, A, lda, B, ldb, beta_value, C, ldc, tmp);
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zgemm_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    const cuDoubleComplex *beta,
    cuDoubleComplex *C,
    int ldc) {
  cublasStatus_t status = psyche_cublas_validate_gemm_args(
      handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
  cuDoubleComplex *tmp = NULL;
  cuDoubleComplex alpha_value = psyche_cublas_zzero();
  cuDoubleComplex beta_value = psyche_cublas_zzero();
  size_t tmp_bytes = 0;
  int product_required = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  product_required = !psyche_cublas_zis_zero(alpha_value) && k > 0;
  if (product_required && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
#if defined(__APPLE__)
  if (product_required) {
    psyche_cublas_zgemm_accelerate_with_tmp(
        transa, transb, m, n, k, alpha, A, lda, B, ldb, beta_value, beta, C, ldc, tmp, tmp_bytes);
    free(tmp);
    return CUBLAS_STATUS_SUCCESS;
  }
#endif
  psyche_cublas_zgemm_compute_with_tmp(
      transa, transb, m, n, k, alpha_value, A, lda, B, ldb, beta_value, C, ldc, tmp);
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_cgemm_strided_batched_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    long long strideA,
    const cuComplex *B,
    int ldb,
    long long strideB,
    const cuComplex *beta,
    cuComplex *C,
    int ldc,
    long long strideC,
    int batchCount) {
  cublasStatus_t status = psyche_cublas_validate_gemm_strided_batched_args(
      handle, transa, transb, m, n, k, alpha, A, lda, strideA, B, ldb, strideB, beta, C, ldc, strideC, batchCount);
  cuComplex *tmp = NULL;
  cuComplex alpha_value = psyche_cublas_czero();
  cuComplex beta_value = psyche_cublas_czero();
  size_t tmp_bytes = 0;
  int product_required = 0;
  int batch = 0;
  if (status != CUBLAS_STATUS_SUCCESS || batchCount == 0 || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  product_required = !psyche_cublas_cis_zero(alpha_value) && k > 0;
  if (product_required && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
#if defined(__APPLE__)
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (batch = 0; batch < batchCount; batch++) {
    ptrdiff_t a_offset = (ptrdiff_t)batch * (ptrdiff_t)strideA;
    ptrdiff_t b_offset = (ptrdiff_t)batch * (ptrdiff_t)strideB;
    ptrdiff_t c_offset = (ptrdiff_t)batch * (ptrdiff_t)strideC;
    const cuComplex *batch_A = A == NULL ? NULL : A + a_offset;
    const cuComplex *batch_B = B == NULL ? NULL : B + b_offset;
    cuComplex *batch_C = C + c_offset;
    if (product_required) {
      psyche_cublas_cgemm_accelerate_with_tmp(
          transa, transb, m, n, k, alpha, batch_A, lda, batch_B, ldb, beta_value, beta, batch_C, ldc, tmp, tmp_bytes);
    } else {
      psyche_cublas_cgemm_compute_with_tmp(
          transa, transb, m, n, k, alpha_value, batch_A, lda, batch_B, ldb, beta_value, batch_C, ldc, tmp);
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (batch = 0; batch < batchCount; batch++) {
    ptrdiff_t a_offset = (ptrdiff_t)batch * (ptrdiff_t)strideA;
    ptrdiff_t b_offset = (ptrdiff_t)batch * (ptrdiff_t)strideB;
    ptrdiff_t c_offset = (ptrdiff_t)batch * (ptrdiff_t)strideC;
    const cuComplex *batch_A = A == NULL ? NULL : A + a_offset;
    const cuComplex *batch_B = B == NULL ? NULL : B + b_offset;
    cuComplex *batch_C = C + c_offset;
    psyche_cublas_cgemm_compute_with_tmp(
        transa, transb, m, n, k, alpha_value, batch_A, lda, batch_B, ldb, beta_value, batch_C, ldc, tmp);
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zgemm_strided_batched_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    long long strideA,
    const cuDoubleComplex *B,
    int ldb,
    long long strideB,
    const cuDoubleComplex *beta,
    cuDoubleComplex *C,
    int ldc,
    long long strideC,
    int batchCount) {
  cublasStatus_t status = psyche_cublas_validate_gemm_strided_batched_args(
      handle, transa, transb, m, n, k, alpha, A, lda, strideA, B, ldb, strideB, beta, C, ldc, strideC, batchCount);
  cuDoubleComplex *tmp = NULL;
  cuDoubleComplex alpha_value = psyche_cublas_zzero();
  cuDoubleComplex beta_value = psyche_cublas_zzero();
  size_t tmp_bytes = 0;
  int product_required = 0;
  int batch = 0;
  if (status != CUBLAS_STATUS_SUCCESS || batchCount == 0 || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  product_required = !psyche_cublas_zis_zero(alpha_value) && k > 0;
  if (product_required && (A == NULL || B == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
#if defined(__APPLE__)
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (batch = 0; batch < batchCount; batch++) {
    ptrdiff_t a_offset = (ptrdiff_t)batch * (ptrdiff_t)strideA;
    ptrdiff_t b_offset = (ptrdiff_t)batch * (ptrdiff_t)strideB;
    ptrdiff_t c_offset = (ptrdiff_t)batch * (ptrdiff_t)strideC;
    const cuDoubleComplex *batch_A = A == NULL ? NULL : A + a_offset;
    const cuDoubleComplex *batch_B = B == NULL ? NULL : B + b_offset;
    cuDoubleComplex *batch_C = C + c_offset;
    if (product_required) {
      psyche_cublas_zgemm_accelerate_with_tmp(
          transa, transb, m, n, k, alpha, batch_A, lda, batch_B, ldb, beta_value, beta, batch_C, ldc, tmp, tmp_bytes);
    } else {
      psyche_cublas_zgemm_compute_with_tmp(
          transa, transb, m, n, k, alpha_value, batch_A, lda, batch_B, ldb, beta_value, batch_C, ldc, tmp);
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (batch = 0; batch < batchCount; batch++) {
    ptrdiff_t a_offset = (ptrdiff_t)batch * (ptrdiff_t)strideA;
    ptrdiff_t b_offset = (ptrdiff_t)batch * (ptrdiff_t)strideB;
    ptrdiff_t c_offset = (ptrdiff_t)batch * (ptrdiff_t)strideC;
    const cuDoubleComplex *batch_A = A == NULL ? NULL : A + a_offset;
    const cuDoubleComplex *batch_B = B == NULL ? NULL : B + b_offset;
    cuDoubleComplex *batch_C = C + c_offset;
    psyche_cublas_zgemm_compute_with_tmp(
        transa, transb, m, n, k, alpha_value, batch_A, lda, batch_B, ldb, beta_value, batch_C, ldc, tmp);
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_cgemm_batched_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *const Aarray[],
    int lda,
    const cuComplex *const Barray[],
    int ldb,
    const cuComplex *beta,
    cuComplex *const Carray[],
    int ldc,
    int batchCount) {
  cublasStatus_t status = psyche_cublas_validate_gemm_batched_args(
      handle, transa, transb, m, n, k, alpha, lda, ldb, beta, Carray, ldc, batchCount);
  cuComplex *tmp = NULL;
  cuComplex alpha_value = psyche_cublas_czero();
  cuComplex beta_value = psyche_cublas_czero();
  size_t tmp_bytes = 0;
  int product_required = 0;
  int batch = 0;
  if (status != CUBLAS_STATUS_SUCCESS || batchCount == 0 || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  product_required = !psyche_cublas_cis_zero(alpha_value) && k > 0;
  if (Carray == NULL || (product_required && (Aarray == NULL || Barray == NULL))) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (batch = 0; batch < batchCount; batch++) {
    if (Carray[batch] == NULL) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (product_required && (Aarray[batch] == NULL || Barray[batch] == NULL)) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
  }
#if defined(__APPLE__)
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (batch = 0; batch < batchCount; batch++) {
    const cuComplex *batch_A = product_required ? Aarray[batch] : NULL;
    const cuComplex *batch_B = product_required ? Barray[batch] : NULL;
    if (product_required) {
      psyche_cublas_cgemm_accelerate_with_tmp(
          transa, transb, m, n, k, alpha, batch_A, lda, batch_B, ldb, beta_value, beta, Carray[batch], ldc, tmp, tmp_bytes);
    } else {
      psyche_cublas_cgemm_compute_with_tmp(
          transa, transb, m, n, k, alpha_value, batch_A, lda, batch_B, ldb, beta_value, Carray[batch], ldc, tmp);
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (batch = 0; batch < batchCount; batch++) {
    const cuComplex *batch_A = product_required ? Aarray[batch] : NULL;
    const cuComplex *batch_B = product_required ? Barray[batch] : NULL;
    psyche_cublas_cgemm_compute_with_tmp(
        transa, transb, m, n, k, alpha_value, batch_A, lda, batch_B, ldb, beta_value, Carray[batch], ldc, tmp);
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zgemm_batched_impl(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *const Aarray[],
    int lda,
    const cuDoubleComplex *const Barray[],
    int ldb,
    const cuDoubleComplex *beta,
    cuDoubleComplex *const Carray[],
    int ldc,
    int batchCount) {
  cublasStatus_t status = psyche_cublas_validate_gemm_batched_args(
      handle, transa, transb, m, n, k, alpha, lda, ldb, beta, Carray, ldc, batchCount);
  cuDoubleComplex *tmp = NULL;
  cuDoubleComplex alpha_value = psyche_cublas_zzero();
  cuDoubleComplex beta_value = psyche_cublas_zzero();
  size_t tmp_bytes = 0;
  int product_required = 0;
  int batch = 0;
  if (status != CUBLAS_STATUS_SUCCESS || batchCount == 0 || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  product_required = !psyche_cublas_zis_zero(alpha_value) && k > 0;
  if (Carray == NULL || (product_required && (Aarray == NULL || Barray == NULL))) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (batch = 0; batch < batchCount; batch++) {
    if (Carray[batch] == NULL) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (product_required && (Aarray[batch] == NULL || Barray[batch] == NULL)) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
  }
#if defined(__APPLE__)
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (batch = 0; batch < batchCount; batch++) {
    const cuDoubleComplex *batch_A = product_required ? Aarray[batch] : NULL;
    const cuDoubleComplex *batch_B = product_required ? Barray[batch] : NULL;
    if (product_required) {
      psyche_cublas_zgemm_accelerate_with_tmp(
          transa, transb, m, n, k, alpha, batch_A, lda, batch_B, ldb, beta_value, beta, Carray[batch], ldc, tmp, tmp_bytes);
    } else {
      psyche_cublas_zgemm_compute_with_tmp(
          transa, transb, m, n, k, alpha_value, batch_A, lda, batch_B, ldb, beta_value, Carray[batch], ldc, tmp);
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
#endif
  status = psyche_cublas_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (batch = 0; batch < batchCount; batch++) {
    const cuDoubleComplex *batch_A = product_required ? Aarray[batch] : NULL;
    const cuDoubleComplex *batch_B = product_required ? Barray[batch] : NULL;
    psyche_cublas_zgemm_compute_with_tmp(
        transa, transb, m, n, k, alpha_value, batch_A, lda, batch_B, ldb, beta_value, Carray[batch], ldc, tmp);
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_saxpy_impl(
    cublasHandle_t handle,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    float *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (alpha == 0 || x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
#if defined(__APPLE__)
  if (psyche_cublas_metal_enabled()) {
    if (incx == 1 && incy == 1) {
      const unsigned int block_dim_x = 256;
      unsigned int grid_dim_x = 0;
      size_t bytes = 0;
      CUresult metal_result = CUDA_ERROR_INVALID_VALUE;
      status = psyche_cublas_contiguous_f32_launch_shape(n, block_dim_x, &bytes, &grid_dim_x);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      if (psyche_cublas_f32_ranges_partially_overlap(x, y, bytes)) {
        if (psyche_cublas_metal_required()) {
          return CUBLAS_STATUS_NOT_SUPPORTED;
        }
      } else {
      metal_result = psyche_cuda_metal_launch_saxpy_f32(
          x,
          y,
          *alpha,
          (unsigned int)n,
          bytes,
          grid_dim_x,
          block_dim_x);
      if (
          metal_result == CUDA_SUCCESS ||
          psyche_cublas_metal_required() ||
          !psyche_cublas_metal_preferred_can_fallback(metal_result)) {
        return psyche_cublas_status_from_cuda_result(metal_result);
      }
      }
    } else if (psyche_cublas_metal_required()) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
  }
#else
  if (psyche_cublas_metal_required()) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
#endif
  for (i = 0; i < n; i++) {
    y[(size_t)i * (size_t)incy] += (*alpha) * x[(size_t)i * (size_t)incx];
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_daxpy_impl(
    cublasHandle_t handle,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    double *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (alpha == 0 || x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    y[(size_t)i * (size_t)incy] += (*alpha) * x[(size_t)i * (size_t)incx];
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_cgemv_impl(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *x,
    int incx,
    const cuComplex *beta,
    cuComplex *y,
    int incy) {
  int input_len = 0;
  int output_len = 0;
  cublasStatus_t status = psyche_cublas_validate_gemv_args(
      handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy, &input_len, &output_len);
  cuComplex *tmp = NULL;
  cuComplex alpha_value = psyche_cublas_czero();
  cuComplex beta_value = psyche_cublas_czero();
  size_t tmp_bytes = 0;
  int out = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || output_len == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (!psyche_cublas_cis_zero(alpha_value) && input_len > 0 && (A == NULL || x == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(output_len, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (out = 0; out < output_len; out++) {
    cuComplex acc = psyche_cublas_czero();
    cuComplex product = psyche_cublas_czero();
    cuComplex existing = psyche_cublas_czero();
    if (!psyche_cublas_cis_zero(alpha_value)) {
      for (inner = 0; inner < input_len; inner++) {
        acc = psyche_cublas_cadd(
            acc,
            psyche_cublas_cmul(
                psyche_cublas_cgemv_a(A, trans, out, inner, lda),
                x[psyche_cublas_signed_stride_index(input_len, incx, inner)]));
      }
      product = psyche_cublas_cmul(alpha_value, acc);
    }
    if (!psyche_cublas_cis_zero(beta_value)) {
      existing = psyche_cublas_cmul(beta_value, y[psyche_cublas_signed_stride_index(output_len, incy, out)]);
    }
    tmp[out] = psyche_cublas_cadd(product, existing);
  }
  for (out = 0; out < output_len; out++) {
    y[psyche_cublas_signed_stride_index(output_len, incy, out)] = tmp[out];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zgemv_impl(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *beta,
    cuDoubleComplex *y,
    int incy) {
  int input_len = 0;
  int output_len = 0;
  cublasStatus_t status = psyche_cublas_validate_gemv_args(
      handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy, &input_len, &output_len);
  cuDoubleComplex *tmp = NULL;
  cuDoubleComplex alpha_value = psyche_cublas_zzero();
  cuDoubleComplex beta_value = psyche_cublas_zzero();
  size_t tmp_bytes = 0;
  int out = 0;
  int inner = 0;
  if (status != CUBLAS_STATUS_SUCCESS || output_len == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (!psyche_cublas_zis_zero(alpha_value) && input_len > 0 && (A == NULL || x == NULL)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(output_len, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (out = 0; out < output_len; out++) {
    cuDoubleComplex acc = psyche_cublas_zzero();
    cuDoubleComplex product = psyche_cublas_zzero();
    cuDoubleComplex existing = psyche_cublas_zzero();
    if (!psyche_cublas_zis_zero(alpha_value)) {
      for (inner = 0; inner < input_len; inner++) {
        acc = psyche_cublas_zadd(
            acc,
            psyche_cublas_zmul(
                psyche_cublas_zgemv_a(A, trans, out, inner, lda),
                x[psyche_cublas_signed_stride_index(input_len, incx, inner)]));
      }
      product = psyche_cublas_zmul(alpha_value, acc);
    }
    if (!psyche_cublas_zis_zero(beta_value)) {
      existing = psyche_cublas_zmul(beta_value, y[psyche_cublas_signed_stride_index(output_len, incy, out)]);
    }
    tmp[out] = psyche_cublas_zadd(product, existing);
  }
  for (out = 0; out < output_len; out++) {
    y[psyche_cublas_signed_stride_index(output_len, incy, out)] = tmp[out];
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_cger_impl(
    cublasHandle_t handle,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *A,
    int lda,
    int conjugate_y) {
  cublasStatus_t status = psyche_cublas_validate_ger_args(handle, m, n, alpha, incx, incy, lda);
  cuComplex alpha_value = psyche_cublas_czero();
  cuComplex *x_tmp = NULL;
  cuComplex *y_tmp = NULL;
  size_t x_bytes = 0;
  size_t y_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublas_cis_zero(alpha_value)) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(m, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*y_tmp), &y_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (cuComplex *)malloc(x_bytes);
  y_tmp = (cuComplex *)malloc(y_bytes);
  if (x_tmp == NULL || y_tmp == NULL) {
    free(x_tmp);
    free(y_tmp);
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < m; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(m, incx, row)];
  }
  for (col = 0; col < n; col++) {
    cuComplex value = y[psyche_cublas_signed_stride_index(n, incy, col)];
    y_tmp[col] = conjugate_y ? psyche_cublas_cconj(value) : value;
  }
  for (col = 0; col < n; col++) {
    cuComplex scaled_y = psyche_cublas_cmul(alpha_value, y_tmp[col]);
    for (row = 0; row < m; row++) {
      size_t index = (size_t)row + (size_t)col * (size_t)lda;
      A[index] = psyche_cublas_cadd(A[index], psyche_cublas_cmul(x_tmp[row], scaled_y));
    }
  }
  free(x_tmp);
  free(y_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zger_impl(
    cublasHandle_t handle,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *A,
    int lda,
    int conjugate_y) {
  cublasStatus_t status = psyche_cublas_validate_ger_args(handle, m, n, alpha, incx, incy, lda);
  cuDoubleComplex alpha_value = psyche_cublas_zzero();
  cuDoubleComplex *x_tmp = NULL;
  cuDoubleComplex *y_tmp = NULL;
  size_t x_bytes = 0;
  size_t y_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || m == 0 || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (A == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublas_zis_zero(alpha_value)) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(m, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*y_tmp), &y_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (cuDoubleComplex *)malloc(x_bytes);
  y_tmp = (cuDoubleComplex *)malloc(y_bytes);
  if (x_tmp == NULL || y_tmp == NULL) {
    free(x_tmp);
    free(y_tmp);
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < m; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(m, incx, row)];
  }
  for (col = 0; col < n; col++) {
    cuDoubleComplex value = y[psyche_cublas_signed_stride_index(n, incy, col)];
    y_tmp[col] = conjugate_y ? psyche_cublas_zconj(value) : value;
  }
  for (col = 0; col < n; col++) {
    cuDoubleComplex scaled_y = psyche_cublas_zmul(alpha_value, y_tmp[col]);
    for (row = 0; row < m; row++) {
      size_t index = (size_t)row + (size_t)col * (size_t)lda;
      A[index] = psyche_cublas_zadd(A[index], psyche_cublas_zmul(x_tmp[row], scaled_y));
    }
  }
  free(x_tmp);
  free(y_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_chemv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *x,
    int incx,
    const cuComplex *beta,
    cuComplex *y,
    int incy) {
  cublasStatus_t status =
      psyche_cublas_validate_symv_args(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
  cuComplex alpha_value = psyche_cublas_czero();
  cuComplex beta_value = psyche_cublas_czero();
  cuComplex *tmp = NULL;
  cuComplex *x_tmp = NULL;
  size_t tmp_bytes = 0;
  size_t x_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (!psyche_cublas_cis_zero(alpha_value)) {
    if (A == NULL || x == NULL) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    status = psyche_cublas_temp_bytes(n, 1, sizeof(*x_tmp), &x_bytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuComplex *)malloc(tmp_bytes);
  if (!psyche_cublas_cis_zero(alpha_value)) {
    x_tmp = (cuComplex *)malloc(x_bytes);
  }
  if (tmp == NULL || (!psyche_cublas_cis_zero(alpha_value) && x_tmp == NULL)) {
    free(tmp);
    free(x_tmp);
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  if (!psyche_cublas_cis_zero(alpha_value)) {
    for (row = 0; row < n; row++) {
      x_tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
    }
  }
  for (row = 0; row < n; row++) {
    cuComplex acc = psyche_cublas_czero();
    cuComplex product = psyche_cublas_czero();
    cuComplex existing = psyche_cublas_czero();
    if (!psyche_cublas_cis_zero(alpha_value)) {
      for (col = 0; col < n; col++) {
        acc = psyche_cublas_cadd(
            acc,
            psyche_cublas_cmul(
                psyche_cublas_chermitian_value(A, uplo, row, col, lda),
                x_tmp[col]));
      }
      product = psyche_cublas_cmul(alpha_value, acc);
    }
    if (!psyche_cublas_cis_zero(beta_value)) {
      existing = psyche_cublas_cmul(beta_value, y[psyche_cublas_signed_stride_index(n, incy, row)]);
    }
    tmp[row] = psyche_cublas_cadd(product, existing);
  }
  for (row = 0; row < n; row++) {
    y[psyche_cublas_signed_stride_index(n, incy, row)] = tmp[row];
  }
  free(tmp);
  free(x_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zhemv_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *beta,
    cuDoubleComplex *y,
    int incy) {
  cublasStatus_t status =
      psyche_cublas_validate_symv_args(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
  cuDoubleComplex alpha_value = psyche_cublas_zzero();
  cuDoubleComplex beta_value = psyche_cublas_zzero();
  cuDoubleComplex *tmp = NULL;
  cuDoubleComplex *x_tmp = NULL;
  size_t tmp_bytes = 0;
  size_t x_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  beta_value = *beta;
  if (!psyche_cublas_zis_zero(alpha_value)) {
    if (A == NULL || x == NULL) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    status = psyche_cublas_temp_bytes(n, 1, sizeof(*x_tmp), &x_bytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (cuDoubleComplex *)malloc(tmp_bytes);
  if (!psyche_cublas_zis_zero(alpha_value)) {
    x_tmp = (cuDoubleComplex *)malloc(x_bytes);
  }
  if (tmp == NULL || (!psyche_cublas_zis_zero(alpha_value) && x_tmp == NULL)) {
    free(tmp);
    free(x_tmp);
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  if (!psyche_cublas_zis_zero(alpha_value)) {
    for (row = 0; row < n; row++) {
      x_tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
    }
  }
  for (row = 0; row < n; row++) {
    cuDoubleComplex acc = psyche_cublas_zzero();
    cuDoubleComplex product = psyche_cublas_zzero();
    cuDoubleComplex existing = psyche_cublas_zzero();
    if (!psyche_cublas_zis_zero(alpha_value)) {
      for (col = 0; col < n; col++) {
        acc = psyche_cublas_zadd(
            acc,
            psyche_cublas_zmul(
                psyche_cublas_zhermitian_value(A, uplo, row, col, lda),
                x_tmp[col]));
      }
      product = psyche_cublas_zmul(alpha_value, acc);
    }
    if (!psyche_cublas_zis_zero(beta_value)) {
      existing = psyche_cublas_zmul(beta_value, y[psyche_cublas_signed_stride_index(n, incy, row)]);
    }
    tmp[row] = psyche_cublas_zadd(product, existing);
  }
  for (row = 0; row < n; row++) {
    y[psyche_cublas_signed_stride_index(n, incy, row)] = tmp[row];
  }
  free(tmp);
  free(x_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_cher_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const cuComplex *x,
    int incx,
    cuComplex *A,
    int lda) {
  cublasStatus_t status = psyche_cublas_validate_syr_args(handle, uplo, n, alpha, incx, A, lda);
  float alpha_value = 0.0f;
  cuComplex *x_tmp = NULL;
  size_t x_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (alpha_value == 0.0f) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (cuComplex *)malloc(x_bytes);
  if (x_tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t index = (size_t)row + (size_t)col * (size_t)lda;
      cuComplex term;
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        continue;
      }
      if (row == col) {
        A[index].x += alpha_value * psyche_cublas_cabs_squared(x_tmp[row]);
        A[index].y = 0.0f;
        continue;
      }
      term = psyche_cublas_cmul(x_tmp[row], psyche_cublas_cconj(x_tmp[col]));
      term.x *= alpha_value;
      term.y *= alpha_value;
      A[index] = psyche_cublas_cadd(A[index], term);
    }
  }
  free(x_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zher_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *A,
    int lda) {
  cublasStatus_t status = psyche_cublas_validate_syr_args(handle, uplo, n, alpha, incx, A, lda);
  double alpha_value = 0.0;
  cuDoubleComplex *x_tmp = NULL;
  size_t x_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (alpha_value == 0.0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (cuDoubleComplex *)malloc(x_bytes);
  if (x_tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t index = (size_t)row + (size_t)col * (size_t)lda;
      cuDoubleComplex term;
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        continue;
      }
      if (row == col) {
        A[index].x += alpha_value * psyche_cublas_zabs_squared(x_tmp[row]);
        A[index].y = 0.0;
        continue;
      }
      term = psyche_cublas_zmul(x_tmp[row], psyche_cublas_zconj(x_tmp[col]));
      term.x *= alpha_value;
      term.y *= alpha_value;
      A[index] = psyche_cublas_zadd(A[index], term);
    }
  }
  free(x_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_cher2_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *A,
    int lda) {
  cublasStatus_t status = psyche_cublas_validate_syr2_args(handle, uplo, n, alpha, incx, incy, A, lda);
  cuComplex alpha_value = psyche_cublas_czero();
  cuComplex alpha_conj = psyche_cublas_czero();
  cuComplex *x_tmp = NULL;
  cuComplex *y_tmp = NULL;
  size_t x_bytes = 0;
  size_t y_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (psyche_cublas_cis_zero(alpha_value)) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  alpha_conj = psyche_cublas_cconj(alpha_value);
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*y_tmp), &y_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (cuComplex *)malloc(x_bytes);
  y_tmp = (cuComplex *)malloc(y_bytes);
  if (x_tmp == NULL || y_tmp == NULL) {
    free(x_tmp);
    free(y_tmp);
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
    y_tmp[row] = y[psyche_cublas_signed_stride_index(n, incy, row)];
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t index = (size_t)row + (size_t)col * (size_t)lda;
      cuComplex first;
      cuComplex second;
      cuComplex term;
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        continue;
      }
      first = psyche_cublas_cmul(
          alpha_value,
          psyche_cublas_cmul(x_tmp[row], psyche_cublas_cconj(y_tmp[col])));
      second = psyche_cublas_cmul(
          alpha_conj,
          psyche_cublas_cmul(y_tmp[row], psyche_cublas_cconj(x_tmp[col])));
      term = psyche_cublas_cadd(first, second);
      A[index] = psyche_cublas_cadd(A[index], term);
      if (row == col) {
        A[index].y = 0.0f;
      }
    }
  }
  free(x_tmp);
  free(y_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zher2_impl(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *A,
    int lda) {
  cublasStatus_t status = psyche_cublas_validate_syr2_args(handle, uplo, n, alpha, incx, incy, A, lda);
  cuDoubleComplex alpha_value = psyche_cublas_zzero();
  cuDoubleComplex alpha_conj = psyche_cublas_zzero();
  cuDoubleComplex *x_tmp = NULL;
  cuDoubleComplex *y_tmp = NULL;
  size_t x_bytes = 0;
  size_t y_bytes = 0;
  int row = 0;
  int col = 0;
  if (status != CUBLAS_STATUS_SUCCESS || n == 0) {
    return status;
  }
  alpha_value = *alpha;
  if (psyche_cublas_zis_zero(alpha_value)) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  alpha_conj = psyche_cublas_zconj(alpha_value);
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*x_tmp), &x_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_temp_bytes(n, 1, sizeof(*y_tmp), &y_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  x_tmp = (cuDoubleComplex *)malloc(x_bytes);
  y_tmp = (cuDoubleComplex *)malloc(y_bytes);
  if (x_tmp == NULL || y_tmp == NULL) {
    free(x_tmp);
    free(y_tmp);
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < n; row++) {
    x_tmp[row] = x[psyche_cublas_signed_stride_index(n, incx, row)];
    y_tmp[row] = y[psyche_cublas_signed_stride_index(n, incy, row)];
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < n; row++) {
      size_t index = (size_t)row + (size_t)col * (size_t)lda;
      cuDoubleComplex first;
      cuDoubleComplex second;
      cuDoubleComplex term;
      if (!psyche_cublas_symmetric_element_is_stored(uplo, row, col)) {
        continue;
      }
      first = psyche_cublas_zmul(
          alpha_value,
          psyche_cublas_zmul(x_tmp[row], psyche_cublas_zconj(y_tmp[col])));
      second = psyche_cublas_zmul(
          alpha_conj,
          psyche_cublas_zmul(y_tmp[row], psyche_cublas_zconj(x_tmp[col])));
      term = psyche_cublas_zadd(first, second);
      A[index] = psyche_cublas_zadd(A[index], term);
      if (row == col) {
        A[index].y = 0.0;
      }
    }
  }
  free(x_tmp);
  free(y_tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_caxpy_impl(
    cublasHandle_t handle,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    cuComplex *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  cuComplex alpha_value;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (alpha == 0 || x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  alpha_value = *alpha;
  for (i = 0; i < n; i++) {
    cuComplex product = psyche_cublas_cmul(alpha_value, x[(size_t)i * (size_t)incx]);
    y[(size_t)i * (size_t)incy] = psyche_cublas_cadd(product, y[(size_t)i * (size_t)incy]);
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zaxpy_impl(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  cuDoubleComplex alpha_value;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (alpha == 0 || x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  alpha_value = *alpha;
  for (i = 0; i < n; i++) {
    cuDoubleComplex product = psyche_cublas_zmul(alpha_value, x[(size_t)i * (size_t)incx]);
    y[(size_t)i * (size_t)incy] = psyche_cublas_zadd(product, y[(size_t)i * (size_t)incy]);
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_scopy_impl(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    float *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 0);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
#if defined(__APPLE__)
  if (psyche_cublas_metal_enabled()) {
    if (incx == 1 && incy == 1) {
      const unsigned int block_dim_x = 256;
      unsigned int grid_dim_x = 0;
      size_t bytes = 0;
      CUresult metal_result = CUDA_ERROR_INVALID_VALUE;
      status = psyche_cublas_contiguous_f32_launch_shape(n, block_dim_x, &bytes, &grid_dim_x);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      if (psyche_cublas_f32_ranges_partially_overlap(x, y, bytes)) {
        if (psyche_cublas_metal_required()) {
          return CUBLAS_STATUS_NOT_SUPPORTED;
        }
      } else {
        metal_result = psyche_cuda_metal_launch_copy_f32(
            x,
            y,
            (unsigned int)n,
            bytes,
            grid_dim_x,
            block_dim_x);
        if (
            metal_result == CUDA_SUCCESS ||
            psyche_cublas_metal_required() ||
            !psyche_cublas_metal_preferred_can_fallback(metal_result)) {
          return psyche_cublas_status_from_cuda_result(metal_result);
        }
      }
    } else if (psyche_cublas_metal_required()) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
  }
#else
  if (psyche_cublas_metal_required()) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
#endif
  for (i = 0; i < n; i++) {
    y[(size_t)i * (size_t)incy] = x[(size_t)i * (size_t)incx];
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dcopy_impl(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    double *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 0);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    y[(size_t)i * (size_t)incy] = x[(size_t)i * (size_t)incx];
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ccopy_impl(
    cublasHandle_t handle,
    int n,
    const cuComplex *x,
    int incx,
    cuComplex *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 0);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    y[(size_t)i * (size_t)incy] = x[(size_t)i * (size_t)incx];
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zcopy_impl(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 0);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    y[(size_t)i * (size_t)incy] = x[(size_t)i * (size_t)incx];
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_sdot_impl(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    const float *y,
    int incy,
    float *result) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  float acc = 0.0f;
  CUresult metal_result = CUDA_ERROR_INVALID_VALUE;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (result == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (n <= 0) {
    *result = 0.0f;
    return CUBLAS_STATUS_SUCCESS;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
#if defined(__APPLE__)
  if (psyche_cublas_metal_enabled()) {
    if (incx == 1 && incy == 1) {
      const unsigned int block_dim_x = 256;
      unsigned int grid_dim_x = 0;
      size_t bytes = 0;
      status = psyche_cublas_contiguous_f32_launch_shape(n, block_dim_x, &bytes, &grid_dim_x);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      metal_result = psyche_cuda_metal_launch_dot_f32(
          x,
          y,
          result,
          (unsigned int)n,
          bytes,
          grid_dim_x,
          block_dim_x);
      if (
          metal_result == CUDA_SUCCESS ||
          psyche_cublas_metal_required() ||
          !psyche_cublas_metal_preferred_can_fallback(metal_result)) {
        return psyche_cublas_status_from_cuda_result(metal_result);
      }
    } else if (psyche_cublas_metal_required()) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
  }
#else
  if (psyche_cublas_metal_required()) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
#endif
  for (i = 0; i < n; i++) {
    acc += x[(size_t)i * (size_t)incx] * y[(size_t)i * (size_t)incy];
  }
  *result = acc;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_ddot_impl(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    const double *y,
    int incy,
    double *result) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  double acc = 0.0;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (result == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (n <= 0) {
    *result = 0.0;
    return CUBLAS_STATUS_SUCCESS;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    acc += x[(size_t)i * (size_t)incx] * y[(size_t)i * (size_t)incy];
  }
  *result = acc;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_cdot_impl(
    cublasHandle_t handle,
    int n,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *result,
    int conjugate_x) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  cuComplex acc;
  int i = 0;
  acc.x = 0.0f;
  acc.y = 0.0f;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (result == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (n <= 0) {
    *result = acc;
    return CUBLAS_STATUS_SUCCESS;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    cuComplex left = x[(size_t)i * (size_t)incx];
    if (conjugate_x) {
      left = psyche_cublas_cconj(left);
    }
    acc = psyche_cublas_cadd(acc, psyche_cublas_cmul(left, y[(size_t)i * (size_t)incy]));
  }
  *result = acc;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zdot_impl(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *result,
    int conjugate_x) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  cuDoubleComplex acc;
  int i = 0;
  acc.x = 0.0;
  acc.y = 0.0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (result == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (n <= 0) {
    *result = acc;
    return CUBLAS_STATUS_SUCCESS;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    cuDoubleComplex left = x[(size_t)i * (size_t)incx];
    if (conjugate_x) {
      left = psyche_cublas_zconj(left);
    }
    acc = psyche_cublas_zadd(acc, psyche_cublas_zmul(left, y[(size_t)i * (size_t)incy]));
  }
  *result = acc;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_sscal_impl(
    cublasHandle_t handle,
    int n,
    const float *alpha,
    float *x,
    int incx) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (alpha == 0 || x == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
#if defined(__APPLE__)
  if (psyche_cublas_metal_enabled()) {
    if (incx == 1) {
      const unsigned int block_dim_x = 256;
      unsigned int grid_dim_x = 0;
      size_t bytes = 0;
      CUresult metal_result = CUDA_ERROR_INVALID_VALUE;
      status = psyche_cublas_contiguous_f32_launch_shape(n, block_dim_x, &bytes, &grid_dim_x);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      metal_result = psyche_cuda_metal_launch_scale_f32(
          x,
          *alpha,
          (unsigned int)n,
          bytes,
          grid_dim_x,
          block_dim_x);
      if (
          metal_result == CUDA_SUCCESS ||
          psyche_cublas_metal_required() ||
          !psyche_cublas_metal_preferred_can_fallback(metal_result)) {
        return psyche_cublas_status_from_cuda_result(metal_result);
      }
    } else if (psyche_cublas_metal_required()) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
  }
#else
  if (psyche_cublas_metal_required()) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
#endif
  for (i = 0; i < n; i++) {
    x[(size_t)i * (size_t)incx] *= *alpha;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dscal_impl(
    cublasHandle_t handle,
    int n,
    const double *alpha,
    double *x,
    int incx) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (alpha == 0 || x == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    x[(size_t)i * (size_t)incx] *= *alpha;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_cscal_impl(
    cublasHandle_t handle,
    int n,
    const cuComplex *alpha,
    cuComplex *x,
    int incx) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  cuComplex alpha_value;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (alpha == 0 || x == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  alpha_value = *alpha;
  for (i = 0; i < n; i++) {
    x[(size_t)i * (size_t)incx] = psyche_cublas_cmul(alpha_value, x[(size_t)i * (size_t)incx]);
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_csscal_impl(
    cublasHandle_t handle,
    int n,
    const float *alpha,
    cuComplex *x,
    int incx) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  float alpha_value = 0.0f;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (alpha == 0 || x == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  alpha_value = *alpha;
  for (i = 0; i < n; i++) {
    x[(size_t)i * (size_t)incx].x *= alpha_value;
    x[(size_t)i * (size_t)incx].y *= alpha_value;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zscal_impl(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *alpha,
    cuDoubleComplex *x,
    int incx) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  cuDoubleComplex alpha_value;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (alpha == 0 || x == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  alpha_value = *alpha;
  for (i = 0; i < n; i++) {
    x[(size_t)i * (size_t)incx] = psyche_cublas_zmul(alpha_value, x[(size_t)i * (size_t)incx]);
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zdscal_impl(
    cublasHandle_t handle,
    int n,
    const double *alpha,
    cuDoubleComplex *x,
    int incx) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  double alpha_value = 0.0;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (alpha == 0 || x == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  alpha_value = *alpha;
  for (i = 0; i < n; i++) {
    x[(size_t)i * (size_t)incx].x *= alpha_value;
    x[(size_t)i * (size_t)incx].y *= alpha_value;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_srot_impl(
    cublasHandle_t handle,
    int n,
    float *x,
    int incx,
    float *y,
    int incy,
    const float *c,
    const float *s) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  float *tmp = NULL;
  size_t tmp_bytes = 0;
  float c_value = 0.0f;
  float s_value = 0.0f;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL || c == NULL || s == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 2, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  c_value = *c;
  s_value = *s;
  for (i = 0; i < n; i++) {
    tmp[(size_t)i] = x[(size_t)i * (size_t)incx];
    tmp[(size_t)n + (size_t)i] = y[(size_t)i * (size_t)incy];
  }
  for (i = 0; i < n; i++) {
    float x_old = tmp[(size_t)i];
    float y_old = tmp[(size_t)n + (size_t)i];
    x[(size_t)i * (size_t)incx] = c_value * x_old + s_value * y_old;
    y[(size_t)i * (size_t)incy] = c_value * y_old - s_value * x_old;
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_drot_impl(
    cublasHandle_t handle,
    int n,
    double *x,
    int incx,
    double *y,
    int incy,
    const double *c,
    const double *s) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  double *tmp = NULL;
  size_t tmp_bytes = 0;
  double c_value = 0.0;
  double s_value = 0.0;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL || c == NULL || s == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublas_temp_bytes(n, 2, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  c_value = *c;
  s_value = *s;
  for (i = 0; i < n; i++) {
    tmp[(size_t)i] = x[(size_t)i * (size_t)incx];
    tmp[(size_t)n + (size_t)i] = y[(size_t)i * (size_t)incy];
  }
  for (i = 0; i < n; i++) {
    double x_old = tmp[(size_t)i];
    double y_old = tmp[(size_t)n + (size_t)i];
    x[(size_t)i * (size_t)incx] = c_value * x_old + s_value * y_old;
    y[(size_t)i * (size_t)incy] = c_value * y_old - s_value * x_old;
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_srotg_impl(
    cublasHandle_t handle,
    float *a,
    float *b,
    float *c,
    float *s) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  float a_value = 0.0f;
  float b_value = 0.0f;
  float abs_a = 0.0f;
  float abs_b = 0.0f;
  float roe = 0.0f;
  float scale = 0.0f;
  float r = 0.0f;
  float z = 0.0f;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (a == NULL || b == NULL || c == NULL || s == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  a_value = *a;
  b_value = *b;
  abs_a = fabsf(a_value);
  abs_b = fabsf(b_value);
  roe = abs_a > abs_b ? a_value : b_value;
  scale = abs_a + abs_b;
  if (scale == 0.0f) {
    *c = 1.0f;
    *s = 0.0f;
    *a = 0.0f;
    *b = 0.0f;
    return CUBLAS_STATUS_SUCCESS;
  }
  r = scale * sqrtf((a_value / scale) * (a_value / scale) + (b_value / scale) * (b_value / scale));
  if (roe < 0.0f) {
    r = -r;
  }
  *c = a_value / r;
  *s = b_value / r;
  z = 1.0f;
  if (abs_a > abs_b) {
    z = *s;
  } else if (*c != 0.0f) {
    z = 1.0f / *c;
  }
  *a = r;
  *b = z;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_drotg_impl(
    cublasHandle_t handle,
    double *a,
    double *b,
    double *c,
    double *s) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  double a_value = 0.0;
  double b_value = 0.0;
  double abs_a = 0.0;
  double abs_b = 0.0;
  double roe = 0.0;
  double scale = 0.0;
  double r = 0.0;
  double z = 0.0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (a == NULL || b == NULL || c == NULL || s == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  a_value = *a;
  b_value = *b;
  abs_a = fabs(a_value);
  abs_b = fabs(b_value);
  roe = abs_a > abs_b ? a_value : b_value;
  scale = abs_a + abs_b;
  if (scale == 0.0) {
    *c = 1.0;
    *s = 0.0;
    *a = 0.0;
    *b = 0.0;
    return CUBLAS_STATUS_SUCCESS;
  }
  r = scale * sqrt((a_value / scale) * (a_value / scale) + (b_value / scale) * (b_value / scale));
  if (roe < 0.0) {
    r = -r;
  }
  *c = a_value / r;
  *s = b_value / r;
  z = 1.0;
  if (abs_a > abs_b) {
    z = *s;
  } else if (*c != 0.0) {
    z = 1.0 / *c;
  }
  *a = r;
  *b = z;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_srotmg_impl(
    cublasHandle_t handle,
    float *d1,
    float *d2,
    float *x1,
    const float *y1,
    float *param) {
  /* ROTMG reads and writes CPU scalars, so device pointer mode is outside this shim contract. */
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  const float zero = 0.0f;
  const float one = 1.0f;
  const float two = 2.0f;
  const float gam = 4096.0f;
  const float gamsq = 16777216.0f;
  const float rgamsq = 5.960464477539063e-8f;
  float d1_value = 0.0f;
  float d2_value = 0.0f;
  float x1_value = 0.0f;
  float y1_value = 0.0f;
  float flag = 0.0f;
  float h11 = 0.0f;
  float h12 = 0.0f;
  float h21 = 0.0f;
  float h22 = 0.0f;
  float p1 = 0.0f;
  float p2 = 0.0f;
  float q1 = 0.0f;
  float q2 = 0.0f;
  float temp = 0.0f;
  float u = 0.0f;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (d1 == NULL || d2 == NULL || x1 == NULL || y1 == NULL || param == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  d1_value = *d1;
  d2_value = *d2;
  x1_value = *x1;
  y1_value = *y1;
  /* Netlib ROTMG branch map: negative d1 resets, p2==0 emits identity, otherwise q1/q2 choose flag 0 or 1 before scaling. */
  if (d1_value < zero) {
    flag = -one;
    d1_value = zero;
    d2_value = zero;
    x1_value = zero;
  } else {
    p2 = d2_value * y1_value;
    if (p2 == zero) {
      param[0] = -two;
      return CUBLAS_STATUS_SUCCESS;
    }
    p1 = d1_value * x1_value;
    q2 = p2 * y1_value;
    q1 = p1 * x1_value;
    if (fabsf(q1) > fabsf(q2)) {
      h21 = -y1_value / x1_value;
      h12 = p2 / p1;
      u = one - h12 * h21;
      if (u > zero) {
        flag = zero;
        d1_value = d1_value / u;
        d2_value = d2_value / u;
        x1_value = x1_value * u;
      } else {
        flag = -one;
        h11 = zero;
        h12 = zero;
        h21 = zero;
        h22 = zero;
        d1_value = zero;
        d2_value = zero;
        x1_value = zero;
      }
    } else if (q2 < zero) {
      flag = -one;
      h11 = zero;
      h12 = zero;
      h21 = zero;
      h22 = zero;
      d1_value = zero;
      d2_value = zero;
      x1_value = zero;
    } else {
      flag = one;
      h11 = p1 / p2;
      h22 = x1_value / y1_value;
      u = one + h11 * h22;
      temp = d2_value / u;
      d2_value = d1_value / u;
      d1_value = temp;
      x1_value = y1_value * u;
    }
    if (d1_value != zero && isfinite(d1_value)) {
      while (d1_value <= rgamsq || d1_value >= gamsq) {
        if (flag == zero) {
          h11 = one;
          h22 = one;
          flag = -one;
        } else {
          h21 = -one;
          h12 = one;
          flag = -one;
        }
        if (d1_value <= rgamsq) {
          d1_value = d1_value * gam * gam;
          x1_value = x1_value / gam;
          h11 = h11 / gam;
          h12 = h12 / gam;
        } else {
          d1_value = d1_value / (gam * gam);
          x1_value = x1_value * gam;
          h11 = h11 * gam;
          h12 = h12 * gam;
        }
      }
    }
    if (d2_value != zero && isfinite(d2_value)) {
      while (fabsf(d2_value) <= rgamsq || fabsf(d2_value) >= gamsq) {
        if (flag == zero) {
          h11 = one;
          h22 = one;
          flag = -one;
        } else {
          h21 = -one;
          h12 = one;
          flag = -one;
        }
        if (fabsf(d2_value) <= rgamsq) {
          d2_value = d2_value * gam * gam;
          h21 = h21 / gam;
          h22 = h22 / gam;
        } else {
          d2_value = d2_value / (gam * gam);
          h21 = h21 * gam;
          h22 = h22 * gam;
        }
      }
    }
  }
  *d1 = d1_value;
  *d2 = d2_value;
  *x1 = x1_value;
  if (flag < zero) {
    param[1] = h11;
    param[2] = h21;
    param[3] = h12;
    param[4] = h22;
  } else if (flag == zero) {
    param[2] = h21;
    param[3] = h12;
  } else {
    param[1] = h11;
    param[4] = h22;
  }
  param[0] = flag;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_drotmg_impl(
    cublasHandle_t handle,
    double *d1,
    double *d2,
    double *x1,
    const double *y1,
    double *param) {
  /* ROTMG reads and writes CPU scalars, so device pointer mode is outside this shim contract. */
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  const double zero = 0.0;
  const double one = 1.0;
  const double two = 2.0;
  const double gam = 4096.0;
  const double gamsq = 16777216.0;
  const double rgamsq = 5.9604644775390625e-8;
  double d1_value = 0.0;
  double d2_value = 0.0;
  double x1_value = 0.0;
  double y1_value = 0.0;
  double flag = 0.0;
  double h11 = 0.0;
  double h12 = 0.0;
  double h21 = 0.0;
  double h22 = 0.0;
  double p1 = 0.0;
  double p2 = 0.0;
  double q1 = 0.0;
  double q2 = 0.0;
  double temp = 0.0;
  double u = 0.0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (d1 == NULL || d2 == NULL || x1 == NULL || y1 == NULL || param == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  d1_value = *d1;
  d2_value = *d2;
  x1_value = *x1;
  y1_value = *y1;
  /* Netlib ROTMG branch map: negative d1 resets, p2==0 emits identity, otherwise q1/q2 choose flag 0 or 1 before scaling. */
  if (d1_value < zero) {
    flag = -one;
    d1_value = zero;
    d2_value = zero;
    x1_value = zero;
  } else {
    p2 = d2_value * y1_value;
    if (p2 == zero) {
      param[0] = -two;
      return CUBLAS_STATUS_SUCCESS;
    }
    p1 = d1_value * x1_value;
    q2 = p2 * y1_value;
    q1 = p1 * x1_value;
    if (fabs(q1) > fabs(q2)) {
      h21 = -y1_value / x1_value;
      h12 = p2 / p1;
      u = one - h12 * h21;
      if (u > zero) {
        flag = zero;
        d1_value = d1_value / u;
        d2_value = d2_value / u;
        x1_value = x1_value * u;
      } else {
        flag = -one;
        h11 = zero;
        h12 = zero;
        h21 = zero;
        h22 = zero;
        d1_value = zero;
        d2_value = zero;
        x1_value = zero;
      }
    } else if (q2 < zero) {
      flag = -one;
      h11 = zero;
      h12 = zero;
      h21 = zero;
      h22 = zero;
      d1_value = zero;
      d2_value = zero;
      x1_value = zero;
    } else {
      flag = one;
      h11 = p1 / p2;
      h22 = x1_value / y1_value;
      u = one + h11 * h22;
      temp = d2_value / u;
      d2_value = d1_value / u;
      d1_value = temp;
      x1_value = y1_value * u;
    }
    if (d1_value != zero && isfinite(d1_value)) {
      while (d1_value <= rgamsq || d1_value >= gamsq) {
        if (flag == zero) {
          h11 = one;
          h22 = one;
          flag = -one;
        } else {
          h21 = -one;
          h12 = one;
          flag = -one;
        }
        if (d1_value <= rgamsq) {
          d1_value = d1_value * gam * gam;
          x1_value = x1_value / gam;
          h11 = h11 / gam;
          h12 = h12 / gam;
        } else {
          d1_value = d1_value / (gam * gam);
          x1_value = x1_value * gam;
          h11 = h11 * gam;
          h12 = h12 * gam;
        }
      }
    }
    if (d2_value != zero && isfinite(d2_value)) {
      while (fabs(d2_value) <= rgamsq || fabs(d2_value) >= gamsq) {
        if (flag == zero) {
          h11 = one;
          h22 = one;
          flag = -one;
        } else {
          h21 = -one;
          h12 = one;
          flag = -one;
        }
        if (fabs(d2_value) <= rgamsq) {
          d2_value = d2_value * gam * gam;
          h21 = h21 / gam;
          h22 = h22 / gam;
        } else {
          d2_value = d2_value / (gam * gam);
          h21 = h21 * gam;
          h22 = h22 * gam;
        }
      }
    }
  }
  *d1 = d1_value;
  *d2 = d2_value;
  *x1 = x1_value;
  if (flag < zero) {
    param[1] = h11;
    param[2] = h21;
    param[3] = h12;
    param[4] = h22;
  } else if (flag == zero) {
    param[2] = h21;
    param[3] = h12;
  } else {
    param[1] = h11;
    param[4] = h22;
  }
  param[0] = flag;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_srotm_impl(
    cublasHandle_t handle,
    int n,
    float *x,
    int incx,
    float *y,
    int incy,
    const float *param) {
  /* ROTM reads param on CPU, so device pointer mode is outside this shim contract. */
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  float *tmp = NULL;
  size_t tmp_bytes = 0;
  float flag = 0.0f;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL || param == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  flag = param[0];
  if (flag != -2.0f && flag != -1.0f && flag != 0.0f && flag != 1.0f) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (flag == -2.0f) {
    return CUBLAS_STATUS_SUCCESS;
  }
  status = psyche_cublas_temp_bytes(n, 2, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (i = 0; i < n; i++) {
    tmp[(size_t)i] = x[(size_t)i * (size_t)incx];
    tmp[(size_t)n + (size_t)i] = y[(size_t)i * (size_t)incy];
  }
  if (flag == -1.0f) {
    float h11 = param[1];
    float h21 = param[2];
    float h12 = param[3];
    float h22 = param[4];
    for (i = 0; i < n; i++) {
      float x_old = tmp[(size_t)i];
      float y_old = tmp[(size_t)n + (size_t)i];
      x[(size_t)i * (size_t)incx] = h11 * x_old + h12 * y_old;
      y[(size_t)i * (size_t)incy] = h21 * x_old + h22 * y_old;
    }
  } else if (flag == 0.0f) {
    float h21 = param[2];
    float h12 = param[3];
    for (i = 0; i < n; i++) {
      float x_old = tmp[(size_t)i];
      float y_old = tmp[(size_t)n + (size_t)i];
      x[(size_t)i * (size_t)incx] = x_old + h12 * y_old;
      y[(size_t)i * (size_t)incy] = h21 * x_old + y_old;
    }
  } else {
    float h11 = param[1];
    float h22 = param[4];
    for (i = 0; i < n; i++) {
      float x_old = tmp[(size_t)i];
      float y_old = tmp[(size_t)n + (size_t)i];
      x[(size_t)i * (size_t)incx] = h11 * x_old + y_old;
      y[(size_t)i * (size_t)incy] = -x_old + h22 * y_old;
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_drotm_impl(
    cublasHandle_t handle,
    int n,
    double *x,
    int incx,
    double *y,
    int incy,
    const double *param) {
  /* ROTM reads param on CPU, so device pointer mode is outside this shim contract. */
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 1);
  double *tmp = NULL;
  size_t tmp_bytes = 0;
  double flag = 0.0;
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == NULL || y == NULL || param == NULL) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  flag = param[0];
  if (flag != -2.0 && flag != -1.0 && flag != 0.0 && flag != 1.0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (flag == -2.0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  status = psyche_cublas_temp_bytes(n, 2, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == NULL) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  for (i = 0; i < n; i++) {
    tmp[(size_t)i] = x[(size_t)i * (size_t)incx];
    tmp[(size_t)n + (size_t)i] = y[(size_t)i * (size_t)incy];
  }
  if (flag == -1.0) {
    double h11 = param[1];
    double h21 = param[2];
    double h12 = param[3];
    double h22 = param[4];
    for (i = 0; i < n; i++) {
      double x_old = tmp[(size_t)i];
      double y_old = tmp[(size_t)n + (size_t)i];
      x[(size_t)i * (size_t)incx] = h11 * x_old + h12 * y_old;
      y[(size_t)i * (size_t)incy] = h21 * x_old + h22 * y_old;
    }
  } else if (flag == 0.0) {
    double h21 = param[2];
    double h12 = param[3];
    for (i = 0; i < n; i++) {
      double x_old = tmp[(size_t)i];
      double y_old = tmp[(size_t)n + (size_t)i];
      x[(size_t)i * (size_t)incx] = x_old + h12 * y_old;
      y[(size_t)i * (size_t)incy] = h21 * x_old + y_old;
    }
  } else {
    double h11 = param[1];
    double h22 = param[4];
    for (i = 0; i < n; i++) {
      double x_old = tmp[(size_t)i];
      double y_old = tmp[(size_t)n + (size_t)i];
      x[(size_t)i * (size_t)incx] = h11 * x_old + y_old;
      y[(size_t)i * (size_t)incy] = -x_old + h22 * y_old;
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_sswap_impl(
    cublasHandle_t handle,
    int n,
    float *x,
    int incx,
    float *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 0);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    float tmp = x[(size_t)i * (size_t)incx];
    x[(size_t)i * (size_t)incx] = y[(size_t)i * (size_t)incy];
    y[(size_t)i * (size_t)incy] = tmp;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_dswap_impl(
    cublasHandle_t handle,
    int n,
    double *x,
    int incx,
    double *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 0);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    double tmp = x[(size_t)i * (size_t)incx];
    x[(size_t)i * (size_t)incx] = y[(size_t)i * (size_t)incy];
    y[(size_t)i * (size_t)incy] = tmp;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_cswap_impl(
    cublasHandle_t handle,
    int n,
    cuComplex *x,
    int incx,
    cuComplex *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 0);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    cuComplex tmp = x[(size_t)i * (size_t)incx];
    x[(size_t)i * (size_t)incx] = y[(size_t)i * (size_t)incy];
    y[(size_t)i * (size_t)incy] = tmp;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublas_zswap_impl(
    cublasHandle_t handle,
    int n,
    cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *y,
    int incy) {
  cublasStatus_t status = psyche_cublas_validate_handle(handle, 0);
  int i = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incx);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublas_validate_positive_stride(n, incy);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (x == 0 || y == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  for (i = 0; i < n; i++) {
    cuDoubleComplex tmp = x[(size_t)i * (size_t)incx];
    x[(size_t)i * (size_t)incx] = y[(size_t)i * (size_t)incy];
    y[(size_t)i * (size_t)incy] = tmp;
  }
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCreate_v2(cublasHandle_t *handle) {
  PsycheCublasContext *ctx = 0;
  if (handle == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *handle = 0;
  if (!psyche_cublas_simulated_memory_enabled()) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  ctx = (PsycheCublasContext *)malloc(sizeof(*ctx));
  if (ctx == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  ctx->magic = PSYCHE_CUBLAS_HANDLE_MAGIC;
  ctx->stream = 0;
  ctx->pointer_mode = CUBLAS_POINTER_MODE_HOST;
  ctx->math_mode = CUBLAS_DEFAULT_MATH;
  ctx->atomics_mode = CUBLAS_ATOMICS_NOT_ALLOWED;
  ctx->next = 0;
  psyche_cublas_register_context(ctx);
  *handle = (cublasHandle_t)ctx;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCreate(cublasHandle_t *handle) {
  return cublasCreate_v2(handle);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDestroy_v2(cublasHandle_t handle) {
  PsycheCublasContext *ctx = psyche_cublas_unregister_context(handle);
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  ctx->magic = 0;
  free(ctx);
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDestroy(cublasHandle_t handle) {
  return cublasDestroy_v2(handle);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetVersion_v2(
    cublasHandle_t handle,
    int *version) {
  if (version == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublas_context(handle) == 0) {
    *version = 0;
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  *version = PSYCHE_CUBLAS_VERSION;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetVersion(
    cublasHandle_t handle,
    int *version) {
  return cublasGetVersion_v2(handle, version);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetProperty(
    libraryPropertyType type,
    int *value) {
  if (value == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  switch (type) {
    case MAJOR_VERSION:
    case MINOR_VERSION:
    case PATCH_LEVEL:
      *value = 0;
      return CUBLAS_STATUS_SUCCESS;
    default:
      *value = 0;
      return CUBLAS_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API const char *cublasGetStatusName(cublasStatus_t status) {
  switch (status) {
    case CUBLAS_STATUS_SUCCESS: return "CUBLAS_STATUS_SUCCESS";
    case CUBLAS_STATUS_NOT_INITIALIZED: return "CUBLAS_STATUS_NOT_INITIALIZED";
    case CUBLAS_STATUS_ALLOC_FAILED: return "CUBLAS_STATUS_ALLOC_FAILED";
    case CUBLAS_STATUS_INVALID_VALUE: return "CUBLAS_STATUS_INVALID_VALUE";
    case CUBLAS_STATUS_ARCH_MISMATCH: return "CUBLAS_STATUS_ARCH_MISMATCH";
    case CUBLAS_STATUS_MAPPING_ERROR: return "CUBLAS_STATUS_MAPPING_ERROR";
    case CUBLAS_STATUS_EXECUTION_FAILED: return "CUBLAS_STATUS_EXECUTION_FAILED";
    case CUBLAS_STATUS_INTERNAL_ERROR: return "CUBLAS_STATUS_INTERNAL_ERROR";
    case CUBLAS_STATUS_NOT_SUPPORTED: return "CUBLAS_STATUS_NOT_SUPPORTED";
    case CUBLAS_STATUS_LICENSE_ERROR: return "CUBLAS_STATUS_LICENSE_ERROR";
    default: return "CUBLAS_STATUS_UNKNOWN";
  }
}

PSYCHE_CUDA_STUB_API const char *cublasGetStatusString(cublasStatus_t status) {
  switch (status) {
    case CUBLAS_STATUS_SUCCESS: return "operation completed successfully";
    case CUBLAS_STATUS_NOT_INITIALIZED: return "cuBLAS shim was not initialized";
    case CUBLAS_STATUS_ALLOC_FAILED: return "host allocation failed";
    case CUBLAS_STATUS_INVALID_VALUE: return "invalid value";
    case CUBLAS_STATUS_ARCH_MISMATCH: return "architecture mismatch";
    case CUBLAS_STATUS_MAPPING_ERROR: return "mapping error";
    case CUBLAS_STATUS_EXECUTION_FAILED: return "execution failed";
    case CUBLAS_STATUS_INTERNAL_ERROR: return "internal error";
    case CUBLAS_STATUS_NOT_SUPPORTED: return "operation is not supported by the shim";
    case CUBLAS_STATUS_LICENSE_ERROR: return "license error";
    default: return "unknown cuBLAS status";
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSetStream_v2(
    cublasHandle_t handle,
    cudaStream_t streamId) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  ctx->stream = streamId;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSetStream(
    cublasHandle_t handle,
    cudaStream_t streamId) {
  return cublasSetStream_v2(handle, streamId);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetStream_v2(
    cublasHandle_t handle,
    cudaStream_t *streamId) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  if (streamId == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *streamId = 0;
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  *streamId = ctx->stream;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetStream(
    cublasHandle_t handle,
    cudaStream_t *streamId) {
  return cublasGetStream_v2(handle, streamId);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSetVector(
    int n,
    int elemSize,
    const void *x,
    int incx,
    void *y,
    int incy) {
  return psyche_cublas_copy_vector_bytes(n, elemSize, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetVector(
    int n,
    int elemSize,
    const void *x,
    int incx,
    void *y,
    int incy) {
  return psyche_cublas_copy_vector_bytes(n, elemSize, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSetMatrix(
    int rows,
    int cols,
    int elemSize,
    const void *A,
    int lda,
    void *B,
    int ldb) {
  return psyche_cublas_copy_matrix_bytes(rows, cols, elemSize, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetMatrix(
    int rows,
    int cols,
    int elemSize,
    const void *A,
    int lda,
    void *B,
    int ldb) {
  return psyche_cublas_copy_matrix_bytes(rows, cols, elemSize, A, lda, B, ldb);
}

/* Async transfer helpers execute synchronously; streams are metadata only. */
PSYCHE_CUDA_STUB_API cublasStatus_t cublasSetVectorAsync(
    int n,
    int elemSize,
    const void *hostPtr,
    int incx,
    void *devicePtr,
    int incy,
    cudaStream_t stream) {
  (void)stream;
  return psyche_cublas_copy_vector_bytes(n, elemSize, hostPtr, incx, devicePtr, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetVectorAsync(
    int n,
    int elemSize,
    const void *devicePtr,
    int incx,
    void *hostPtr,
    int incy,
    cudaStream_t stream) {
  (void)stream;
  return psyche_cublas_copy_vector_bytes(n, elemSize, devicePtr, incx, hostPtr, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSetMatrixAsync(
    int rows,
    int cols,
    int elemSize,
    const void *A,
    int lda,
    void *B,
    int ldb,
    cudaStream_t stream) {
  (void)stream;
  return psyche_cublas_copy_matrix_bytes(rows, cols, elemSize, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetMatrixAsync(
    int rows,
    int cols,
    int elemSize,
    const void *A,
    int lda,
    void *B,
    int ldb,
    cudaStream_t stream) {
  (void)stream;
  return psyche_cublas_copy_matrix_bytes(rows, cols, elemSize, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSetPointerMode_v2(
    cublasHandle_t handle,
    cublasPointerMode_t mode) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (mode != CUBLAS_POINTER_MODE_HOST && mode != CUBLAS_POINTER_MODE_DEVICE) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  ctx->pointer_mode = mode;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSetPointerMode(
    cublasHandle_t handle,
    cublasPointerMode_t mode) {
  return cublasSetPointerMode_v2(handle, mode);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetPointerMode_v2(
    cublasHandle_t handle,
    cublasPointerMode_t *mode) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  if (mode == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *mode = CUBLAS_POINTER_MODE_HOST;
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  *mode = ctx->pointer_mode;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetPointerMode(
    cublasHandle_t handle,
    cublasPointerMode_t *mode) {
  return cublasGetPointerMode_v2(handle, mode);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSetMathMode(
    cublasHandle_t handle,
    cublasMath_t mode) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (!psyche_cublas_math_mode_known(mode)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  ctx->math_mode = mode;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetMathMode(
    cublasHandle_t handle,
    cublasMath_t *mode) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  if (mode == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *mode = CUBLAS_DEFAULT_MATH;
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  *mode = ctx->math_mode;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSetAtomicsMode(
    cublasHandle_t handle,
    cublasAtomicsMode_t mode) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (mode != CUBLAS_ATOMICS_NOT_ALLOWED && mode != CUBLAS_ATOMICS_ALLOWED) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  ctx->atomics_mode = mode;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasGetAtomicsMode(
    cublasHandle_t handle,
    cublasAtomicsMode_t *mode) {
  PsycheCublasContext *ctx = psyche_cublas_context(handle);
  if (mode == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *mode = CUBLAS_ATOMICS_NOT_ALLOWED;
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  *mode = ctx->atomics_mode;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSasum_v2(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    float *result) {
  return psyche_cublas_sasum_impl(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSasum(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    float *result) {
  return cublasSasum_v2(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDasum_v2(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    double *result) {
  return psyche_cublas_dasum_impl(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDasum(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    double *result) {
  return cublasDasum_v2(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSnrm2_v2(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    float *result) {
  return psyche_cublas_snrm2_impl(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSnrm2(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    float *result) {
  return cublasSnrm2_v2(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDnrm2_v2(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    double *result) {
  return psyche_cublas_dnrm2_impl(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDnrm2(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    double *result) {
  return cublasDnrm2_v2(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasIsamax_v2(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    int *result) {
  return psyche_cublas_isamax_impl(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasIsamax(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    int *result) {
  return cublasIsamax_v2(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasIdamax_v2(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    int *result) {
  return psyche_cublas_idamax_impl(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasIdamax(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    int *result) {
  return cublasIdamax_v2(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasIsamin_v2(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    int *result) {
  return psyche_cublas_isamin_impl(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasIsamin(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    int *result) {
  return cublasIsamin_v2(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasIdamin_v2(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    int *result) {
  return psyche_cublas_idamin_impl(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasIdamin(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    int *result) {
  return cublasIdamin_v2(handle, n, x, incx, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSaxpy_v2(
    cublasHandle_t handle,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    float *y,
    int incy) {
  return psyche_cublas_saxpy_impl(handle, n, alpha, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSaxpy(
    cublasHandle_t handle,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    float *y,
    int incy) {
  return cublasSaxpy_v2(handle, n, alpha, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDaxpy_v2(
    cublasHandle_t handle,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    double *y,
    int incy) {
  return psyche_cublas_daxpy_impl(handle, n, alpha, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDaxpy(
    cublasHandle_t handle,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    double *y,
    int incy) {
  return cublasDaxpy_v2(handle, n, alpha, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCaxpy_v2(
    cublasHandle_t handle,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    cuComplex *y,
    int incy) {
  return psyche_cublas_caxpy_impl(handle, n, alpha, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCaxpy(
    cublasHandle_t handle,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    cuComplex *y,
    int incy) {
  return cublasCaxpy_v2(handle, n, alpha, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZaxpy_v2(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *y,
    int incy) {
  return psyche_cublas_zaxpy_impl(handle, n, alpha, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZaxpy(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *y,
    int incy) {
  return cublasZaxpy_v2(handle, n, alpha, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasScopy_v2(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    float *y,
    int incy) {
  return psyche_cublas_scopy_impl(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasScopy(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    float *y,
    int incy) {
  return cublasScopy_v2(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDcopy_v2(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    double *y,
    int incy) {
  return psyche_cublas_dcopy_impl(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDcopy(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    double *y,
    int incy) {
  return cublasDcopy_v2(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCcopy_v2(
    cublasHandle_t handle,
    int n,
    const cuComplex *x,
    int incx,
    cuComplex *y,
    int incy) {
  return psyche_cublas_ccopy_impl(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCcopy(
    cublasHandle_t handle,
    int n,
    const cuComplex *x,
    int incx,
    cuComplex *y,
    int incy) {
  return cublasCcopy_v2(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZcopy_v2(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *y,
    int incy) {
  return psyche_cublas_zcopy_impl(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZcopy(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *y,
    int incy) {
  return cublasZcopy_v2(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSdot_v2(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    const float *y,
    int incy,
    float *result) {
  return psyche_cublas_sdot_impl(handle, n, x, incx, y, incy, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSdot(
    cublasHandle_t handle,
    int n,
    const float *x,
    int incx,
    const float *y,
    int incy,
    float *result) {
  return cublasSdot_v2(handle, n, x, incx, y, incy, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDdot_v2(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    const double *y,
    int incy,
    double *result) {
  return psyche_cublas_ddot_impl(handle, n, x, incx, y, incy, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDdot(
    cublasHandle_t handle,
    int n,
    const double *x,
    int incx,
    const double *y,
    int incy,
    double *result) {
  return cublasDdot_v2(handle, n, x, incx, y, incy, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCdotu_v2(
    cublasHandle_t handle,
    int n,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *result) {
  return psyche_cublas_cdot_impl(handle, n, x, incx, y, incy, result, 0);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCdotu(
    cublasHandle_t handle,
    int n,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *result) {
  return cublasCdotu_v2(handle, n, x, incx, y, incy, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCdotc_v2(
    cublasHandle_t handle,
    int n,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *result) {
  return psyche_cublas_cdot_impl(handle, n, x, incx, y, incy, result, 1);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCdotc(
    cublasHandle_t handle,
    int n,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *result) {
  return cublasCdotc_v2(handle, n, x, incx, y, incy, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZdotu_v2(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *result) {
  return psyche_cublas_zdot_impl(handle, n, x, incx, y, incy, result, 0);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZdotu(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *result) {
  return cublasZdotu_v2(handle, n, x, incx, y, incy, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZdotc_v2(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *result) {
  return psyche_cublas_zdot_impl(handle, n, x, incx, y, incy, result, 1);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZdotc(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *result) {
  return cublasZdotc_v2(handle, n, x, incx, y, incy, result);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSscal_v2(
    cublasHandle_t handle,
    int n,
    const float *alpha,
    float *x,
    int incx) {
  return psyche_cublas_sscal_impl(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSscal(
    cublasHandle_t handle,
    int n,
    const float *alpha,
    float *x,
    int incx) {
  return cublasSscal_v2(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDscal_v2(
    cublasHandle_t handle,
    int n,
    const double *alpha,
    double *x,
    int incx) {
  return psyche_cublas_dscal_impl(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDscal(
    cublasHandle_t handle,
    int n,
    const double *alpha,
    double *x,
    int incx) {
  return cublasDscal_v2(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCscal_v2(
    cublasHandle_t handle,
    int n,
    const cuComplex *alpha,
    cuComplex *x,
    int incx) {
  return psyche_cublas_cscal_impl(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCscal(
    cublasHandle_t handle,
    int n,
    const cuComplex *alpha,
    cuComplex *x,
    int incx) {
  return cublasCscal_v2(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCsscal_v2(
    cublasHandle_t handle,
    int n,
    const float *alpha,
    cuComplex *x,
    int incx) {
  return psyche_cublas_csscal_impl(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCsscal(
    cublasHandle_t handle,
    int n,
    const float *alpha,
    cuComplex *x,
    int incx) {
  return cublasCsscal_v2(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZscal_v2(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *alpha,
    cuDoubleComplex *x,
    int incx) {
  return psyche_cublas_zscal_impl(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZscal(
    cublasHandle_t handle,
    int n,
    const cuDoubleComplex *alpha,
    cuDoubleComplex *x,
    int incx) {
  return cublasZscal_v2(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZdscal_v2(
    cublasHandle_t handle,
    int n,
    const double *alpha,
    cuDoubleComplex *x,
    int incx) {
  return psyche_cublas_zdscal_impl(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZdscal(
    cublasHandle_t handle,
    int n,
    const double *alpha,
    cuDoubleComplex *x,
    int incx) {
  return cublasZdscal_v2(handle, n, alpha, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSrot_v2(
    cublasHandle_t handle,
    int n,
    float *x,
    int incx,
    float *y,
    int incy,
    const float *c,
    const float *s) {
  return psyche_cublas_srot_impl(handle, n, x, incx, y, incy, c, s);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSrot(
    cublasHandle_t handle,
    int n,
    float *x,
    int incx,
    float *y,
    int incy,
    const float *c,
    const float *s) {
  return cublasSrot_v2(handle, n, x, incx, y, incy, c, s);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDrot_v2(
    cublasHandle_t handle,
    int n,
    double *x,
    int incx,
    double *y,
    int incy,
    const double *c,
    const double *s) {
  return psyche_cublas_drot_impl(handle, n, x, incx, y, incy, c, s);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDrot(
    cublasHandle_t handle,
    int n,
    double *x,
    int incx,
    double *y,
    int incy,
    const double *c,
    const double *s) {
  return cublasDrot_v2(handle, n, x, incx, y, incy, c, s);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSrotg_v2(
    cublasHandle_t handle,
    float *a,
    float *b,
    float *c,
    float *s) {
  return psyche_cublas_srotg_impl(handle, a, b, c, s);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSrotg(
    cublasHandle_t handle,
    float *a,
    float *b,
    float *c,
    float *s) {
  return cublasSrotg_v2(handle, a, b, c, s);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDrotg_v2(
    cublasHandle_t handle,
    double *a,
    double *b,
    double *c,
    double *s) {
  return psyche_cublas_drotg_impl(handle, a, b, c, s);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDrotg(
    cublasHandle_t handle,
    double *a,
    double *b,
    double *c,
    double *s) {
  return cublasDrotg_v2(handle, a, b, c, s);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSrotm_v2(
    cublasHandle_t handle,
    int n,
    float *x,
    int incx,
    float *y,
    int incy,
    const float *param) {
  return psyche_cublas_srotm_impl(handle, n, x, incx, y, incy, param);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSrotm(
    cublasHandle_t handle,
    int n,
    float *x,
    int incx,
    float *y,
    int incy,
    const float *param) {
  return cublasSrotm_v2(handle, n, x, incx, y, incy, param);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDrotm_v2(
    cublasHandle_t handle,
    int n,
    double *x,
    int incx,
    double *y,
    int incy,
    const double *param) {
  return psyche_cublas_drotm_impl(handle, n, x, incx, y, incy, param);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDrotm(
    cublasHandle_t handle,
    int n,
    double *x,
    int incx,
    double *y,
    int incy,
    const double *param) {
  return cublasDrotm_v2(handle, n, x, incx, y, incy, param);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSrotmg_v2(
    cublasHandle_t handle,
    float *d1,
    float *d2,
    float *x1,
    const float *y1,
    float *param) {
  return psyche_cublas_srotmg_impl(handle, d1, d2, x1, y1, param);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSrotmg(
    cublasHandle_t handle,
    float *d1,
    float *d2,
    float *x1,
    const float *y1,
    float *param) {
  return cublasSrotmg_v2(handle, d1, d2, x1, y1, param);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDrotmg_v2(
    cublasHandle_t handle,
    double *d1,
    double *d2,
    double *x1,
    const double *y1,
    double *param) {
  return psyche_cublas_drotmg_impl(handle, d1, d2, x1, y1, param);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDrotmg(
    cublasHandle_t handle,
    double *d1,
    double *d2,
    double *x1,
    const double *y1,
    double *param) {
  return cublasDrotmg_v2(handle, d1, d2, x1, y1, param);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSswap_v2(
    cublasHandle_t handle,
    int n,
    float *x,
    int incx,
    float *y,
    int incy) {
  return psyche_cublas_sswap_impl(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSswap(
    cublasHandle_t handle,
    int n,
    float *x,
    int incx,
    float *y,
    int incy) {
  return cublasSswap_v2(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDswap_v2(
    cublasHandle_t handle,
    int n,
    double *x,
    int incx,
    double *y,
    int incy) {
  return psyche_cublas_dswap_impl(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDswap(
    cublasHandle_t handle,
    int n,
    double *x,
    int incx,
    double *y,
    int incy) {
  return cublasDswap_v2(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCswap_v2(
    cublasHandle_t handle,
    int n,
    cuComplex *x,
    int incx,
    cuComplex *y,
    int incy) {
  return psyche_cublas_cswap_impl(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCswap(
    cublasHandle_t handle,
    int n,
    cuComplex *x,
    int incx,
    cuComplex *y,
    int incy) {
  return cublasCswap_v2(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZswap_v2(
    cublasHandle_t handle,
    int n,
    cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *y,
    int incy) {
  return psyche_cublas_zswap_impl(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZswap(
    cublasHandle_t handle,
    int n,
    cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *y,
    int incy) {
  return cublasZswap_v2(handle, n, x, incx, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSger_v2(
    cublasHandle_t handle,
    int m,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    const float *y,
    int incy,
    float *A,
    int lda) {
  return psyche_cublas_sger_impl(handle, m, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSger(
    cublasHandle_t handle,
    int m,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    const float *y,
    int incy,
    float *A,
    int lda) {
  return cublasSger_v2(handle, m, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDger_v2(
    cublasHandle_t handle,
    int m,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    const double *y,
    int incy,
    double *A,
    int lda) {
  return psyche_cublas_dger_impl(handle, m, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDger(
    cublasHandle_t handle,
    int m,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    const double *y,
    int incy,
    double *A,
    int lda) {
  return cublasDger_v2(handle, m, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCgeru_v2(
    cublasHandle_t handle,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *A,
    int lda) {
  return psyche_cublas_cger_impl(handle, m, n, alpha, x, incx, y, incy, A, lda, 0);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCgeru(
    cublasHandle_t handle,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *A,
    int lda) {
  return cublasCgeru_v2(handle, m, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCgerc_v2(
    cublasHandle_t handle,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *A,
    int lda) {
  return psyche_cublas_cger_impl(handle, m, n, alpha, x, incx, y, incy, A, lda, 1);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCgerc(
    cublasHandle_t handle,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *A,
    int lda) {
  return cublasCgerc_v2(handle, m, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZgeru_v2(
    cublasHandle_t handle,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *A,
    int lda) {
  return psyche_cublas_zger_impl(handle, m, n, alpha, x, incx, y, incy, A, lda, 0);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZgeru(
    cublasHandle_t handle,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *A,
    int lda) {
  return cublasZgeru_v2(handle, m, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZgerc_v2(
    cublasHandle_t handle,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *A,
    int lda) {
  return psyche_cublas_zger_impl(handle, m, n, alpha, x, incx, y, incy, A, lda, 1);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZgerc(
    cublasHandle_t handle,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *A,
    int lda) {
  return cublasZgerc_v2(handle, m, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasChemv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *x,
    int incx,
    const cuComplex *beta,
    cuComplex *y,
    int incy) {
  return psyche_cublas_chemv_impl(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasChemv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *x,
    int incx,
    const cuComplex *beta,
    cuComplex *y,
    int incy) {
  return cublasChemv_v2(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZhemv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *beta,
    cuDoubleComplex *y,
    int incy) {
  return psyche_cublas_zhemv_impl(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZhemv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *beta,
    cuDoubleComplex *y,
    int incy) {
  return cublasZhemv_v2(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCher_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const cuComplex *x,
    int incx,
    cuComplex *A,
    int lda) {
  return psyche_cublas_cher_impl(handle, uplo, n, alpha, x, incx, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCher(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const cuComplex *x,
    int incx,
    cuComplex *A,
    int lda) {
  return cublasCher_v2(handle, uplo, n, alpha, x, incx, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZher_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *A,
    int lda) {
  return psyche_cublas_zher_impl(handle, uplo, n, alpha, x, incx, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZher(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const cuDoubleComplex *x,
    int incx,
    cuDoubleComplex *A,
    int lda) {
  return cublasZher_v2(handle, uplo, n, alpha, x, incx, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCher2_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *A,
    int lda) {
  return psyche_cublas_cher2_impl(handle, uplo, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCher2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuComplex *alpha,
    const cuComplex *x,
    int incx,
    const cuComplex *y,
    int incy,
    cuComplex *A,
    int lda) {
  return cublasCher2_v2(handle, uplo, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZher2_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *A,
    int lda) {
  return psyche_cublas_zher2_impl(handle, uplo, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZher2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *y,
    int incy,
    cuDoubleComplex *A,
    int lda) {
  return cublasZher2_v2(handle, uplo, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsymv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *x,
    int incx,
    const float *beta,
    float *y,
    int incy) {
  return psyche_cublas_ssymv_impl(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsymv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *x,
    int incx,
    const float *beta,
    float *y,
    int incy) {
  return cublasSsymv_v2(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsymv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *x,
    int incx,
    const double *beta,
    double *y,
    int incy) {
  return psyche_cublas_dsymv_impl(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsymv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *x,
    int incx,
    const double *beta,
    double *y,
    int incy) {
  return cublasDsymv_v2(handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsyr_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    float *A,
    int lda) {
  return psyche_cublas_ssyr_impl(handle, uplo, n, alpha, x, incx, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsyr(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    float *A,
    int lda) {
  return cublasSsyr_v2(handle, uplo, n, alpha, x, incx, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsyr_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    double *A,
    int lda) {
  return psyche_cublas_dsyr_impl(handle, uplo, n, alpha, x, incx, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsyr(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    double *A,
    int lda) {
  return cublasDsyr_v2(handle, uplo, n, alpha, x, incx, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsyr2_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    const float *y,
    int incy,
    float *A,
    int lda) {
  return psyche_cublas_ssyr2_impl(handle, uplo, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsyr2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const float *alpha,
    const float *x,
    int incx,
    const float *y,
    int incy,
    float *A,
    int lda) {
  return cublasSsyr2_v2(handle, uplo, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsyr2_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    const double *y,
    int incy,
    double *A,
    int lda) {
  return psyche_cublas_dsyr2_impl(handle, uplo, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsyr2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    const double *alpha,
    const double *x,
    int incx,
    const double *y,
    int incy,
    double *A,
    int lda) {
  return cublasDsyr2_v2(handle, uplo, n, alpha, x, incx, y, incy, A, lda);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsymm_v2(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    const float *beta,
    float *C,
    int ldc) {
  return psyche_cublas_ssymm_impl(handle, side, uplo, m, n, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsymm(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    const float *beta,
    float *C,
    int ldc) {
  return cublasSsymm_v2(handle, side, uplo, m, n, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsymm_v2(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    const double *beta,
    double *C,
    int ldc) {
  return psyche_cublas_dsymm_impl(handle, side, uplo, m, n, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsymm(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    const double *beta,
    double *C,
    int ldc) {
  return cublasDsymm_v2(handle, side, uplo, m, n, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsyrk_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    const float *beta,
    float *C,
    int ldc) {
  return psyche_cublas_ssyrk_impl(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsyrk(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    const float *beta,
    float *C,
    int ldc) {
  return cublasSsyrk_v2(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsyrk_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    const double *beta,
    double *C,
    int ldc) {
  return psyche_cublas_dsyrk_impl(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsyrk(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    const double *beta,
    double *C,
    int ldc) {
  return cublasDsyrk_v2(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsyr2k_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    const float *beta,
    float *C,
    int ldc) {
  return psyche_cublas_ssyr2k_impl(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSsyr2k(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    const float *beta,
    float *C,
    int ldc) {
  return cublasSsyr2k_v2(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsyr2k_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    const double *beta,
    double *C,
    int ldc) {
  return psyche_cublas_dsyr2k_impl(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDsyr2k(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    const double *beta,
    double *C,
    int ldc) {
  return cublasDsyr2k_v2(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCherk_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const float *alpha,
    const cuComplex *A,
    int lda,
    const float *beta,
    cuComplex *C,
    int ldc) {
  return psyche_cublas_cherk_impl(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCherk(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const float *alpha,
    const cuComplex *A,
    int lda,
    const float *beta,
    cuComplex *C,
    int ldc) {
  return cublasCherk_v2(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZherk_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const double *alpha,
    const cuDoubleComplex *A,
    int lda,
    const double *beta,
    cuDoubleComplex *C,
    int ldc) {
  return psyche_cublas_zherk_impl(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZherk(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const double *alpha,
    const cuDoubleComplex *A,
    int lda,
    const double *beta,
    cuDoubleComplex *C,
    int ldc) {
  return cublasZherk_v2(handle, uplo, trans, n, k, alpha, A, lda, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCher2k_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    const float *beta,
    cuComplex *C,
    int ldc) {
  return psyche_cublas_cher2k_impl(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCher2k(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    const float *beta,
    cuComplex *C,
    int ldc) {
  return cublasCher2k_v2(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZher2k_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    const double *beta,
    cuDoubleComplex *C,
    int ldc) {
  return psyche_cublas_zher2k_impl(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZher2k(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    const double *beta,
    cuDoubleComplex *C,
    int ldc) {
  return cublasZher2k_v2(handle, uplo, trans, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasStrmv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const float *A,
    int lda,
    float *x,
    int incx) {
  return psyche_cublas_strmv_impl(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasStrmv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const float *A,
    int lda,
    float *x,
    int incx) {
  return cublasStrmv_v2(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDtrmv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const double *A,
    int lda,
    double *x,
    int incx) {
  return psyche_cublas_dtrmv_impl(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDtrmv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const double *A,
    int lda,
    double *x,
    int incx) {
  return cublasDtrmv_v2(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCtrmv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuComplex *A,
    int lda,
    cuComplex *x,
    int incx) {
  return psyche_cublas_ctrmv_impl(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCtrmv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuComplex *A,
    int lda,
    cuComplex *x,
    int incx) {
  return cublasCtrmv_v2(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZtrmv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuDoubleComplex *A,
    int lda,
    cuDoubleComplex *x,
    int incx) {
  return psyche_cublas_ztrmv_impl(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZtrmv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuDoubleComplex *A,
    int lda,
    cuDoubleComplex *x,
    int incx) {
  return cublasZtrmv_v2(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasStrsv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const float *A,
    int lda,
    float *x,
    int incx) {
  return psyche_cublas_strsv_impl(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasStrsv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const float *A,
    int lda,
    float *x,
    int incx) {
  return cublasStrsv_v2(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDtrsv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const double *A,
    int lda,
    double *x,
    int incx) {
  return psyche_cublas_dtrsv_impl(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDtrsv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const double *A,
    int lda,
    double *x,
    int incx) {
  return cublasDtrsv_v2(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCtrsv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuComplex *A,
    int lda,
    cuComplex *x,
    int incx) {
  return psyche_cublas_ctrsv_impl(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCtrsv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuComplex *A,
    int lda,
    cuComplex *x,
    int incx) {
  return cublasCtrsv_v2(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZtrsv_v2(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuDoubleComplex *A,
    int lda,
    cuDoubleComplex *x,
    int incx) {
  return psyche_cublas_ztrsv_impl(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZtrsv(
    cublasHandle_t handle,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int n,
    const cuDoubleComplex *A,
    int lda,
    cuDoubleComplex *x,
    int incx) {
  return cublasZtrsv_v2(handle, uplo, trans, diag, n, A, lda, x, incx);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasStrmm_v2(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    float *C,
    int ldc) {
  return psyche_cublas_strmm_impl(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasStrmm(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    float *C,
    int ldc) {
  return cublasStrmm_v2(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDtrmm_v2(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    double *C,
    int ldc) {
  return psyche_cublas_dtrmm_impl(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDtrmm(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    double *C,
    int ldc) {
  return cublasDtrmm_v2(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasStrsm_v2(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    float *B,
    int ldb) {
  return psyche_cublas_strsm_impl(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasStrsm(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    float *B,
    int ldb) {
  return cublasStrsm_v2(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDtrsm_v2(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    double *B,
    int ldb) {
  return psyche_cublas_dtrsm_impl(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDtrsm(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    double *B,
    int ldb) {
  return cublasDtrsm_v2(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCtrmm_v2(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    cuComplex *C,
    int ldc) {
  return psyche_cublas_ctrmm_impl(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCtrmm(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    cuComplex *C,
    int ldc) {
  return cublasCtrmm_v2(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZtrmm_v2(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    cuDoubleComplex *C,
    int ldc) {
  return psyche_cublas_ztrmm_impl(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZtrmm(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    cuDoubleComplex *C,
    int ldc) {
  return cublasZtrmm_v2(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCtrsm_v2(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    cuComplex *B,
    int ldb) {
  return psyche_cublas_ctrsm_impl(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCtrsm(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    cuComplex *B,
    int ldb) {
  return cublasCtrsm_v2(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZtrsm_v2(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    cuDoubleComplex *B,
    int ldb) {
  return psyche_cublas_ztrsm_impl(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZtrsm(
    cublasHandle_t handle,
    cublasSideMode_t side,
    cublasFillMode_t uplo,
    cublasOperation_t trans,
    cublasDiagType_t diag,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    cuDoubleComplex *B,
    int ldb) {
  return cublasZtrsm_v2(handle, side, uplo, trans, diag, m, n, alpha, A, lda, B, ldb);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSgemv_v2(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *x,
    int incx,
    const float *beta,
    float *y,
    int incy) {
  return psyche_cublas_sgemv_impl(handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy);
}

/* Current cuBLAS docs use the handle-based ABI for these names; legacy v1 is not modeled. */
PSYCHE_CUDA_STUB_API cublasStatus_t cublasSgemv(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const float *alpha,
    const float *A,
    int lda,
    const float *x,
    int incx,
    const float *beta,
    float *y,
    int incy) {
  return cublasSgemv_v2(handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDgemv_v2(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *x,
    int incx,
    const double *beta,
    double *y,
    int incy) {
  return psyche_cublas_dgemv_impl(handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDgemv(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const double *alpha,
    const double *A,
    int lda,
    const double *x,
    int incx,
    const double *beta,
    double *y,
    int incy) {
  return cublasDgemv_v2(handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCgemv_v2(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *x,
    int incx,
    const cuComplex *beta,
    cuComplex *y,
    int incy) {
  return psyche_cublas_cgemv_impl(handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCgemv(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *x,
    int incx,
    const cuComplex *beta,
    cuComplex *y,
    int incy) {
  return cublasCgemv_v2(handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZgemv_v2(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *beta,
    cuDoubleComplex *y,
    int incy) {
  return psyche_cublas_zgemv_impl(handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZgemv(
    cublasHandle_t handle,
    cublasOperation_t trans,
    int m,
    int n,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *x,
    int incx,
    const cuDoubleComplex *beta,
    cuDoubleComplex *y,
    int incy) {
  return cublasZgemv_v2(handle, trans, m, n, alpha, A, lda, x, incx, beta, y, incy);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSgemm_v2(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    const float *beta,
    float *C,
    int ldc) {
  return psyche_cublas_sgemm_impl(
      handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSgemm(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    const float *B,
    int ldb,
    const float *beta,
    float *C,
    int ldc) {
  return cublasSgemm_v2(handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDgemm_v2(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    const double *beta,
    double *C,
    int ldc) {
  return psyche_cublas_dgemm_impl(
      handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDgemm(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    const double *B,
    int ldb,
    const double *beta,
    double *C,
    int ldc) {
  return cublasDgemm_v2(handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCgemm_v2(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    const cuComplex *beta,
    cuComplex *C,
    int ldc) {
  return psyche_cublas_cgemm_impl(
      handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCgemm(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    const cuComplex *B,
    int ldb,
    const cuComplex *beta,
    cuComplex *C,
    int ldc) {
  return cublasCgemm_v2(handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZgemm_v2(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    const cuDoubleComplex *beta,
    cuDoubleComplex *C,
    int ldc) {
  return psyche_cublas_zgemm_impl(
      handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZgemm(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    const cuDoubleComplex *B,
    int ldb,
    const cuDoubleComplex *beta,
    cuDoubleComplex *C,
    int ldc) {
  return cublasZgemm_v2(handle, transa, transb, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSgemmBatched(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const float *alpha,
    const float *const Aarray[],
    int lda,
    const float *const Barray[],
    int ldb,
    const float *beta,
    float *const Carray[],
    int ldc,
    int batchCount) {
  return psyche_cublas_sgemm_batched_impl(
      handle, transa, transb, m, n, k, alpha, Aarray, lda, Barray, ldb, beta, Carray, ldc, batchCount);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDgemmBatched(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const double *alpha,
    const double *const Aarray[],
    int lda,
    const double *const Barray[],
    int ldb,
    const double *beta,
    double *const Carray[],
    int ldc,
    int batchCount) {
  return psyche_cublas_dgemm_batched_impl(
      handle, transa, transb, m, n, k, alpha, Aarray, lda, Barray, ldb, beta, Carray, ldc, batchCount);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCgemmBatched(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *const Aarray[],
    int lda,
    const cuComplex *const Barray[],
    int ldb,
    const cuComplex *beta,
    cuComplex *const Carray[],
    int ldc,
    int batchCount) {
  return psyche_cublas_cgemm_batched_impl(
      handle, transa, transb, m, n, k, alpha, Aarray, lda, Barray, ldb, beta, Carray, ldc, batchCount);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZgemmBatched(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *const Aarray[],
    int lda,
    const cuDoubleComplex *const Barray[],
    int ldb,
    const cuDoubleComplex *beta,
    cuDoubleComplex *const Carray[],
    int ldc,
    int batchCount) {
  return psyche_cublas_zgemm_batched_impl(
      handle, transa, transb, m, n, k, alpha, Aarray, lda, Barray, ldb, beta, Carray, ldc, batchCount);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasSgemmStridedBatched(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const float *alpha,
    const float *A,
    int lda,
    long long int strideA,
    const float *B,
    int ldb,
    long long int strideB,
    const float *beta,
    float *C,
    int ldc,
    long long int strideC,
    int batchCount) {
  return psyche_cublas_sgemm_strided_batched_impl(
      handle, transa, transb, m, n, k, alpha, A, lda, strideA, B, ldb, strideB, beta, C, ldc, strideC, batchCount);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasDgemmStridedBatched(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const double *alpha,
    const double *A,
    int lda,
    long long int strideA,
    const double *B,
    int ldb,
    long long int strideB,
    const double *beta,
    double *C,
    int ldc,
    long long int strideC,
    int batchCount) {
  return psyche_cublas_dgemm_strided_batched_impl(
      handle, transa, transb, m, n, k, alpha, A, lda, strideA, B, ldb, strideB, beta, C, ldc, strideC, batchCount);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasCgemmStridedBatched(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuComplex *alpha,
    const cuComplex *A,
    int lda,
    long long int strideA,
    const cuComplex *B,
    int ldb,
    long long int strideB,
    const cuComplex *beta,
    cuComplex *C,
    int ldc,
    long long int strideC,
    int batchCount) {
  return psyche_cublas_cgemm_strided_batched_impl(
      handle, transa, transb, m, n, k, alpha, A, lda, strideA, B, ldb, strideB, beta, C, ldc, strideC, batchCount);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasZgemmStridedBatched(
    cublasHandle_t handle,
    cublasOperation_t transa,
    cublasOperation_t transb,
    int m,
    int n,
    int k,
    const cuDoubleComplex *alpha,
    const cuDoubleComplex *A,
    int lda,
    long long int strideA,
    const cuDoubleComplex *B,
    int ldb,
    long long int strideB,
    const cuDoubleComplex *beta,
    cuDoubleComplex *C,
    int ldc,
    long long int strideC,
    int batchCount) {
  return psyche_cublas_zgemm_strided_batched_impl(
      handle, transa, transb, m, n, k, alpha, A, lda, strideA, B, ldb, strideB, beta, C, ldc, strideC, batchCount);
}
