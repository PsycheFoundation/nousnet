#define _POSIX_C_SOURCE 200809L

#include "cuda_compat_stub.h"

#include <limits.h>
#if !defined(__APPLE__)
#include <math.h>
#endif
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(__APPLE__)
#define ACCELERATE_NEW_LAPACK 1
#include <Accelerate/Accelerate.h>
typedef __LAPACK_int psyche_cusolver_lapack_int;
_Static_assert(sizeof(psyche_cusolver_lapack_int) == sizeof(int),
               "Psyche cuSOLVER shim requires Accelerate LAPACK pivots to match CUDA int pivots");
#else
typedef int psyche_cusolver_lapack_int;
#endif

#define PSYCHE_CUSOLVER_HANDLE_MAGIC UINT64_C(0x70737963736f6c76)
#define PSYCHE_CUSOLVER_VERSION 0

struct cusolverDnContext {
  uint64_t magic;
  void *stream;
  struct cusolverDnContext *next;
};

static pthread_mutex_t psyche_cusolver_handle_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct cusolverDnContext *psyche_cusolver_handles = 0;

static int psyche_cusolver_env_truthy(const char *value) {
  if (value == 0 || value[0] == '\0') {
    return 0;
  }
  return
      strcmp(value, "1") == 0 ||
      strcasecmp(value, "true") == 0 ||
      strcasecmp(value, "yes") == 0 ||
      strcasecmp(value, "on") == 0;
}

static int psyche_cusolver_simulated_memory_enabled(void) {
  return psyche_cusolver_env_truthy(getenv("PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY"));
}

static void psyche_cusolver_register_handle(struct cusolverDnContext *ctx) {
  pthread_mutex_lock(&psyche_cusolver_handle_mutex);
  ctx->next = psyche_cusolver_handles;
  psyche_cusolver_handles = ctx;
  pthread_mutex_unlock(&psyche_cusolver_handle_mutex);
}

static struct cusolverDnContext *psyche_cusolver_handle(cusolverDnHandle_t handle) {
  struct cusolverDnContext *cursor = 0;
  pthread_mutex_lock(&psyche_cusolver_handle_mutex);
  cursor = psyche_cusolver_handles;
  while (cursor != 0) {
    if ((cusolverDnHandle_t)cursor == handle && cursor->magic == PSYCHE_CUSOLVER_HANDLE_MAGIC) {
      pthread_mutex_unlock(&psyche_cusolver_handle_mutex);
      return cursor;
    }
    cursor = cursor->next;
  }
  pthread_mutex_unlock(&psyche_cusolver_handle_mutex);
  return 0;
}

static struct cusolverDnContext *psyche_cusolver_unregister_handle(cusolverDnHandle_t handle) {
  struct cusolverDnContext **cursor = 0;
  pthread_mutex_lock(&psyche_cusolver_handle_mutex);
  cursor = &psyche_cusolver_handles;
  while (*cursor != 0) {
    struct cusolverDnContext *ctx = *cursor;
    if ((cusolverDnHandle_t)ctx == handle && ctx->magic == PSYCHE_CUSOLVER_HANDLE_MAGIC) {
      *cursor = ctx->next;
      ctx->next = 0;
      pthread_mutex_unlock(&psyche_cusolver_handle_mutex);
      return ctx;
    }
    cursor = &ctx->next;
  }
  pthread_mutex_unlock(&psyche_cusolver_handle_mutex);
  return 0;
}

static int psyche_cusolver_aligned(const void *ptr, size_t alignment) {
  if (ptr == 0) {
    return 0;
  }
  return ((uintptr_t)ptr % alignment) == 0;
}

static int psyche_cusolver_mul_size(size_t a, size_t b, size_t *out) {
  if (out == 0) {
    return 0;
  }
  if (a != 0 && b > SIZE_MAX / a) {
    return 0;
  }
  *out = a * b;
  return 1;
}

static int psyche_cusolver_dense_bytes(int rows, int cols, int ld, size_t element_size, size_t *bytes) {
  size_t elements = 0;
  if (bytes == 0 || rows < 0 || cols < 0 || ld < 1) {
    return 0;
  }
  if (cols == 0 || rows == 0) {
    *bytes = 0;
    return 1;
  }
  if (ld < rows) {
    return 0;
  }
  if (!psyche_cusolver_mul_size((size_t)ld, (size_t)cols, &elements)) {
    return 0;
  }
  return psyche_cusolver_mul_size(elements, element_size, bytes);
}

static int psyche_cusolver_ranges_overlap(const void *a, size_t a_bytes, const void *b, size_t b_bytes) {
  uintptr_t a_start = (uintptr_t)a;
  uintptr_t b_start = (uintptr_t)b;
  uintptr_t a_end = 0;
  uintptr_t b_end = 0;
  if (a == 0 || b == 0 || a_bytes == 0 || b_bytes == 0) {
    return 0;
  }
  if (a_start > UINTPTR_MAX - a_bytes || b_start > UINTPTR_MAX - b_bytes) {
    return 1;
  }
  a_end = a_start + a_bytes;
  b_end = b_start + b_bytes;
  return a_start < b_end && b_start < a_end;
}

static int psyche_cusolver_cholesky_lwork(int n, int *lwork) {
  if (lwork == NULL || n < 0) {
    return 0;
  }
  if (n == 0) {
    *lwork = 0;
    return 1;
  }
  if (n > INT_MAX / n) {
    return 0;
  }
  *lwork = n * n;
  return 1;
}

static int psyche_cusolver_lapack_uplo(cublasFillMode_t uplo, char *lapack_uplo) {
  if (lapack_uplo == 0) {
    return 0;
  }
  if (uplo == CUBLAS_FILL_MODE_LOWER) {
    *lapack_uplo = 'L';
    return 1;
  }
  if (uplo == CUBLAS_FILL_MODE_UPPER) {
    *lapack_uplo = 'U';
    return 1;
  }
  return 0;
}

static cusolverStatus_t psyche_cusolver_invalid_with_info(int *devInfo, int parameter_index) {
  if (devInfo != 0 && psyche_cusolver_aligned(devInfo, sizeof(int))) {
    *devInfo = -parameter_index;
  }
  return CUSOLVER_STATUS_INVALID_VALUE;
}

static cusolverStatus_t psyche_cusolver_validate_getrf(
    cusolverDnHandle_t handle,
    int m,
    int n,
    void *A,
    int lda,
    void *Workspace,
    int *devIpiv,
    int *devInfo,
    size_t element_size,
    size_t *a_bytes,
    int *lwork) {
  size_t matrix_bytes = 0;
  if (!psyche_cusolver_simulated_memory_enabled()) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (psyche_cusolver_handle(handle) == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (m < 0 || n < 0 || lda < 1 || devInfo == 0 || !psyche_cusolver_aligned(devInfo, sizeof(int))) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (m > 0 && lda < m) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (!psyche_cusolver_dense_bytes(m, n, lda, element_size, &matrix_bytes)) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (matrix_bytes != 0) {
    if (A == 0 || Workspace == 0) {
      return CUSOLVER_STATUS_INVALID_VALUE;
    }
    if (!psyche_cusolver_aligned(A, element_size) || !psyche_cusolver_aligned(Workspace, element_size)) {
      return CUSOLVER_STATUS_INVALID_VALUE;
    }
  }
  if (devIpiv != 0 && !psyche_cusolver_aligned(devIpiv, sizeof(int))) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (m != 0 && n > INT_MAX / m) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (a_bytes != 0) {
    *a_bytes = matrix_bytes;
  }
  if (lwork != 0) {
    *lwork = m * n;
  }
  return CUSOLVER_STATUS_SUCCESS;
}

static cusolverStatus_t psyche_cusolver_validate_getrs(
    cusolverDnHandle_t handle,
    cublasOperation_t trans,
    int n,
    int nrhs,
    const void *A,
    int lda,
    const int *devIpiv,
    void *B,
    int ldb,
    int *devInfo,
    size_t element_size,
    size_t *a_bytes,
    size_t *b_bytes,
    char *lapack_trans) {
  size_t matrix_a_bytes = 0;
  size_t matrix_b_bytes = 0;
  if (!psyche_cusolver_simulated_memory_enabled()) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (psyche_cusolver_handle(handle) == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (trans == CUBLAS_OP_N) {
    *lapack_trans = 'N';
  } else if (trans == CUBLAS_OP_T) {
    *lapack_trans = 'T';
  } else if (trans == CUBLAS_OP_C) {
    *lapack_trans = 'C';
  } else {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (n < 0 || nrhs < 0 || lda < 1 || ldb < 1 || devInfo == 0 || !psyche_cusolver_aligned(devInfo, sizeof(int))) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (n > 0 && (lda < n || ldb < n)) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (!psyche_cusolver_dense_bytes(n, n, lda, element_size, &matrix_a_bytes) ||
      !psyche_cusolver_dense_bytes(n, nrhs, ldb, element_size, &matrix_b_bytes)) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (matrix_a_bytes != 0 && (A == 0 || !psyche_cusolver_aligned(A, element_size))) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (matrix_b_bytes != 0 && (B == 0 || !psyche_cusolver_aligned(B, element_size))) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (devIpiv != 0 && !psyche_cusolver_aligned(devIpiv, sizeof(int))) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (psyche_cusolver_ranges_overlap(A, matrix_a_bytes, B, matrix_b_bytes)) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (a_bytes != 0) {
    *a_bytes = matrix_a_bytes;
  }
  if (b_bytes != 0) {
    *b_bytes = matrix_b_bytes;
  }
  return CUSOLVER_STATUS_SUCCESS;
}

static cusolverStatus_t psyche_cusolver_validate_potrf_buffer_size(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    int lda,
    int *Lwork) {
  int required_lwork = 0;
  char lapack_uplo = 0;
  if (!psyche_cusolver_simulated_memory_enabled()) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (psyche_cusolver_handle(handle) == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (!psyche_cusolver_lapack_uplo(uplo, &lapack_uplo) ||
      n < 0 ||
      lda < 1 ||
      (n > 0 && lda < n) ||
      Lwork == 0) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (!psyche_cusolver_cholesky_lwork(n, &required_lwork)) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  *Lwork = required_lwork;
  return CUSOLVER_STATUS_SUCCESS;
}

static cusolverStatus_t psyche_cusolver_validate_potrf(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    void *A,
    int lda,
    void *Workspace,
    int Lwork,
    int *devInfo,
    size_t element_size,
    size_t *a_bytes,
    int *required_lwork,
    char *lapack_uplo) {
  size_t matrix_bytes = 0;
  int needed_lwork = 0;
  if (!psyche_cusolver_simulated_memory_enabled()) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (psyche_cusolver_handle(handle) == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (devInfo == 0 || !psyche_cusolver_aligned(devInfo, sizeof(int))) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  *devInfo = 0;
  if (!psyche_cusolver_lapack_uplo(uplo, lapack_uplo)) {
    return psyche_cusolver_invalid_with_info(devInfo, 2);
  }
  if (n < 0) {
    return psyche_cusolver_invalid_with_info(devInfo, 3);
  }
  if (lda < 1 || (n > 0 && lda < n)) {
    return psyche_cusolver_invalid_with_info(devInfo, 5);
  }
  if (!psyche_cusolver_dense_bytes(n, n, lda, element_size, &matrix_bytes)) {
    return psyche_cusolver_invalid_with_info(devInfo, 5);
  }
  if (matrix_bytes != 0) {
    if (A == 0 || !psyche_cusolver_aligned(A, element_size)) {
      return psyche_cusolver_invalid_with_info(devInfo, 4);
    }
  }
  if (!psyche_cusolver_cholesky_lwork(n, &needed_lwork) || Lwork < needed_lwork) {
    return psyche_cusolver_invalid_with_info(devInfo, 7);
  }
  if (needed_lwork != 0) {
    if (Workspace == 0 || !psyche_cusolver_aligned(Workspace, element_size)) {
      return psyche_cusolver_invalid_with_info(devInfo, 6);
    }
  }
  if (a_bytes != 0) {
    *a_bytes = matrix_bytes;
  }
  if (required_lwork != 0) {
    *required_lwork = needed_lwork;
  }
  return CUSOLVER_STATUS_SUCCESS;
}

static cusolverStatus_t psyche_cusolver_validate_potri_buffer_size(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    int lda,
    int *Lwork) {
  return psyche_cusolver_validate_potrf_buffer_size(handle, uplo, n, lda, Lwork);
}

static cusolverStatus_t psyche_cusolver_validate_potri(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    void *A,
    int lda,
    void *Workspace,
    int Lwork,
    int *devInfo,
    size_t element_size,
    size_t *a_bytes,
    char *lapack_uplo) {
  size_t matrix_bytes = 0;
  size_t workspace_bytes = 0;
  int needed_lwork = 0;
  if (!psyche_cusolver_simulated_memory_enabled()) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (psyche_cusolver_handle(handle) == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (devInfo == 0 || !psyche_cusolver_aligned(devInfo, sizeof(int))) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  *devInfo = 0;
  if (!psyche_cusolver_lapack_uplo(uplo, lapack_uplo)) {
    return psyche_cusolver_invalid_with_info(devInfo, 2);
  }
  if (n < 0) {
    return psyche_cusolver_invalid_with_info(devInfo, 3);
  }
  if (lda < 1 || (n > 0 && lda < n)) {
    return psyche_cusolver_invalid_with_info(devInfo, 5);
  }
  if (!psyche_cusolver_dense_bytes(n, n, lda, element_size, &matrix_bytes)) {
    return psyche_cusolver_invalid_with_info(devInfo, 5);
  }
  if (matrix_bytes != 0) {
    if (A == 0 || !psyche_cusolver_aligned(A, element_size)) {
      return psyche_cusolver_invalid_with_info(devInfo, 4);
    }
  }
  if (!psyche_cusolver_cholesky_lwork(n, &needed_lwork) || Lwork < needed_lwork) {
    return psyche_cusolver_invalid_with_info(devInfo, 7);
  }
  if (needed_lwork != 0) {
    if (Workspace == 0 || !psyche_cusolver_aligned(Workspace, element_size)) {
      return psyche_cusolver_invalid_with_info(devInfo, 6);
    }
    if (!psyche_cusolver_mul_size((size_t)needed_lwork, element_size, &workspace_bytes)) {
      return psyche_cusolver_invalid_with_info(devInfo, 7);
    }
    if (psyche_cusolver_ranges_overlap(A, matrix_bytes, Workspace, workspace_bytes)) {
      return psyche_cusolver_invalid_with_info(devInfo, 6);
    }
  }
  if (a_bytes != 0) {
    *a_bytes = matrix_bytes;
  }
  return CUSOLVER_STATUS_SUCCESS;
}

static cusolverStatus_t psyche_cusolver_validate_potrs(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    int nrhs,
    const void *A,
    int lda,
    void *B,
    int ldb,
    int *devInfo,
    size_t element_size,
    size_t *a_bytes,
    size_t *b_bytes,
    char *lapack_uplo) {
  size_t matrix_a_bytes = 0;
  size_t matrix_b_bytes = 0;
  if (!psyche_cusolver_simulated_memory_enabled()) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (psyche_cusolver_handle(handle) == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (devInfo == 0 || !psyche_cusolver_aligned(devInfo, sizeof(int))) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  *devInfo = 0;
  if (!psyche_cusolver_lapack_uplo(uplo, lapack_uplo)) {
    return psyche_cusolver_invalid_with_info(devInfo, 2);
  }
  if (n < 0) {
    return psyche_cusolver_invalid_with_info(devInfo, 3);
  }
  if (nrhs < 0) {
    return psyche_cusolver_invalid_with_info(devInfo, 4);
  }
  if (lda < 1 || (n > 0 && lda < n)) {
    return psyche_cusolver_invalid_with_info(devInfo, 6);
  }
  if (ldb < 1 || (n > 0 && ldb < n)) {
    return psyche_cusolver_invalid_with_info(devInfo, 8);
  }
  if (!psyche_cusolver_dense_bytes(n, n, lda, element_size, &matrix_a_bytes) ||
      !psyche_cusolver_dense_bytes(n, nrhs, ldb, element_size, &matrix_b_bytes)) {
    return psyche_cusolver_invalid_with_info(devInfo, 8);
  }
  if (matrix_a_bytes != 0 && (A == 0 || !psyche_cusolver_aligned(A, element_size))) {
    return psyche_cusolver_invalid_with_info(devInfo, 5);
  }
  if (matrix_b_bytes != 0 && (B == 0 || !psyche_cusolver_aligned(B, element_size))) {
    return psyche_cusolver_invalid_with_info(devInfo, 7);
  }
  if (psyche_cusolver_ranges_overlap(A, matrix_a_bytes, B, matrix_b_bytes)) {
    return psyche_cusolver_invalid_with_info(devInfo, 7);
  }
  if (a_bytes != 0) {
    *a_bytes = matrix_a_bytes;
  }
  if (b_bytes != 0) {
    *b_bytes = matrix_b_bytes;
  }
  return CUSOLVER_STATUS_SUCCESS;
}

static int psyche_cusolver_sgetrf_no_pivot(int m, int n, float *A, int lda) {
  int limit = m < n ? m : n;
  int k = 0;
  for (k = 0; k < limit; k++) {
    int i = 0;
    int j = 0;
    float pivot = A[(size_t)k + (size_t)k * (size_t)lda];
    if (pivot == 0.0f) {
      return k + 1;
    }
    for (i = k + 1; i < m; i++) {
      A[(size_t)i + (size_t)k * (size_t)lda] /= pivot;
    }
    for (j = k + 1; j < n; j++) {
      float upper = A[(size_t)k + (size_t)j * (size_t)lda];
      for (i = k + 1; i < m; i++) {
        A[(size_t)i + (size_t)j * (size_t)lda] -=
            A[(size_t)i + (size_t)k * (size_t)lda] * upper;
      }
    }
  }
  return 0;
}

static int psyche_cusolver_dgetrf_no_pivot(int m, int n, double *A, int lda) {
  int limit = m < n ? m : n;
  int k = 0;
  for (k = 0; k < limit; k++) {
    int i = 0;
    int j = 0;
    double pivot = A[(size_t)k + (size_t)k * (size_t)lda];
    if (pivot == 0.0) {
      return k + 1;
    }
    for (i = k + 1; i < m; i++) {
      A[(size_t)i + (size_t)k * (size_t)lda] /= pivot;
    }
    for (j = k + 1; j < n; j++) {
      double upper = A[(size_t)k + (size_t)j * (size_t)lda];
      for (i = k + 1; i < m; i++) {
        A[(size_t)i + (size_t)j * (size_t)lda] -=
            A[(size_t)i + (size_t)k * (size_t)lda] * upper;
      }
    }
  }
  return 0;
}

static int psyche_cusolver_sgetrs_no_pivot(char trans, int n, int nrhs, const float *A, int lda, float *B, int ldb);
static int psyche_cusolver_dgetrs_no_pivot(char trans, int n, int nrhs, const double *A, int lda, double *B, int ldb);

#if !defined(__APPLE__)
static void psyche_cusolver_sswap_rows(int cols, float *A, int lda, int row_a, int row_b) {
  if (row_a == row_b) {
    return;
  }
  for (int col = 0; col < cols; col++) {
    float tmp = A[(size_t)row_a + (size_t)col * (size_t)lda];
    A[(size_t)row_a + (size_t)col * (size_t)lda] = A[(size_t)row_b + (size_t)col * (size_t)lda];
    A[(size_t)row_b + (size_t)col * (size_t)lda] = tmp;
  }
}

static void psyche_cusolver_dswap_rows(int cols, double *A, int lda, int row_a, int row_b) {
  if (row_a == row_b) {
    return;
  }
  for (int col = 0; col < cols; col++) {
    double tmp = A[(size_t)row_a + (size_t)col * (size_t)lda];
    A[(size_t)row_a + (size_t)col * (size_t)lda] = A[(size_t)row_b + (size_t)col * (size_t)lda];
    A[(size_t)row_b + (size_t)col * (size_t)lda] = tmp;
  }
}

static int psyche_cusolver_sgetrf_pivot_reference(int m, int n, float *A, int lda, psyche_cusolver_lapack_int *ipiv) {
  int limit = m < n ? m : n;
  int info = 0;
  for (int k = 0; k < limit; k++) {
    int pivot_row = k;
    float pivot_abs = fabsf(A[(size_t)k + (size_t)k * (size_t)lda]);
    for (int row = k + 1; row < m; row++) {
      float candidate = fabsf(A[(size_t)row + (size_t)k * (size_t)lda]);
      if (candidate > pivot_abs) {
        pivot_abs = candidate;
        pivot_row = row;
      }
    }
    ipiv[k] = (psyche_cusolver_lapack_int)(pivot_row + 1);
    psyche_cusolver_sswap_rows(n, A, lda, k, pivot_row);
    /* Match LAPACK/cuSOLVER singular signaling: only an exact zero pivot sets positive info. */
    if (A[(size_t)k + (size_t)k * (size_t)lda] == 0.0f) {
      if (info == 0) {
        info = k + 1;
      }
      continue;
    }
    for (int row = k + 1; row < m; row++) {
      A[(size_t)row + (size_t)k * (size_t)lda] /= A[(size_t)k + (size_t)k * (size_t)lda];
    }
    for (int col = k + 1; col < n; col++) {
      float upper = A[(size_t)k + (size_t)col * (size_t)lda];
      for (int row = k + 1; row < m; row++) {
        A[(size_t)row + (size_t)col * (size_t)lda] -= A[(size_t)row + (size_t)k * (size_t)lda] * upper;
      }
    }
  }
  return info;
}

static int psyche_cusolver_dgetrf_pivot_reference(int m, int n, double *A, int lda, psyche_cusolver_lapack_int *ipiv) {
  int limit = m < n ? m : n;
  int info = 0;
  for (int k = 0; k < limit; k++) {
    int pivot_row = k;
    double pivot_abs = fabs(A[(size_t)k + (size_t)k * (size_t)lda]);
    for (int row = k + 1; row < m; row++) {
      double candidate = fabs(A[(size_t)row + (size_t)k * (size_t)lda]);
      if (candidate > pivot_abs) {
        pivot_abs = candidate;
        pivot_row = row;
      }
    }
    ipiv[k] = (psyche_cusolver_lapack_int)(pivot_row + 1);
    psyche_cusolver_dswap_rows(n, A, lda, k, pivot_row);
    /* Match LAPACK/cuSOLVER singular signaling: only an exact zero pivot sets positive info. */
    if (A[(size_t)k + (size_t)k * (size_t)lda] == 0.0) {
      if (info == 0) {
        info = k + 1;
      }
      continue;
    }
    for (int row = k + 1; row < m; row++) {
      A[(size_t)row + (size_t)k * (size_t)lda] /= A[(size_t)k + (size_t)k * (size_t)lda];
    }
    for (int col = k + 1; col < n; col++) {
      double upper = A[(size_t)k + (size_t)col * (size_t)lda];
      for (int row = k + 1; row < m; row++) {
        A[(size_t)row + (size_t)col * (size_t)lda] -= A[(size_t)row + (size_t)k * (size_t)lda] * upper;
      }
    }
  }
  return info;
}

static void psyche_cusolver_sapply_pivots(int n, int nrhs, float *B, int ldb, const psyche_cusolver_lapack_int *ipiv, int reverse) {
  if (!reverse) {
    for (int row = 0; row < n; row++) {
      int pivot_row = (int)ipiv[row] - 1;
      psyche_cusolver_sswap_rows(nrhs, B, ldb, row, pivot_row);
    }
  } else {
    for (int row = n - 1; row >= 0; row--) {
      int pivot_row = (int)ipiv[row] - 1;
      psyche_cusolver_sswap_rows(nrhs, B, ldb, row, pivot_row);
    }
  }
}

static void psyche_cusolver_dapply_pivots(int n, int nrhs, double *B, int ldb, const psyche_cusolver_lapack_int *ipiv, int reverse) {
  if (!reverse) {
    for (int row = 0; row < n; row++) {
      int pivot_row = (int)ipiv[row] - 1;
      psyche_cusolver_dswap_rows(nrhs, B, ldb, row, pivot_row);
    }
  } else {
    for (int row = n - 1; row >= 0; row--) {
      int pivot_row = (int)ipiv[row] - 1;
      psyche_cusolver_dswap_rows(nrhs, B, ldb, row, pivot_row);
    }
  }
}

static int psyche_cusolver_sgetrs_pivot_reference(char trans, int n, int nrhs, const float *A, int lda, const psyche_cusolver_lapack_int *ipiv, float *B, int ldb) {
  int info = 0;
  if (trans == 'N') {
    psyche_cusolver_sapply_pivots(n, nrhs, B, ldb, ipiv, 0);
    return psyche_cusolver_sgetrs_no_pivot('N', n, nrhs, A, lda, B, ldb);
  }
  info = psyche_cusolver_sgetrs_no_pivot(trans, n, nrhs, A, lda, B, ldb);
  if (info == 0) {
    psyche_cusolver_sapply_pivots(n, nrhs, B, ldb, ipiv, 1);
  }
  return info;
}

static int psyche_cusolver_dgetrs_pivot_reference(char trans, int n, int nrhs, const double *A, int lda, const psyche_cusolver_lapack_int *ipiv, double *B, int ldb) {
  int info = 0;
  if (trans == 'N') {
    psyche_cusolver_dapply_pivots(n, nrhs, B, ldb, ipiv, 0);
    return psyche_cusolver_dgetrs_no_pivot('N', n, nrhs, A, lda, B, ldb);
  }
  info = psyche_cusolver_dgetrs_no_pivot(trans, n, nrhs, A, lda, B, ldb);
  if (info == 0) {
    psyche_cusolver_dapply_pivots(n, nrhs, B, ldb, ipiv, 1);
  }
  return info;
}

static int psyche_cusolver_spotrf_reference(char uplo, int n, float *A, int lda) {
  if (uplo == 'L') {
    for (int j = 0; j < n; j++) {
      float sum = A[(size_t)j + (size_t)j * (size_t)lda];
      for (int k = 0; k < j; k++) {
        float ljk = A[(size_t)j + (size_t)k * (size_t)lda];
        sum -= ljk * ljk;
      }
      if (sum <= 0.0f) {
        return j + 1;
      }
      A[(size_t)j + (size_t)j * (size_t)lda] = sqrtf(sum);
      for (int i = j + 1; i < n; i++) {
        float value = A[(size_t)i + (size_t)j * (size_t)lda];
        for (int k = 0; k < j; k++) {
          value -= A[(size_t)i + (size_t)k * (size_t)lda] * A[(size_t)j + (size_t)k * (size_t)lda];
        }
        A[(size_t)i + (size_t)j * (size_t)lda] = value / A[(size_t)j + (size_t)j * (size_t)lda];
      }
    }
  } else {
    for (int j = 0; j < n; j++) {
      float sum = A[(size_t)j + (size_t)j * (size_t)lda];
      for (int k = 0; k < j; k++) {
        float ukj = A[(size_t)k + (size_t)j * (size_t)lda];
        sum -= ukj * ukj;
      }
      if (sum <= 0.0f) {
        return j + 1;
      }
      A[(size_t)j + (size_t)j * (size_t)lda] = sqrtf(sum);
      for (int col = j + 1; col < n; col++) {
        float value = A[(size_t)j + (size_t)col * (size_t)lda];
        for (int k = 0; k < j; k++) {
          value -= A[(size_t)k + (size_t)col * (size_t)lda] * A[(size_t)k + (size_t)j * (size_t)lda];
        }
        A[(size_t)j + (size_t)col * (size_t)lda] = value / A[(size_t)j + (size_t)j * (size_t)lda];
      }
    }
  }
  return 0;
}

static int psyche_cusolver_dpotrf_reference(char uplo, int n, double *A, int lda) {
  if (uplo == 'L') {
    for (int j = 0; j < n; j++) {
      double sum = A[(size_t)j + (size_t)j * (size_t)lda];
      for (int k = 0; k < j; k++) {
        double ljk = A[(size_t)j + (size_t)k * (size_t)lda];
        sum -= ljk * ljk;
      }
      if (sum <= 0.0) {
        return j + 1;
      }
      A[(size_t)j + (size_t)j * (size_t)lda] = sqrt(sum);
      for (int i = j + 1; i < n; i++) {
        double value = A[(size_t)i + (size_t)j * (size_t)lda];
        for (int k = 0; k < j; k++) {
          value -= A[(size_t)i + (size_t)k * (size_t)lda] * A[(size_t)j + (size_t)k * (size_t)lda];
        }
        A[(size_t)i + (size_t)j * (size_t)lda] = value / A[(size_t)j + (size_t)j * (size_t)lda];
      }
    }
  } else {
    for (int j = 0; j < n; j++) {
      double sum = A[(size_t)j + (size_t)j * (size_t)lda];
      for (int k = 0; k < j; k++) {
        double ukj = A[(size_t)k + (size_t)j * (size_t)lda];
        sum -= ukj * ukj;
      }
      if (sum <= 0.0) {
        return j + 1;
      }
      A[(size_t)j + (size_t)j * (size_t)lda] = sqrt(sum);
      for (int col = j + 1; col < n; col++) {
        double value = A[(size_t)j + (size_t)col * (size_t)lda];
        for (int k = 0; k < j; k++) {
          value -= A[(size_t)k + (size_t)col * (size_t)lda] * A[(size_t)k + (size_t)j * (size_t)lda];
        }
        A[(size_t)j + (size_t)col * (size_t)lda] = value / A[(size_t)j + (size_t)j * (size_t)lda];
      }
    }
  }
  return 0;
}

static int psyche_cusolver_spotrs_reference(char uplo, int n, int nrhs, const float *A, int lda, float *B, int ldb) {
  for (int rhs = 0; rhs < nrhs; rhs++) {
    float *x = B + (size_t)rhs * (size_t)ldb;
    if (uplo == 'L') {
      for (int i = 0; i < n; i++) {
        float sum = x[i];
        float diag = A[(size_t)i + (size_t)i * (size_t)lda];
        if (diag == 0.0f) {
          return i + 1;
        }
        for (int k = 0; k < i; k++) {
          sum -= A[(size_t)i + (size_t)k * (size_t)lda] * x[k];
        }
        x[i] = sum / diag;
      }
      for (int i = n - 1; i >= 0; i--) {
        float sum = x[i];
        float diag = A[(size_t)i + (size_t)i * (size_t)lda];
        if (diag == 0.0f) {
          return i + 1;
        }
        for (int k = i + 1; k < n; k++) {
          sum -= A[(size_t)k + (size_t)i * (size_t)lda] * x[k];
        }
        x[i] = sum / diag;
      }
    } else {
      for (int i = 0; i < n; i++) {
        float sum = x[i];
        float diag = A[(size_t)i + (size_t)i * (size_t)lda];
        if (diag == 0.0f) {
          return i + 1;
        }
        for (int k = 0; k < i; k++) {
          sum -= A[(size_t)k + (size_t)i * (size_t)lda] * x[k];
        }
        x[i] = sum / diag;
      }
      for (int i = n - 1; i >= 0; i--) {
        float sum = x[i];
        float diag = A[(size_t)i + (size_t)i * (size_t)lda];
        if (diag == 0.0f) {
          return i + 1;
        }
        for (int k = i + 1; k < n; k++) {
          sum -= A[(size_t)i + (size_t)k * (size_t)lda] * x[k];
        }
        x[i] = sum / diag;
      }
    }
  }
  return 0;
}

static int psyche_cusolver_dpotrs_reference(char uplo, int n, int nrhs, const double *A, int lda, double *B, int ldb) {
  for (int rhs = 0; rhs < nrhs; rhs++) {
    double *x = B + (size_t)rhs * (size_t)ldb;
    if (uplo == 'L') {
      for (int i = 0; i < n; i++) {
        double sum = x[i];
        double diag = A[(size_t)i + (size_t)i * (size_t)lda];
        if (diag == 0.0) {
          return i + 1;
        }
        for (int k = 0; k < i; k++) {
          sum -= A[(size_t)i + (size_t)k * (size_t)lda] * x[k];
        }
        x[i] = sum / diag;
      }
      for (int i = n - 1; i >= 0; i--) {
        double sum = x[i];
        double diag = A[(size_t)i + (size_t)i * (size_t)lda];
        if (diag == 0.0) {
          return i + 1;
        }
        for (int k = i + 1; k < n; k++) {
          sum -= A[(size_t)k + (size_t)i * (size_t)lda] * x[k];
        }
        x[i] = sum / diag;
      }
    } else {
      for (int i = 0; i < n; i++) {
        double sum = x[i];
        double diag = A[(size_t)i + (size_t)i * (size_t)lda];
        if (diag == 0.0) {
          return i + 1;
        }
        for (int k = 0; k < i; k++) {
          sum -= A[(size_t)k + (size_t)i * (size_t)lda] * x[k];
        }
        x[i] = sum / diag;
      }
      for (int i = n - 1; i >= 0; i--) {
        double sum = x[i];
        double diag = A[(size_t)i + (size_t)i * (size_t)lda];
        if (diag == 0.0) {
          return i + 1;
        }
        for (int k = i + 1; k < n; k++) {
          sum -= A[(size_t)i + (size_t)k * (size_t)lda] * x[k];
        }
        x[i] = sum / diag;
      }
    }
  }
  return 0;
}

static int psyche_cusolver_spotri_reference(char uplo, int n, float *A, int lda, float *Workspace) {
  int info = 0;
  if (n == 0) {
    return 0;
  }
  memset(Workspace, 0, (size_t)n * (size_t)n * sizeof(*Workspace));
  for (int i = 0; i < n; i++) {
    Workspace[(size_t)i + (size_t)i * (size_t)n] = 1.0f;
  }
  info = psyche_cusolver_spotrs_reference(uplo, n, n, A, lda, Workspace, n);
  if (info != 0) {
    return info;
  }
  /*
   * POTRI writes only the requested inverse triangle. The full identity-solve
   * workspace may have tiny asymmetric roundoff; copying one triangle also
   * preserves the caller's opposite-triangle factor bytes.
   */
  for (int col = 0; col < n; col++) {
    for (int row = 0; row < n; row++) {
      if ((uplo == 'L' && row >= col) || (uplo == 'U' && row <= col)) {
        A[(size_t)row + (size_t)col * (size_t)lda] = Workspace[(size_t)row + (size_t)col * (size_t)n];
      }
    }
  }
  return 0;
}

static int psyche_cusolver_dpotri_reference(char uplo, int n, double *A, int lda, double *Workspace) {
  int info = 0;
  if (n == 0) {
    return 0;
  }
  memset(Workspace, 0, (size_t)n * (size_t)n * sizeof(*Workspace));
  for (int i = 0; i < n; i++) {
    Workspace[(size_t)i + (size_t)i * (size_t)n] = 1.0;
  }
  info = psyche_cusolver_dpotrs_reference(uplo, n, n, A, lda, Workspace, n);
  if (info != 0) {
    return info;
  }
  /*
   * POTRI writes only the requested inverse triangle. The full identity-solve
   * workspace may have tiny asymmetric roundoff; copying one triangle also
   * preserves the caller's opposite-triangle factor bytes.
   */
  for (int col = 0; col < n; col++) {
    for (int row = 0; row < n; row++) {
      if ((uplo == 'L' && row >= col) || (uplo == 'U' && row <= col)) {
        A[(size_t)row + (size_t)col * (size_t)lda] = Workspace[(size_t)row + (size_t)col * (size_t)n];
      }
    }
  }
  return 0;
}
#endif

static int psyche_cusolver_scholesky_zero_diagonal(char uplo, int n, const float *A, int lda) {
  /* The Cholesky diagonal is shared by lower and upper storage. */
  (void)uplo;
  for (int i = 0; i < n; i++) {
    if (A[(size_t)i + (size_t)i * (size_t)lda] == 0.0f) {
      return i + 1;
    }
  }
  return 0;
}

static int psyche_cusolver_dcholesky_zero_diagonal(char uplo, int n, const double *A, int lda) {
  /* The Cholesky diagonal is shared by lower and upper storage. */
  (void)uplo;
  for (int i = 0; i < n; i++) {
    if (A[(size_t)i + (size_t)i * (size_t)lda] == 0.0) {
      return i + 1;
    }
  }
  return 0;
}

static int psyche_cusolver_sgetrs_no_pivot(char trans, int n, int nrhs, const float *A, int lda, float *B, int ldb) {
  int rhs = 0;
  for (rhs = 0; rhs < nrhs; rhs++) {
    float *x = B + (size_t)rhs * (size_t)ldb;
    int i = 0;
    if (trans == 'N') {
      for (i = 0; i < n; i++) {
        int k = 0;
        float sum = x[i];
        for (k = 0; k < i; k++) {
          sum -= A[(size_t)i + (size_t)k * (size_t)lda] * x[k];
        }
        x[i] = sum;
      }
      for (i = n - 1; i >= 0; i--) {
        int k = 0;
        float pivot = A[(size_t)i + (size_t)i * (size_t)lda];
        float sum = x[i];
        if (pivot == 0.0f) {
          return i + 1;
        }
        for (k = i + 1; k < n; k++) {
          sum -= A[(size_t)i + (size_t)k * (size_t)lda] * x[k];
        }
        x[i] = sum / pivot;
      }
    } else if (trans == 'T' || trans == 'C') {
      /* S/D are real-valued, so conjugate transpose is identical to transpose. */
      for (i = 0; i < n; i++) {
        int k = 0;
        float pivot = A[(size_t)i + (size_t)i * (size_t)lda];
        float sum = x[i];
        if (pivot == 0.0f) {
          return i + 1;
        }
        for (k = 0; k < i; k++) {
          sum -= A[(size_t)k + (size_t)i * (size_t)lda] * x[k];
        }
        x[i] = sum / pivot;
      }
      for (i = n - 1; i >= 0; i--) {
        int k = 0;
        float sum = x[i];
        for (k = i + 1; k < n; k++) {
          sum -= A[(size_t)k + (size_t)i * (size_t)lda] * x[k];
        }
        x[i] = sum;
      }
    } else {
      return -2;
    }
  }
  return 0;
}

static int psyche_cusolver_dgetrs_no_pivot(char trans, int n, int nrhs, const double *A, int lda, double *B, int ldb) {
  int rhs = 0;
  for (rhs = 0; rhs < nrhs; rhs++) {
    double *x = B + (size_t)rhs * (size_t)ldb;
    int i = 0;
    if (trans == 'N') {
      for (i = 0; i < n; i++) {
        int k = 0;
        double sum = x[i];
        for (k = 0; k < i; k++) {
          sum -= A[(size_t)i + (size_t)k * (size_t)lda] * x[k];
        }
        x[i] = sum;
      }
      for (i = n - 1; i >= 0; i--) {
        int k = 0;
        double pivot = A[(size_t)i + (size_t)i * (size_t)lda];
        double sum = x[i];
        if (pivot == 0.0) {
          return i + 1;
        }
        for (k = i + 1; k < n; k++) {
          sum -= A[(size_t)i + (size_t)k * (size_t)lda] * x[k];
        }
        x[i] = sum / pivot;
      }
    } else if (trans == 'T' || trans == 'C') {
      /* S/D are real-valued, so conjugate transpose is identical to transpose. */
      for (i = 0; i < n; i++) {
        int k = 0;
        double pivot = A[(size_t)i + (size_t)i * (size_t)lda];
        double sum = x[i];
        if (pivot == 0.0) {
          return i + 1;
        }
        for (k = 0; k < i; k++) {
          sum -= A[(size_t)k + (size_t)i * (size_t)lda] * x[k];
        }
        x[i] = sum / pivot;
      }
      for (i = n - 1; i >= 0; i--) {
        int k = 0;
        double sum = x[i];
        for (k = i + 1; k < n; k++) {
          sum -= A[(size_t)k + (size_t)i * (size_t)lda] * x[k];
        }
        x[i] = sum;
      }
    } else {
      return -2;
    }
  }
  return 0;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnCreate(cusolverDnHandle_t *handle) {
  struct cusolverDnContext *ctx = 0;
  if (handle == 0) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  *handle = 0;
  if (!psyche_cusolver_simulated_memory_enabled()) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  ctx = (struct cusolverDnContext *)calloc(1, sizeof(*ctx));
  if (ctx == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  ctx->magic = PSYCHE_CUSOLVER_HANDLE_MAGIC;
  ctx->stream = 0;
  psyche_cusolver_register_handle(ctx);
  *handle = (cusolverDnHandle_t)ctx;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDestroy(cusolverDnHandle_t handle) {
  struct cusolverDnContext *ctx = psyche_cusolver_unregister_handle(handle);
  if (ctx == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  ctx->magic = 0;
  free(ctx);
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnGetVersion(cusolverDnHandle_t handle, int *version) {
  if (version == 0) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  *version = 0;
  if (psyche_cusolver_handle(handle) == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  *version = PSYCHE_CUSOLVER_VERSION;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnGetProperty(libraryPropertyType type, int *value) {
  if (value == 0) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  switch (type) {
  case MAJOR_VERSION:
  case MINOR_VERSION:
  case PATCH_LEVEL:
    *value = 0;
    return CUSOLVER_STATUS_SUCCESS;
  default:
    *value = 0;
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API const char *cusolverGetErrorName(cusolverStatus_t status) {
  switch (status) {
  case CUSOLVER_STATUS_SUCCESS: return "CUSOLVER_STATUS_SUCCESS";
  case CUSOLVER_STATUS_NOT_INITIALIZED: return "CUSOLVER_STATUS_NOT_INITIALIZED";
  case CUSOLVER_STATUS_ALLOC_FAILED: return "CUSOLVER_STATUS_ALLOC_FAILED";
  case CUSOLVER_STATUS_INVALID_VALUE: return "CUSOLVER_STATUS_INVALID_VALUE";
  case CUSOLVER_STATUS_ARCH_MISMATCH: return "CUSOLVER_STATUS_ARCH_MISMATCH";
  case CUSOLVER_STATUS_MAPPING_ERROR: return "CUSOLVER_STATUS_MAPPING_ERROR";
  case CUSOLVER_STATUS_EXECUTION_FAILED: return "CUSOLVER_STATUS_EXECUTION_FAILED";
  case CUSOLVER_STATUS_INTERNAL_ERROR: return "CUSOLVER_STATUS_INTERNAL_ERROR";
  case CUSOLVER_STATUS_MATRIX_TYPE_NOT_SUPPORTED: return "CUSOLVER_STATUS_MATRIX_TYPE_NOT_SUPPORTED";
  case CUSOLVER_STATUS_NOT_SUPPORTED: return "CUSOLVER_STATUS_NOT_SUPPORTED";
  case CUSOLVER_STATUS_ZERO_PIVOT: return "CUSOLVER_STATUS_ZERO_PIVOT";
  case CUSOLVER_STATUS_INVALID_LICENSE: return "CUSOLVER_STATUS_INVALID_LICENSE";
  case CUSOLVER_STATUS_INVALID_WORKSPACE: return "CUSOLVER_STATUS_INVALID_WORKSPACE";
  default: return "CUSOLVER_STATUS_UNKNOWN";
  }
}

PSYCHE_CUDA_STUB_API const char *cusolverGetErrorString(cusolverStatus_t status) {
  switch (status) {
  case CUSOLVER_STATUS_SUCCESS: return "operation completed successfully";
  case CUSOLVER_STATUS_NOT_INITIALIZED: return "cuSOLVER shim was not initialized";
  case CUSOLVER_STATUS_ALLOC_FAILED: return "resource allocation failed";
  case CUSOLVER_STATUS_INVALID_VALUE: return "invalid value";
  case CUSOLVER_STATUS_ARCH_MISMATCH: return "architecture mismatch";
  case CUSOLVER_STATUS_MAPPING_ERROR: return "memory mapping error";
  case CUSOLVER_STATUS_EXECUTION_FAILED: return "execution failed";
  case CUSOLVER_STATUS_INTERNAL_ERROR: return "internal error";
  case CUSOLVER_STATUS_MATRIX_TYPE_NOT_SUPPORTED: return "matrix type not supported";
  case CUSOLVER_STATUS_NOT_SUPPORTED: return "operation not supported by this Apple Silicon compatibility slice";
  case CUSOLVER_STATUS_ZERO_PIVOT: return "zero pivot";
  case CUSOLVER_STATUS_INVALID_LICENSE: return "license error";
  case CUSOLVER_STATUS_INVALID_WORKSPACE: return "invalid workspace";
  default: return "unrecognized error code";
  }
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSetStream(cusolverDnHandle_t handle, void *streamId) {
  struct cusolverDnContext *ctx = psyche_cusolver_handle(handle);
  if (ctx == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  ctx->stream = streamId;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnGetStream(cusolverDnHandle_t handle, void **streamId) {
  struct cusolverDnContext *ctx = psyche_cusolver_handle(handle);
  if (streamId == 0) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  *streamId = 0;
  if (ctx == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  *streamId = ctx->stream;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSgetrf_bufferSize(
    cusolverDnHandle_t handle,
    int m,
    int n,
    float *A,
    int lda,
    int *Lwork) {
  (void)A;
  if (!psyche_cusolver_simulated_memory_enabled()) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (psyche_cusolver_handle(handle) == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (m < 0 || n < 0 || lda < 1 || (m > 0 && lda < m) || Lwork == 0) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (m != 0 && n > INT_MAX / m) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  *Lwork = m * n;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDgetrf_bufferSize(
    cusolverDnHandle_t handle,
    int m,
    int n,
    double *A,
    int lda,
    int *Lwork) {
  (void)A;
  if (!psyche_cusolver_simulated_memory_enabled()) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (psyche_cusolver_handle(handle) == 0) {
    return CUSOLVER_STATUS_NOT_INITIALIZED;
  }
  if (m < 0 || n < 0 || lda < 1 || (m > 0 && lda < m) || Lwork == 0) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (m != 0 && n > INT_MAX / m) {
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  *Lwork = m * n;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSgetrf(
    cusolverDnHandle_t handle,
    int m,
    int n,
    float *A,
    int lda,
    float *Workspace,
    int *devIpiv,
    int *devInfo) {
  cusolverStatus_t status = CUSOLVER_STATUS_SUCCESS;
  size_t a_bytes = 0;
  int lwork = 0;
  float *a_copy = 0;
  psyche_cusolver_lapack_int *ipiv_copy = 0;
  int info = 0;
  status = psyche_cusolver_validate_getrf(handle, m, n, A, lda, Workspace, devIpiv, devInfo, sizeof(float), &a_bytes, &lwork);
  if (status != CUSOLVER_STATUS_SUCCESS) {
    return status;
  }
  *devInfo = 0;
  if (a_bytes == 0) {
    return CUSOLVER_STATUS_SUCCESS;
  }
  a_copy = (float *)malloc(a_bytes);
  if (a_copy == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  memcpy(a_copy, A, a_bytes);
  if (devIpiv != 0) {
    int pivots = m < n ? m : n;
    ipiv_copy = (psyche_cusolver_lapack_int *)malloc((size_t)pivots * sizeof(*ipiv_copy));
    if (ipiv_copy == 0) {
      free(a_copy);
      return CUSOLVER_STATUS_ALLOC_FAILED;
    }
#if defined(__APPLE__)
    {
      __LAPACK_int lm = (__LAPACK_int)m;
      __LAPACK_int ln = (__LAPACK_int)n;
      __LAPACK_int llda = (__LAPACK_int)lda;
      __LAPACK_int linfo = 0;
      sgetrf_(&lm, &ln, a_copy, &llda, ipiv_copy, &linfo);
      info = (int)linfo;
    }
#else
    info = psyche_cusolver_sgetrf_pivot_reference(m, n, a_copy, lda, ipiv_copy);
#endif
    if (info < 0) {
      free(ipiv_copy);
      free(a_copy);
      *devInfo = info;
      return CUSOLVER_STATUS_INVALID_VALUE;
    }
    memcpy(A, a_copy, a_bytes);
    for (int i = 0; i < pivots; i++) {
      devIpiv[i] = (int)ipiv_copy[i];
    }
    free(ipiv_copy);
  } else {
    (void)lwork;
    info = psyche_cusolver_sgetrf_no_pivot(m, n, a_copy, lda);
    memcpy(A, a_copy, a_bytes);
  }
  free(a_copy);
  *devInfo = info;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDgetrf(
    cusolverDnHandle_t handle,
    int m,
    int n,
    double *A,
    int lda,
    double *Workspace,
    int *devIpiv,
    int *devInfo) {
  cusolverStatus_t status = CUSOLVER_STATUS_SUCCESS;
  size_t a_bytes = 0;
  int lwork = 0;
  double *a_copy = 0;
  psyche_cusolver_lapack_int *ipiv_copy = 0;
  int info = 0;
  status = psyche_cusolver_validate_getrf(handle, m, n, A, lda, Workspace, devIpiv, devInfo, sizeof(double), &a_bytes, &lwork);
  if (status != CUSOLVER_STATUS_SUCCESS) {
    return status;
  }
  *devInfo = 0;
  if (a_bytes == 0) {
    return CUSOLVER_STATUS_SUCCESS;
  }
  a_copy = (double *)malloc(a_bytes);
  if (a_copy == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  memcpy(a_copy, A, a_bytes);
  if (devIpiv != 0) {
    int pivots = m < n ? m : n;
    ipiv_copy = (psyche_cusolver_lapack_int *)malloc((size_t)pivots * sizeof(*ipiv_copy));
    if (ipiv_copy == 0) {
      free(a_copy);
      return CUSOLVER_STATUS_ALLOC_FAILED;
    }
#if defined(__APPLE__)
    {
      __LAPACK_int lm = (__LAPACK_int)m;
      __LAPACK_int ln = (__LAPACK_int)n;
      __LAPACK_int llda = (__LAPACK_int)lda;
      __LAPACK_int linfo = 0;
      dgetrf_(&lm, &ln, a_copy, &llda, ipiv_copy, &linfo);
      info = (int)linfo;
    }
#else
    info = psyche_cusolver_dgetrf_pivot_reference(m, n, a_copy, lda, ipiv_copy);
#endif
    if (info < 0) {
      free(ipiv_copy);
      free(a_copy);
      *devInfo = info;
      return CUSOLVER_STATUS_INVALID_VALUE;
    }
    memcpy(A, a_copy, a_bytes);
    for (int i = 0; i < pivots; i++) {
      devIpiv[i] = (int)ipiv_copy[i];
    }
    free(ipiv_copy);
  } else {
    (void)lwork;
    info = psyche_cusolver_dgetrf_no_pivot(m, n, a_copy, lda);
    memcpy(A, a_copy, a_bytes);
  }
  free(a_copy);
  *devInfo = info;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSgetrs(
    cusolverDnHandle_t handle,
    cublasOperation_t trans,
    int n,
    int nrhs,
    const float *A,
    int lda,
    const int *devIpiv,
    float *B,
    int ldb,
    int *devInfo) {
  cusolverStatus_t status = CUSOLVER_STATUS_SUCCESS;
  size_t a_bytes = 0;
  size_t b_bytes = 0;
  float *b_copy = 0;
  psyche_cusolver_lapack_int *ipiv_copy = 0;
  char lapack_trans = 'N';
  int info = 0;
  status = psyche_cusolver_validate_getrs(handle, trans, n, nrhs, A, lda, devIpiv, B, ldb, devInfo, sizeof(float), &a_bytes, &b_bytes, &lapack_trans);
  (void)a_bytes;
  if (status != CUSOLVER_STATUS_SUCCESS) {
    return status;
  }
  *devInfo = 0;
  if (b_bytes == 0) {
    return CUSOLVER_STATUS_SUCCESS;
  }
  b_copy = (float *)malloc(b_bytes);
  if (b_copy == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  memcpy(b_copy, B, b_bytes);
  if (devIpiv != 0) {
    ipiv_copy = (psyche_cusolver_lapack_int *)malloc((size_t)n * sizeof(*ipiv_copy));
    if (ipiv_copy == 0) {
      free(b_copy);
      return CUSOLVER_STATUS_ALLOC_FAILED;
    }
    for (int i = 0; i < n; i++) {
      ipiv_copy[i] = (psyche_cusolver_lapack_int)devIpiv[i];
    }
#if defined(__APPLE__)
    __LAPACK_int ln = (__LAPACK_int)n;
    __LAPACK_int lnrhs = (__LAPACK_int)nrhs;
    __LAPACK_int llda = (__LAPACK_int)lda;
    __LAPACK_int lldb = (__LAPACK_int)ldb;
    __LAPACK_int linfo = 0;
    sgetrs_(&lapack_trans, &ln, &lnrhs, A, &llda, ipiv_copy, b_copy, &lldb, &linfo);
    info = (int)linfo;
#else
    info = psyche_cusolver_sgetrs_pivot_reference(lapack_trans, n, nrhs, A, lda, ipiv_copy, b_copy, ldb);
#endif
    free(ipiv_copy);
  } else {
    info = psyche_cusolver_sgetrs_no_pivot(lapack_trans, n, nrhs, A, lda, b_copy, ldb);
  }
  if (info < 0) {
    free(b_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (info > 0) {
    free(b_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_EXECUTION_FAILED;
  }
  memcpy(B, b_copy, b_bytes);
  free(b_copy);
  *devInfo = info;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDgetrs(
    cusolverDnHandle_t handle,
    cublasOperation_t trans,
    int n,
    int nrhs,
    const double *A,
    int lda,
    const int *devIpiv,
    double *B,
    int ldb,
    int *devInfo) {
  cusolverStatus_t status = CUSOLVER_STATUS_SUCCESS;
  size_t a_bytes = 0;
  size_t b_bytes = 0;
  double *b_copy = 0;
  psyche_cusolver_lapack_int *ipiv_copy = 0;
  char lapack_trans = 'N';
  int info = 0;
  status = psyche_cusolver_validate_getrs(handle, trans, n, nrhs, A, lda, devIpiv, B, ldb, devInfo, sizeof(double), &a_bytes, &b_bytes, &lapack_trans);
  (void)a_bytes;
  if (status != CUSOLVER_STATUS_SUCCESS) {
    return status;
  }
  *devInfo = 0;
  if (b_bytes == 0) {
    return CUSOLVER_STATUS_SUCCESS;
  }
  b_copy = (double *)malloc(b_bytes);
  if (b_copy == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  memcpy(b_copy, B, b_bytes);
  if (devIpiv != 0) {
    ipiv_copy = (psyche_cusolver_lapack_int *)malloc((size_t)n * sizeof(*ipiv_copy));
    if (ipiv_copy == 0) {
      free(b_copy);
      return CUSOLVER_STATUS_ALLOC_FAILED;
    }
    for (int i = 0; i < n; i++) {
      ipiv_copy[i] = (psyche_cusolver_lapack_int)devIpiv[i];
    }
#if defined(__APPLE__)
    __LAPACK_int ln = (__LAPACK_int)n;
    __LAPACK_int lnrhs = (__LAPACK_int)nrhs;
    __LAPACK_int llda = (__LAPACK_int)lda;
    __LAPACK_int lldb = (__LAPACK_int)ldb;
    __LAPACK_int linfo = 0;
    dgetrs_(&lapack_trans, &ln, &lnrhs, A, &llda, ipiv_copy, b_copy, &lldb, &linfo);
    info = (int)linfo;
#else
    info = psyche_cusolver_dgetrs_pivot_reference(lapack_trans, n, nrhs, A, lda, ipiv_copy, b_copy, ldb);
#endif
    free(ipiv_copy);
  } else {
    info = psyche_cusolver_dgetrs_no_pivot(lapack_trans, n, nrhs, A, lda, b_copy, ldb);
  }
  if (info < 0) {
    free(b_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (info > 0) {
    free(b_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_EXECUTION_FAILED;
  }
  memcpy(B, b_copy, b_bytes);
  free(b_copy);
  *devInfo = info;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSpotrf_bufferSize(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    float *A,
    int lda,
    int *Lwork) {
  (void)A;
  return psyche_cusolver_validate_potrf_buffer_size(handle, uplo, n, lda, Lwork);
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDpotrf_bufferSize(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    double *A,
    int lda,
    int *Lwork) {
  (void)A;
  return psyche_cusolver_validate_potrf_buffer_size(handle, uplo, n, lda, Lwork);
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSpotrf(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    float *A,
    int lda,
    float *Workspace,
    int Lwork,
    int *devInfo) {
  cusolverStatus_t status = CUSOLVER_STATUS_SUCCESS;
  size_t a_bytes = 0;
  float *a_copy = 0;
  char lapack_uplo = 'L';
  int info = 0;
  status = psyche_cusolver_validate_potrf(handle, uplo, n, A, lda, Workspace, Lwork, devInfo, sizeof(float), &a_bytes, 0, &lapack_uplo);
  if (status != CUSOLVER_STATUS_SUCCESS) {
    return status;
  }
  *devInfo = 0;
  if (a_bytes == 0) {
    return CUSOLVER_STATUS_SUCCESS;
  }
  a_copy = (float *)malloc(a_bytes);
  if (a_copy == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  memcpy(a_copy, A, a_bytes);
#if defined(__APPLE__)
  {
    __LAPACK_int ln = (__LAPACK_int)n;
    __LAPACK_int llda = (__LAPACK_int)lda;
    __LAPACK_int linfo = 0;
    /*
     * cuSOLVER requires caller workspace; Accelerate's legacy LAPACK entry
     * does not consume it, so validation above enforces the shim contract.
     */
    spotrf_(&lapack_uplo, &ln, a_copy, &llda, &linfo);
    info = (int)linfo;
  }
#else
  (void)Workspace;
  (void)Lwork;
  info = psyche_cusolver_spotrf_reference(lapack_uplo, n, a_copy, lda);
#endif
  if (info < 0) {
    free(a_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (info > 0) {
    free(a_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_SUCCESS;
  }
  memcpy(A, a_copy, a_bytes);
  free(a_copy);
  *devInfo = info;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDpotrf(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    double *A,
    int lda,
    double *Workspace,
    int Lwork,
    int *devInfo) {
  cusolverStatus_t status = CUSOLVER_STATUS_SUCCESS;
  size_t a_bytes = 0;
  double *a_copy = 0;
  char lapack_uplo = 'L';
  int info = 0;
  status = psyche_cusolver_validate_potrf(handle, uplo, n, A, lda, Workspace, Lwork, devInfo, sizeof(double), &a_bytes, 0, &lapack_uplo);
  if (status != CUSOLVER_STATUS_SUCCESS) {
    return status;
  }
  *devInfo = 0;
  if (a_bytes == 0) {
    return CUSOLVER_STATUS_SUCCESS;
  }
  a_copy = (double *)malloc(a_bytes);
  if (a_copy == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  memcpy(a_copy, A, a_bytes);
#if defined(__APPLE__)
  {
    __LAPACK_int ln = (__LAPACK_int)n;
    __LAPACK_int llda = (__LAPACK_int)lda;
    __LAPACK_int linfo = 0;
    /*
     * cuSOLVER requires caller workspace; Accelerate's legacy LAPACK entry
     * does not consume it, so validation above enforces the shim contract.
     */
    dpotrf_(&lapack_uplo, &ln, a_copy, &llda, &linfo);
    info = (int)linfo;
  }
#else
  (void)Workspace;
  (void)Lwork;
  info = psyche_cusolver_dpotrf_reference(lapack_uplo, n, a_copy, lda);
#endif
  if (info < 0) {
    free(a_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (info > 0) {
    free(a_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_SUCCESS;
  }
  memcpy(A, a_copy, a_bytes);
  free(a_copy);
  *devInfo = info;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSpotri_bufferSize(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    float *A,
    int lda,
    int *Lwork) {
  (void)A;
  return psyche_cusolver_validate_potri_buffer_size(handle, uplo, n, lda, Lwork);
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDpotri_bufferSize(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    double *A,
    int lda,
    int *Lwork) {
  (void)A;
  return psyche_cusolver_validate_potri_buffer_size(handle, uplo, n, lda, Lwork);
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSpotri(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    float *A,
    int lda,
    float *Workspace,
    int Lwork,
    int *devInfo) {
  cusolverStatus_t status = CUSOLVER_STATUS_SUCCESS;
  size_t a_bytes = 0;
  float *a_copy = 0;
  char lapack_uplo = 'L';
  int info = 0;
  status = psyche_cusolver_validate_potri(handle, uplo, n, A, lda, Workspace, Lwork, devInfo, sizeof(float), &a_bytes, &lapack_uplo);
  if (status != CUSOLVER_STATUS_SUCCESS) {
    return status;
  }
  *devInfo = 0;
  if (a_bytes == 0) {
    return CUSOLVER_STATUS_SUCCESS;
  }
  info = psyche_cusolver_scholesky_zero_diagonal(lapack_uplo, n, A, lda);
  if (info > 0) {
    *devInfo = info;
    return CUSOLVER_STATUS_SUCCESS;
  }
  a_copy = (float *)malloc(a_bytes);
  if (a_copy == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  memcpy(a_copy, A, a_bytes);
#if defined(__APPLE__)
  {
    __LAPACK_int ln = (__LAPACK_int)n;
    __LAPACK_int llda = (__LAPACK_int)lda;
    __LAPACK_int linfo = 0;
    /*
     * cuSOLVER requires caller workspace; Accelerate's legacy LAPACK POTRI
     * entry does not consume it, so validation above enforces the shim contract.
     */
    spotri_(&lapack_uplo, &ln, a_copy, &llda, &linfo);
    info = (int)linfo;
  }
#else
  info = psyche_cusolver_spotri_reference(lapack_uplo, n, a_copy, lda, Workspace);
#endif
  if (info < 0) {
    free(a_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (info > 0) {
    free(a_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_SUCCESS;
  }
  for (int col = 0; col < n; col++) {
    for (int row = 0; row < n; row++) {
      if ((lapack_uplo == 'L' && row >= col) || (lapack_uplo == 'U' && row <= col)) {
        A[(size_t)row + (size_t)col * (size_t)lda] = a_copy[(size_t)row + (size_t)col * (size_t)lda];
      }
    }
  }
  free(a_copy);
  *devInfo = info;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDpotri(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    double *A,
    int lda,
    double *Workspace,
    int Lwork,
    int *devInfo) {
  cusolverStatus_t status = CUSOLVER_STATUS_SUCCESS;
  size_t a_bytes = 0;
  double *a_copy = 0;
  char lapack_uplo = 'L';
  int info = 0;
  status = psyche_cusolver_validate_potri(handle, uplo, n, A, lda, Workspace, Lwork, devInfo, sizeof(double), &a_bytes, &lapack_uplo);
  if (status != CUSOLVER_STATUS_SUCCESS) {
    return status;
  }
  *devInfo = 0;
  if (a_bytes == 0) {
    return CUSOLVER_STATUS_SUCCESS;
  }
  info = psyche_cusolver_dcholesky_zero_diagonal(lapack_uplo, n, A, lda);
  if (info > 0) {
    *devInfo = info;
    return CUSOLVER_STATUS_SUCCESS;
  }
  a_copy = (double *)malloc(a_bytes);
  if (a_copy == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  memcpy(a_copy, A, a_bytes);
#if defined(__APPLE__)
  {
    __LAPACK_int ln = (__LAPACK_int)n;
    __LAPACK_int llda = (__LAPACK_int)lda;
    __LAPACK_int linfo = 0;
    /*
     * cuSOLVER requires caller workspace; Accelerate's legacy LAPACK POTRI
     * entry does not consume it, so validation above enforces the shim contract.
     */
    dpotri_(&lapack_uplo, &ln, a_copy, &llda, &linfo);
    info = (int)linfo;
  }
#else
  info = psyche_cusolver_dpotri_reference(lapack_uplo, n, a_copy, lda, Workspace);
#endif
  if (info < 0) {
    free(a_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (info > 0) {
    free(a_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_SUCCESS;
  }
  for (int col = 0; col < n; col++) {
    for (int row = 0; row < n; row++) {
      if ((lapack_uplo == 'L' && row >= col) || (lapack_uplo == 'U' && row <= col)) {
        A[(size_t)row + (size_t)col * (size_t)lda] = a_copy[(size_t)row + (size_t)col * (size_t)lda];
      }
    }
  }
  free(a_copy);
  *devInfo = info;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSpotrs(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    int nrhs,
    const float *A,
    int lda,
    float *B,
    int ldb,
    int *devInfo) {
  cusolverStatus_t status = CUSOLVER_STATUS_SUCCESS;
  size_t a_bytes = 0;
  size_t b_bytes = 0;
  float *b_copy = 0;
  char lapack_uplo = 'L';
  int info = 0;
  status = psyche_cusolver_validate_potrs(handle, uplo, n, nrhs, A, lda, B, ldb, devInfo, sizeof(float), &a_bytes, &b_bytes, &lapack_uplo);
  (void)a_bytes;
  if (status != CUSOLVER_STATUS_SUCCESS) {
    return status;
  }
  *devInfo = 0;
  if (b_bytes == 0) {
    return CUSOLVER_STATUS_SUCCESS;
  }
  info = psyche_cusolver_scholesky_zero_diagonal(lapack_uplo, n, A, lda);
  if (info > 0) {
    *devInfo = info;
    return CUSOLVER_STATUS_EXECUTION_FAILED;
  }
  b_copy = (float *)malloc(b_bytes);
  if (b_copy == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  memcpy(b_copy, B, b_bytes);
#if defined(__APPLE__)
  {
    __LAPACK_int ln = (__LAPACK_int)n;
    __LAPACK_int lnrhs = (__LAPACK_int)nrhs;
    __LAPACK_int llda = (__LAPACK_int)lda;
    __LAPACK_int lldb = (__LAPACK_int)ldb;
    __LAPACK_int linfo = 0;
    /* Accelerate's Fortran ABI omits const; POTRS does not modify A. */
    spotrs_(&lapack_uplo, &ln, &lnrhs, (float *)A, &llda, b_copy, &lldb, &linfo);
    info = (int)linfo;
  }
#else
  info = psyche_cusolver_spotrs_reference(lapack_uplo, n, nrhs, A, lda, b_copy, ldb);
#endif
  if (info < 0) {
    free(b_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (info > 0) {
    free(b_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_EXECUTION_FAILED;
  }
  memcpy(B, b_copy, b_bytes);
  free(b_copy);
  *devInfo = info;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDpotrs(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    int nrhs,
    const double *A,
    int lda,
    double *B,
    int ldb,
    int *devInfo) {
  cusolverStatus_t status = CUSOLVER_STATUS_SUCCESS;
  size_t a_bytes = 0;
  size_t b_bytes = 0;
  double *b_copy = 0;
  char lapack_uplo = 'L';
  int info = 0;
  status = psyche_cusolver_validate_potrs(handle, uplo, n, nrhs, A, lda, B, ldb, devInfo, sizeof(double), &a_bytes, &b_bytes, &lapack_uplo);
  (void)a_bytes;
  if (status != CUSOLVER_STATUS_SUCCESS) {
    return status;
  }
  *devInfo = 0;
  if (b_bytes == 0) {
    return CUSOLVER_STATUS_SUCCESS;
  }
  info = psyche_cusolver_dcholesky_zero_diagonal(lapack_uplo, n, A, lda);
  if (info > 0) {
    *devInfo = info;
    return CUSOLVER_STATUS_EXECUTION_FAILED;
  }
  b_copy = (double *)malloc(b_bytes);
  if (b_copy == 0) {
    return CUSOLVER_STATUS_ALLOC_FAILED;
  }
  memcpy(b_copy, B, b_bytes);
#if defined(__APPLE__)
  {
    __LAPACK_int ln = (__LAPACK_int)n;
    __LAPACK_int lnrhs = (__LAPACK_int)nrhs;
    __LAPACK_int llda = (__LAPACK_int)lda;
    __LAPACK_int lldb = (__LAPACK_int)ldb;
    __LAPACK_int linfo = 0;
    /* Accelerate's Fortran ABI omits const; POTRS does not modify A. */
    dpotrs_(&lapack_uplo, &ln, &lnrhs, (double *)A, &llda, b_copy, &lldb, &linfo);
    info = (int)linfo;
  }
#else
  info = psyche_cusolver_dpotrs_reference(lapack_uplo, n, nrhs, A, lda, b_copy, ldb);
#endif
  if (info < 0) {
    free(b_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_INVALID_VALUE;
  }
  if (info > 0) {
    free(b_copy);
    *devInfo = info;
    return CUSOLVER_STATUS_EXECUTION_FAILED;
  }
  memcpy(B, b_copy, b_bytes);
  free(b_copy);
  *devInfo = info;
  return CUSOLVER_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API int psyche_cuda_compat_stub_is_stub(void) {
  return 1;
}

PSYCHE_CUDA_STUB_API const char *psyche_cuda_compat_stub_version(void) {
  return "psyche-cusolver-compat-stub/0.1";
}
