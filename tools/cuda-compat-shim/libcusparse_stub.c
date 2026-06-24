#define _POSIX_C_SOURCE 200809L

#include "cuda_compat_stub.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PSYCHE_CUSPARSE_OBJECT_MAGIC 0x7073796373707273ULL
#define PSYCHE_CUSPARSE_VERSION 0

typedef enum {
  PSYCHE_CUSPARSE_OBJECT_CONTEXT = 1,
  PSYCHE_CUSPARSE_OBJECT_SPMAT = 2,
  PSYCHE_CUSPARSE_OBJECT_DNVEC = 3,
  PSYCHE_CUSPARSE_OBJECT_DNMAT = 4
} PsycheCusparseObjectKind;

typedef struct PsycheCusparseObject {
  uint64_t magic;
  PsycheCusparseObjectKind kind;
  int destroyed;
  struct PsycheCusparseObject *next;
} PsycheCusparseObject;

struct cusparseContext {
  PsycheCusparseObject base;
  void *stream;
  cusparsePointerMode_t pointer_mode;
};

struct cusparseSpMatStruct {
  PsycheCusparseObject base;
  cusparseFormat_t format;
  int64_t rows;
  int64_t cols;
  int64_t nnz;
  void *row_offsets;
  void *col_indices;
  void *values;
  cusparseIndexType_t row_offsets_type;
  cusparseIndexType_t col_indices_type;
  cusparseIndexBase_t index_base;
  cudaDataType value_type;
};

struct cusparseDnVecStruct {
  PsycheCusparseObject base;
  int64_t size;
  void *values;
  cudaDataType value_type;
};

struct cusparseDnMatStruct {
  PsycheCusparseObject base;
  int64_t rows;
  int64_t cols;
  int64_t ld;
  void *values;
  cudaDataType value_type;
  cusparseOrder_t order;
};

typedef struct {
  const void *row_offsets;
  const void *col_indices;
  const float *values;
  const float *x;
  float *y;
  float alpha;
  float beta;
  int64_t rows;
  int64_t cols;
  int64_t nnz;
  int index_base;
  cusparseIndexType_t index_type;
  size_t row_offsets_bytes;
  size_t col_indices_bytes;
  size_t values_bytes;
  size_t x_bytes;
  size_t y_bytes;
  unsigned int rows_u;
  unsigned int cols_u;
  unsigned int nnz_u;
} PsycheCusparseSpMVPlan;

typedef struct {
  const void *row_offsets;
  const void *col_indices;
  const float *values;
  const float *b;
  float *c;
  float alpha;
  float beta;
  int64_t rows;
  int64_t cols;
  int64_t nnz;
  int64_t n;
  int64_t b_ld;
  int64_t c_ld;
  int index_base;
  cusparseIndexType_t index_type;
  cusparseOrder_t b_order;
  cusparseOrder_t c_order;
  size_t row_offsets_bytes;
  size_t col_indices_bytes;
  size_t values_bytes;
  size_t b_bytes;
  size_t c_bytes;
  size_t output_elements;
  size_t output_bytes;
} PsycheCusparseSpMMPlan;

static pthread_mutex_t psyche_cusparse_object_mutex = PTHREAD_MUTEX_INITIALIZER;
static PsycheCusparseObject *psyche_cusparse_objects = 0;

#if defined(__APPLE__)
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
    unsigned int blockDimX);
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
    unsigned int blockDimX);
#endif

static int psyche_cusparse_env_truthy(const char *value) {
  if (value == 0 || value[0] == '\0') {
    return 0;
  }
  return
      strcmp(value, "1") == 0 ||
      strcasecmp(value, "true") == 0 ||
      strcasecmp(value, "yes") == 0 ||
      strcasecmp(value, "on") == 0;
}

static int psyche_cusparse_env_required(const char *value) {
  return value != 0 && strcasecmp(value, "required") == 0;
}

static int psyche_cusparse_simulated_memory_enabled(void) {
  return psyche_cusparse_env_truthy(getenv("PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY"));
}

static int psyche_cusparse_metal_enabled(void) {
  const char *value = getenv("PSYCHE_CUDA_COMPAT_CUSPARSE_METAL");
  return psyche_cusparse_env_truthy(value) || psyche_cusparse_env_required(value);
}

static int psyche_cusparse_metal_required(void) {
  return psyche_cusparse_env_required(getenv("PSYCHE_CUDA_COMPAT_CUSPARSE_METAL"));
}

static int psyche_cusparse_metal_backend_disabled_for_test(void) {
  const char *value = getenv("PSYCHE_CUDA_COMPAT_METAL_DISABLE_BACKEND_FOR_TEST");
  return value != 0 && value[0] != '\0' && strcasecmp(value, "0") != 0;
}

static int psyche_cusparse_metal_preferred_can_fallback(CUresult result) {
  return
      result == CUDA_ERROR_NOT_SUPPORTED ||
      result == CUDA_ERROR_NO_DEVICE ||
      result == CUDA_ERROR_NOT_INITIALIZED;
}

static cusparseStatus_t psyche_cusparse_status_from_cuda_result(CUresult result) {
  switch (result) {
  case CUDA_SUCCESS:
    return CUSPARSE_STATUS_SUCCESS;
  case CUDA_ERROR_OUT_OF_MEMORY:
    return CUSPARSE_STATUS_ALLOC_FAILED;
  case CUDA_ERROR_INVALID_VALUE:
  case CUDA_ERROR_INVALID_HANDLE:
    return CUSPARSE_STATUS_INVALID_VALUE;
  case CUDA_ERROR_NOT_INITIALIZED:
  case CUDA_ERROR_NO_DEVICE:
  case CUDA_ERROR_NOT_SUPPORTED:
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  case CUDA_ERROR_UNKNOWN:
    return CUSPARSE_STATUS_EXECUTION_FAILED;
  default:
    return CUSPARSE_STATUS_INTERNAL_ERROR;
  }
}

static int psyche_cusparse_aligned(const void *ptr, size_t alignment) {
  if (ptr == 0 || alignment == 0) {
    return 1;
  }
  return ((uintptr_t)ptr % alignment) == 0;
}

static int psyche_cusparse_ranges_overlap(
    const void *a,
    size_t a_bytes,
    const void *b,
    size_t b_bytes) {
  uintptr_t a_start;
  uintptr_t b_start;
  uintptr_t a_end;
  uintptr_t b_end;
  if (a == 0 || b == 0 || a_bytes == 0 || b_bytes == 0) {
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

static int psyche_cusparse_mul_size(size_t a, size_t b, size_t *out) {
  if (out == 0) {
    return 0;
  }
  if (a != 0 && b > SIZE_MAX / a) {
    return 0;
  }
  *out = a * b;
  return 1;
}

static size_t psyche_cusparse_index_size(cusparseIndexType_t type) {
  switch (type) {
  case CUSPARSE_INDEX_32I:
    return sizeof(int32_t);
  case CUSPARSE_INDEX_64I:
    return sizeof(int64_t);
  default:
    return 0;
  }
}

static int64_t psyche_cusparse_read_index(const void *values, cusparseIndexType_t type, int64_t index) {
  switch (type) {
  case CUSPARSE_INDEX_32I:
    return (int64_t)((const int32_t *)values)[index];
  case CUSPARSE_INDEX_64I:
    return ((const int64_t *)values)[index];
  default:
    return 0;
  }
}

static void psyche_cusparse_register_object(PsycheCusparseObject *object, PsycheCusparseObjectKind kind) {
  pthread_mutex_lock(&psyche_cusparse_object_mutex);
  object->magic = PSYCHE_CUSPARSE_OBJECT_MAGIC;
  object->kind = kind;
  object->destroyed = 0;
  object->next = psyche_cusparse_objects;
  psyche_cusparse_objects = object;
  pthread_mutex_unlock(&psyche_cusparse_object_mutex);
}

static PsycheCusparseObject *psyche_cusparse_find_object(const void *handle, PsycheCusparseObjectKind kind) {
  PsycheCusparseObject *object = 0;
  if (handle == 0) {
    return 0;
  }
  pthread_mutex_lock(&psyche_cusparse_object_mutex);
  object = psyche_cusparse_objects;
  while (object != 0) {
    if ((const void *)object == handle) {
      break;
    }
    object = object->next;
  }
  if (
      object == 0 ||
      object->magic != PSYCHE_CUSPARSE_OBJECT_MAGIC ||
      object->kind != kind ||
      object->destroyed) {
    object = 0;
  }
  pthread_mutex_unlock(&psyche_cusparse_object_mutex);
  return object;
}

static int psyche_cusparse_destroy_object(const void *handle, PsycheCusparseObjectKind kind) {
  PsycheCusparseObject *object = 0;
  if (handle == 0) {
    return 0;
  }
  pthread_mutex_lock(&psyche_cusparse_object_mutex);
  object = psyche_cusparse_objects;
  while (object != 0) {
    if ((const void *)object == handle) {
      break;
    }
    object = object->next;
  }
  if (
      object == 0 ||
      object->magic != PSYCHE_CUSPARSE_OBJECT_MAGIC ||
      object->kind != kind ||
      object->destroyed) {
    pthread_mutex_unlock(&psyche_cusparse_object_mutex);
    return 0;
  }
  object->destroyed = 1;
  object->magic = 0;
  pthread_mutex_unlock(&psyche_cusparse_object_mutex);
  return 1;
}

static struct cusparseContext *psyche_cusparse_context(cusparseHandle_t handle) {
  return (struct cusparseContext *)psyche_cusparse_find_object(
      handle,
      PSYCHE_CUSPARSE_OBJECT_CONTEXT);
}

static struct cusparseSpMatStruct *psyche_cusparse_spmat(cusparseConstSpMatDescr_t descriptor) {
  return (struct cusparseSpMatStruct *)psyche_cusparse_find_object(
      descriptor,
      PSYCHE_CUSPARSE_OBJECT_SPMAT);
}

static struct cusparseDnVecStruct *psyche_cusparse_dnvec(cusparseConstDnVecDescr_t descriptor) {
  return (struct cusparseDnVecStruct *)psyche_cusparse_find_object(
      descriptor,
      PSYCHE_CUSPARSE_OBJECT_DNVEC);
}

static struct cusparseDnMatStruct *psyche_cusparse_dnmat(cusparseConstDnMatDescr_t descriptor) {
  return (struct cusparseDnMatStruct *)psyche_cusparse_find_object(
      descriptor,
      PSYCHE_CUSPARSE_OBJECT_DNMAT);
}

static int psyche_cusparse_valid_index_type(cusparseIndexType_t type) {
  return
      type == CUSPARSE_INDEX_16U ||
      type == CUSPARSE_INDEX_32I ||
      type == CUSPARSE_INDEX_64I;
}

static int psyche_cusparse_valid_order(cusparseOrder_t order) {
  return order == CUSPARSE_ORDER_COL || order == CUSPARSE_ORDER_ROW;
}

static int psyche_cusparse_valid_spmm_alg(cusparseSpMMAlg_t alg) {
  return
      alg == CUSPARSE_SPMM_ALG_DEFAULT ||
      alg == CUSPARSE_SPMM_CSR_ALG1 ||
      alg == CUSPARSE_SPMM_CSR_ALG2 ||
      alg == CUSPARSE_SPMM_CSR_ALG3;
}

static int psyche_cusparse_valid_data_type(cudaDataType type) {
  switch (type) {
  case CUDA_R_16F:
  case CUDA_R_16BF:
  case CUDA_R_32F:
  case CUDA_R_64F:
  case CUDA_C_32F:
  case CUDA_C_64F:
    return 1;
  default:
    return 0;
  }
}

static size_t psyche_cusparse_data_type_alignment(cudaDataType type) {
  switch (type) {
  case CUDA_R_16F:
  case CUDA_R_16BF:
    return 2u;
  case CUDA_R_32F:
    return 4u;
  case CUDA_R_64F:
  case CUDA_C_32F:
    return 8u;
  case CUDA_C_64F:
    return 16u;
  default:
    return 0u;
  }
}

static int psyche_cusparse_add_size(size_t a, size_t b, size_t *out) {
  if (out == 0) {
    return 0;
  }
  if (b > SIZE_MAX - a) {
    return 0;
  }
  *out = a + b;
  return 1;
}

static int psyche_cusparse_dnmat_element_span(
    int64_t rows,
    int64_t cols,
    int64_t ld,
    cusparseOrder_t order,
    size_t *elements) {
  size_t leading = 0;
  size_t major_minus_one = 0;
  size_t tail = 0;
  size_t prefix = 0;
  if (elements == 0) {
    return 0;
  }
  *elements = 0;
  if (rows < 0 || cols < 0 || ld < 0 || !psyche_cusparse_valid_order(order)) {
    return 0;
  }
  if (rows == 0 || cols == 0) {
    return 1;
  }
  if (order == CUSPARSE_ORDER_COL) {
    if (ld < rows) {
      return 0;
    }
    major_minus_one = (size_t)(cols - 1);
    tail = (size_t)rows;
  } else {
    if (ld < cols) {
      return 0;
    }
    major_minus_one = (size_t)(rows - 1);
    tail = (size_t)cols;
  }
  leading = (size_t)ld;
  if (
      !psyche_cusparse_mul_size(major_minus_one, leading, &prefix) ||
      !psyche_cusparse_add_size(prefix, tail, elements)) {
    return 0;
  }
  return 1;
}

static size_t psyche_cusparse_dnmat_index(
    int64_t row,
    int64_t col,
    int64_t ld,
    cusparseOrder_t order) {
  if (order == CUSPARSE_ORDER_COL) {
    return (size_t)row + (size_t)col * (size_t)ld;
  }
  return (size_t)row * (size_t)ld + (size_t)col;
}

static cusparseStatus_t psyche_cusparse_validate_spmv_plan(
    cusparseHandle_t handle,
    cusparseOperation_t opA,
    const void *alpha,
    cusparseConstSpMatDescr_t matA,
    cusparseConstDnVecDescr_t vecX,
    const void *beta,
    cusparseDnVecDescr_t vecY,
    cudaDataType computeType,
    cusparseSpMVAlg_t alg,
    PsycheCusparseSpMVPlan *plan,
    int validate_indices) {
  struct cusparseContext *ctx = psyche_cusparse_context(handle);
  struct cusparseSpMatStruct *matrix = psyche_cusparse_spmat(matA);
  struct cusparseDnVecStruct *x_vec = psyche_cusparse_dnvec(vecX);
  struct cusparseDnVecStruct *y_vec = psyche_cusparse_dnvec(vecY);
  int64_t expected_row_end = 0;
  int64_t previous = 0;
  int64_t row = 0;
  int64_t entry = 0;
  size_t rows_plus_one = 0;
  size_t row_offsets_bytes = 0;
  size_t col_indices_bytes = 0;
  size_t values_bytes = 0;
  size_t x_bytes = 0;
  size_t y_bytes = 0;
  size_t index_size = 0;
  float alpha_value = 0.0f;
  float beta_value = 0.0f;

  if (ctx == 0) {
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  if (ctx->pointer_mode != CUSPARSE_POINTER_MODE_HOST) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (alpha == 0 || beta == 0 || plan == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (!psyche_cusparse_aligned(alpha, sizeof(float)) || !psyche_cusparse_aligned(beta, sizeof(float))) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (matrix == 0 || x_vec == 0 || y_vec == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (opA != CUSPARSE_OPERATION_NON_TRANSPOSE) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (
      alg != CUSPARSE_SPMV_ALG_DEFAULT &&
      alg != CUSPARSE_SPMV_CSR_ALG1 &&
      alg != CUSPARSE_SPMV_CSR_ALG2) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (
      matrix->format != CUSPARSE_FORMAT_CSR ||
      matrix->value_type != CUDA_R_32F ||
      x_vec->value_type != CUDA_R_32F ||
      y_vec->value_type != CUDA_R_32F ||
      computeType != CUDA_R_32F) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (
      matrix->row_offsets_type != matrix->col_indices_type ||
      (matrix->row_offsets_type != CUSPARSE_INDEX_32I &&
       matrix->row_offsets_type != CUSPARSE_INDEX_64I)) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  index_size = psyche_cusparse_index_size(matrix->row_offsets_type);
  if (index_size == 0) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (matrix->index_base != CUSPARSE_INDEX_BASE_ZERO && matrix->index_base != CUSPARSE_INDEX_BASE_ONE) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (matrix->rows < 0 || matrix->cols < 0 || matrix->nnz < 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (x_vec->size < matrix->cols || y_vec->size < matrix->rows) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (matrix->rows == INT64_MAX) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  rows_plus_one = (size_t)matrix->rows + 1u;
  if (
      !psyche_cusparse_mul_size(rows_plus_one, index_size, &row_offsets_bytes) ||
      !psyche_cusparse_mul_size((size_t)matrix->nnz, index_size, &col_indices_bytes) ||
      !psyche_cusparse_mul_size((size_t)matrix->nnz, sizeof(float), &values_bytes) ||
      !psyche_cusparse_mul_size((size_t)matrix->cols, sizeof(float), &x_bytes) ||
      !psyche_cusparse_mul_size((size_t)matrix->rows, sizeof(float), &y_bytes)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (matrix->row_offsets == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (matrix->nnz != 0 && (matrix->col_indices == 0 || matrix->values == 0)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (matrix->cols != 0 && x_vec->values == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (matrix->rows != 0 && y_vec->values == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (
      !psyche_cusparse_aligned(matrix->row_offsets, index_size) ||
      !psyche_cusparse_aligned(matrix->col_indices, index_size) ||
      !psyche_cusparse_aligned(matrix->values, sizeof(float)) ||
      !psyche_cusparse_aligned(x_vec->values, sizeof(float)) ||
      !psyche_cusparse_aligned(y_vec->values, sizeof(float))) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (
      psyche_cusparse_ranges_overlap(y_vec->values, y_bytes, matrix->row_offsets, row_offsets_bytes) ||
      psyche_cusparse_ranges_overlap(y_vec->values, y_bytes, matrix->col_indices, col_indices_bytes) ||
      psyche_cusparse_ranges_overlap(y_vec->values, y_bytes, matrix->values, values_bytes) ||
      psyche_cusparse_ranges_overlap(y_vec->values, y_bytes, x_vec->values, x_bytes)) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }

  memset(plan, 0, sizeof(*plan));
  plan->row_offsets = matrix->row_offsets;
  plan->col_indices = matrix->col_indices;
  plan->values = (const float *)matrix->values;
  plan->x = (const float *)x_vec->values;
  plan->y = (float *)y_vec->values;
  plan->alpha = alpha_value;
  plan->beta = beta_value;
  plan->rows = matrix->rows;
  plan->cols = matrix->cols;
  plan->nnz = matrix->nnz;
  plan->index_base = matrix->index_base == CUSPARSE_INDEX_BASE_ONE ? 1 : 0;
  plan->index_type = matrix->row_offsets_type;
  plan->row_offsets_bytes = row_offsets_bytes;
  plan->col_indices_bytes = col_indices_bytes;
  plan->values_bytes = values_bytes;
  plan->x_bytes = x_bytes;
  plan->y_bytes = y_bytes;
  plan->rows_u = matrix->rows <= UINT_MAX ? (unsigned int)matrix->rows : 0u;
  plan->cols_u = matrix->cols <= UINT_MAX ? (unsigned int)matrix->cols : 0u;
  plan->nnz_u = matrix->nnz <= UINT_MAX ? (unsigned int)matrix->nnz : 0u;

  if (!validate_indices) {
    return CUSPARSE_STATUS_SUCCESS;
  }
  if (plan->index_base != 0 && matrix->nnz == INT64_MAX) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  expected_row_end = matrix->nnz + plan->index_base;
  previous = plan->index_base;
  if (
      psyche_cusparse_read_index(plan->row_offsets, plan->index_type, 0) != plan->index_base ||
      psyche_cusparse_read_index(plan->row_offsets, plan->index_type, matrix->rows) != expected_row_end) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  for (row = 0; row < matrix->rows; row++) {
    int64_t start = psyche_cusparse_read_index(plan->row_offsets, plan->index_type, row);
    int64_t end = psyche_cusparse_read_index(plan->row_offsets, plan->index_type, row + 1);
    if (start < previous || end < start || end > expected_row_end) {
      return CUSPARSE_STATUS_INVALID_VALUE;
    }
    previous = end;
  }
  for (entry = 0; entry < matrix->nnz; entry++) {
    int64_t col = psyche_cusparse_read_index(plan->col_indices, plan->index_type, entry) - (int64_t)plan->index_base;
    if (col < 0 || col >= matrix->cols) {
      return CUSPARSE_STATUS_INVALID_VALUE;
    }
  }
  return CUSPARSE_STATUS_SUCCESS;
}

static cusparseStatus_t psyche_cusparse_spmv_cpu(const PsycheCusparseSpMVPlan *plan) {
  float *out = 0;
  int64_t row = 0;
  if (plan == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (plan->rows == 0) {
    return CUSPARSE_STATUS_SUCCESS;
  }
  out = (float *)malloc(plan->y_bytes);
  if (out == 0) {
    return CUSPARSE_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < plan->rows; row++) {
    int64_t start = psyche_cusparse_read_index(plan->row_offsets, plan->index_type, row) - (int64_t)plan->index_base;
    int64_t end = psyche_cusparse_read_index(plan->row_offsets, plan->index_type, row + 1) - (int64_t)plan->index_base;
    int64_t entry = 0;
    float sum = 0.0f;
    if (plan->alpha != 0.0f) {
      for (entry = start; entry < end; entry++) {
        int64_t col = psyche_cusparse_read_index(plan->col_indices, plan->index_type, entry) - (int64_t)plan->index_base;
        sum += plan->values[entry] * plan->x[col];
      }
    }
    out[row] = plan->alpha * sum + (plan->beta != 0.0f ? plan->beta * plan->y[row] : 0.0f);
  }
  memcpy(plan->y, out, plan->y_bytes);
  free(out);
  return CUSPARSE_STATUS_SUCCESS;
}

static cusparseStatus_t psyche_cusparse_validate_spmm_plan(
    cusparseHandle_t handle,
    cusparseOperation_t opA,
    cusparseOperation_t opB,
    const void *alpha,
    cusparseConstSpMatDescr_t matA,
    cusparseConstDnMatDescr_t matB,
    const void *beta,
    cusparseDnMatDescr_t matC,
    cudaDataType computeType,
    cusparseSpMMAlg_t alg,
    PsycheCusparseSpMMPlan *plan,
    int validate_indices) {
  struct cusparseContext *ctx = psyche_cusparse_context(handle);
  struct cusparseSpMatStruct *matrix = psyche_cusparse_spmat(matA);
  struct cusparseDnMatStruct *b_mat = psyche_cusparse_dnmat(matB);
  struct cusparseDnMatStruct *c_mat = psyche_cusparse_dnmat(matC);
  int64_t expected_row_end = 0;
  int64_t previous = 0;
  int64_t row = 0;
  int64_t entry = 0;
  size_t rows_plus_one = 0;
  size_t row_offsets_bytes = 0;
  size_t col_indices_bytes = 0;
  size_t values_bytes = 0;
  size_t b_elements = 0;
  size_t b_bytes = 0;
  size_t c_elements = 0;
  size_t c_bytes = 0;
  size_t output_elements = 0;
  size_t output_bytes = 0;
  size_t index_size = 0;
  float alpha_value = 0.0f;
  float beta_value = 0.0f;

  if (ctx == 0) {
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  if (ctx->pointer_mode != CUSPARSE_POINTER_MODE_HOST) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (alpha == 0 || beta == 0 || plan == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (!psyche_cusparse_aligned(alpha, sizeof(float)) || !psyche_cusparse_aligned(beta, sizeof(float))) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (matrix == 0 || b_mat == 0 || c_mat == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (opA != CUSPARSE_OPERATION_NON_TRANSPOSE || opB != CUSPARSE_OPERATION_NON_TRANSPOSE) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cusparse_valid_spmm_alg(alg)) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (
      matrix->format != CUSPARSE_FORMAT_CSR ||
      matrix->value_type != CUDA_R_32F ||
      b_mat->value_type != CUDA_R_32F ||
      c_mat->value_type != CUDA_R_32F ||
      computeType != CUDA_R_32F) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (
      matrix->row_offsets_type != matrix->col_indices_type ||
      (matrix->row_offsets_type != CUSPARSE_INDEX_32I &&
       matrix->row_offsets_type != CUSPARSE_INDEX_64I)) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  index_size = psyche_cusparse_index_size(matrix->row_offsets_type);
  if (index_size == 0) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (matrix->index_base != CUSPARSE_INDEX_BASE_ZERO && matrix->index_base != CUSPARSE_INDEX_BASE_ONE) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }
  if (
      matrix->rows < 0 ||
      matrix->cols < 0 ||
      matrix->nnz < 0 ||
      b_mat->rows < 0 ||
      b_mat->cols < 0 ||
      c_mat->rows < 0 ||
      c_mat->cols < 0 ||
      b_mat->ld < 0 ||
      c_mat->ld < 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (
      b_mat->rows != matrix->cols ||
      c_mat->rows != matrix->rows ||
      c_mat->cols != b_mat->cols) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (matrix->rows == INT64_MAX) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (
      !psyche_cusparse_valid_order(b_mat->order) ||
      !psyche_cusparse_valid_order(c_mat->order)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  rows_plus_one = (size_t)matrix->rows + 1u;
  if (
      !psyche_cusparse_mul_size(rows_plus_one, index_size, &row_offsets_bytes) ||
      !psyche_cusparse_mul_size((size_t)matrix->nnz, index_size, &col_indices_bytes) ||
      !psyche_cusparse_mul_size((size_t)matrix->nnz, sizeof(float), &values_bytes) ||
      !psyche_cusparse_dnmat_element_span(b_mat->rows, b_mat->cols, b_mat->ld, b_mat->order, &b_elements) ||
      !psyche_cusparse_dnmat_element_span(c_mat->rows, c_mat->cols, c_mat->ld, c_mat->order, &c_elements) ||
      !psyche_cusparse_mul_size(b_elements, sizeof(float), &b_bytes) ||
      !psyche_cusparse_mul_size(c_elements, sizeof(float), &c_bytes) ||
      !psyche_cusparse_mul_size((size_t)matrix->rows, (size_t)b_mat->cols, &output_elements) ||
      !psyche_cusparse_mul_size(output_elements, sizeof(float), &output_bytes)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (matrix->row_offsets == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (matrix->nnz != 0 && (matrix->col_indices == 0 || matrix->values == 0)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (b_bytes != 0 && b_mat->values == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (c_bytes != 0 && c_mat->values == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (
      !psyche_cusparse_aligned(matrix->row_offsets, index_size) ||
      !psyche_cusparse_aligned(matrix->col_indices, index_size) ||
      !psyche_cusparse_aligned(matrix->values, sizeof(float)) ||
      !psyche_cusparse_aligned(b_mat->values, sizeof(float)) ||
      !psyche_cusparse_aligned(c_mat->values, sizeof(float))) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (
      psyche_cusparse_ranges_overlap(c_mat->values, c_bytes, matrix->row_offsets, row_offsets_bytes) ||
      psyche_cusparse_ranges_overlap(c_mat->values, c_bytes, matrix->col_indices, col_indices_bytes) ||
      psyche_cusparse_ranges_overlap(c_mat->values, c_bytes, matrix->values, values_bytes) ||
      psyche_cusparse_ranges_overlap(c_mat->values, c_bytes, b_mat->values, b_bytes)) {
    return CUSPARSE_STATUS_NOT_SUPPORTED;
  }

  memset(plan, 0, sizeof(*plan));
  plan->row_offsets = matrix->row_offsets;
  plan->col_indices = matrix->col_indices;
  plan->values = (const float *)matrix->values;
  plan->b = (const float *)b_mat->values;
  plan->c = (float *)c_mat->values;
  plan->alpha = alpha_value;
  plan->beta = beta_value;
  plan->rows = matrix->rows;
  plan->cols = matrix->cols;
  plan->nnz = matrix->nnz;
  plan->n = b_mat->cols;
  plan->b_ld = b_mat->ld;
  plan->c_ld = c_mat->ld;
  plan->index_base = matrix->index_base == CUSPARSE_INDEX_BASE_ONE ? 1 : 0;
  plan->index_type = matrix->row_offsets_type;
  plan->b_order = b_mat->order;
  plan->c_order = c_mat->order;
  plan->row_offsets_bytes = row_offsets_bytes;
  plan->col_indices_bytes = col_indices_bytes;
  plan->values_bytes = values_bytes;
  plan->b_bytes = b_bytes;
  plan->c_bytes = c_bytes;
  plan->output_elements = output_elements;
  plan->output_bytes = output_bytes;

  if (!validate_indices) {
    return CUSPARSE_STATUS_SUCCESS;
  }
  if (plan->index_base != 0 && matrix->nnz == INT64_MAX) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  expected_row_end = matrix->nnz + plan->index_base;
  previous = plan->index_base;
  if (
      psyche_cusparse_read_index(plan->row_offsets, plan->index_type, 0) != plan->index_base ||
      psyche_cusparse_read_index(plan->row_offsets, plan->index_type, matrix->rows) != expected_row_end) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  for (row = 0; row < matrix->rows; row++) {
    int64_t start = psyche_cusparse_read_index(plan->row_offsets, plan->index_type, row);
    int64_t end = psyche_cusparse_read_index(plan->row_offsets, plan->index_type, row + 1);
    if (start < previous || end < start || end > expected_row_end) {
      return CUSPARSE_STATUS_INVALID_VALUE;
    }
    previous = end;
  }
  for (entry = 0; entry < matrix->nnz; entry++) {
    int64_t col = psyche_cusparse_read_index(plan->col_indices, plan->index_type, entry) - (int64_t)plan->index_base;
    if (col < 0 || col >= matrix->cols) {
      return CUSPARSE_STATUS_INVALID_VALUE;
    }
  }
  return CUSPARSE_STATUS_SUCCESS;
}

static cusparseStatus_t psyche_cusparse_spmm_cpu(const PsycheCusparseSpMMPlan *plan) {
  float *out = 0;
  int64_t row = 0;
  int64_t col = 0;
  if (plan == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (plan->output_elements == 0) {
    return CUSPARSE_STATUS_SUCCESS;
  }
  out = (float *)malloc(plan->output_bytes);
  if (out == 0) {
    return CUSPARSE_STATUS_ALLOC_FAILED;
  }
  for (row = 0; row < plan->rows; row++) {
    int64_t start = psyche_cusparse_read_index(plan->row_offsets, plan->index_type, row) - (int64_t)plan->index_base;
    int64_t end = psyche_cusparse_read_index(plan->row_offsets, plan->index_type, row + 1) - (int64_t)plan->index_base;
    for (col = 0; col < plan->n; col++) {
      int64_t entry = 0;
      float sum = 0.0f;
      float prior = 0.0f;
      if (plan->alpha != 0.0f) {
        for (entry = start; entry < end; entry++) {
          int64_t b_row = psyche_cusparse_read_index(plan->col_indices, plan->index_type, entry) -
              (int64_t)plan->index_base;
          size_t b_index = psyche_cusparse_dnmat_index(b_row, col, plan->b_ld, plan->b_order);
          sum += plan->values[entry] * plan->b[b_index];
        }
      }
      if (plan->beta != 0.0f) {
        size_t c_index = psyche_cusparse_dnmat_index(row, col, plan->c_ld, plan->c_order);
        prior = plan->c[c_index];
      }
      out[(size_t)row * (size_t)plan->n + (size_t)col] = plan->alpha * sum + plan->beta * prior;
    }
  }
  for (row = 0; row < plan->rows; row++) {
    for (col = 0; col < plan->n; col++) {
      size_t c_index = psyche_cusparse_dnmat_index(row, col, plan->c_ld, plan->c_order);
      plan->c[c_index] = out[(size_t)row * (size_t)plan->n + (size_t)col];
    }
  }
  free(out);
  return CUSPARSE_STATUS_SUCCESS;
}

#if defined(__APPLE__)
static CUresult psyche_cusparse_spmm_metal_preflight(
    const PsycheCusparseSpMMPlan *plan,
    unsigned int block_dim_x,
    unsigned int *grid_dim_x) {
  size_t grid_dim_x_size = 0;
  if (grid_dim_x != 0) {
    *grid_dim_x = 0u;
  }
  if (plan == 0 || block_dim_x == 0u || grid_dim_x == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (plan->output_elements == 0) {
    return CUDA_SUCCESS;
  }
  if (psyche_cusparse_metal_backend_disabled_for_test()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  if (
      plan->index_type != CUSPARSE_INDEX_32I ||
      plan->rows > UINT_MAX ||
      plan->cols > UINT_MAX ||
      plan->nnz > UINT_MAX ||
      plan->n > UINT_MAX ||
      plan->b_ld > UINT_MAX ||
      plan->c_ld > UINT_MAX ||
      plan->output_elements > UINT_MAX ||
      plan->b_bytes / sizeof(float) > UINT_MAX ||
      plan->c_bytes / sizeof(float) > UINT_MAX) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  grid_dim_x_size = (plan->output_elements + (size_t)block_dim_x - 1u) / (size_t)block_dim_x;
  if (grid_dim_x_size == 0 || grid_dim_x_size > UINT_MAX) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  *grid_dim_x = (unsigned int)grid_dim_x_size;
  return CUDA_SUCCESS;
}
#endif

PSYCHE_CUDA_STUB_API int psyche_cuda_compat_stub_is_stub(void) {
  return 1;
}

PSYCHE_CUDA_STUB_API const char *psyche_cuda_compat_stub_version(void) {
  return "psyche-cusparse-compat-stub/0.1";
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseCreate(cusparseHandle_t *handle) {
  struct cusparseContext *ctx = 0;
  if (handle == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  *handle = 0;
  if (!psyche_cusparse_simulated_memory_enabled()) {
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  ctx = (struct cusparseContext *)calloc(1, sizeof(*ctx));
  if (ctx == 0) {
    return CUSPARSE_STATUS_ALLOC_FAILED;
  }
  ctx->stream = 0;
  ctx->pointer_mode = CUSPARSE_POINTER_MODE_HOST;
  psyche_cusparse_register_object(&ctx->base, PSYCHE_CUSPARSE_OBJECT_CONTEXT);
  *handle = (cusparseHandle_t)ctx;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseDestroy(cusparseHandle_t handle) {
  if (!psyche_cusparse_destroy_object(handle, PSYCHE_CUSPARSE_OBJECT_CONTEXT)) {
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseGetVersion(cusparseHandle_t handle, int *version) {
  if (version == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  *version = 0;
  if (psyche_cusparse_context(handle) == 0) {
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  *version = PSYCHE_CUSPARSE_VERSION;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseGetProperty(libraryPropertyType type, int *value) {
  if (value == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  switch (type) {
  case MAJOR_VERSION:
  case MINOR_VERSION:
  case PATCH_LEVEL:
    *value = 0;
    return CUSPARSE_STATUS_SUCCESS;
  default:
    *value = 0;
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API const char *cusparseGetErrorName(cusparseStatus_t status) {
  switch (status) {
  case CUSPARSE_STATUS_SUCCESS: return "CUSPARSE_STATUS_SUCCESS";
  case CUSPARSE_STATUS_NOT_INITIALIZED: return "CUSPARSE_STATUS_NOT_INITIALIZED";
  case CUSPARSE_STATUS_ALLOC_FAILED: return "CUSPARSE_STATUS_ALLOC_FAILED";
  case CUSPARSE_STATUS_INVALID_VALUE: return "CUSPARSE_STATUS_INVALID_VALUE";
  case CUSPARSE_STATUS_ARCH_MISMATCH: return "CUSPARSE_STATUS_ARCH_MISMATCH";
  case CUSPARSE_STATUS_MAPPING_ERROR: return "CUSPARSE_STATUS_MAPPING_ERROR";
  case CUSPARSE_STATUS_EXECUTION_FAILED: return "CUSPARSE_STATUS_EXECUTION_FAILED";
  case CUSPARSE_STATUS_INTERNAL_ERROR: return "CUSPARSE_STATUS_INTERNAL_ERROR";
  case CUSPARSE_STATUS_MATRIX_TYPE_NOT_SUPPORTED: return "CUSPARSE_STATUS_MATRIX_TYPE_NOT_SUPPORTED";
  case CUSPARSE_STATUS_ZERO_PIVOT: return "CUSPARSE_STATUS_ZERO_PIVOT";
  case CUSPARSE_STATUS_NOT_SUPPORTED: return "CUSPARSE_STATUS_NOT_SUPPORTED";
  case CUSPARSE_STATUS_INSUFFICIENT_RESOURCES: return "CUSPARSE_STATUS_INSUFFICIENT_RESOURCES";
  default: return "CUSPARSE_STATUS_UNKNOWN";
  }
}

PSYCHE_CUDA_STUB_API const char *cusparseGetErrorString(cusparseStatus_t status) {
  switch (status) {
  case CUSPARSE_STATUS_SUCCESS: return "operation completed successfully";
  case CUSPARSE_STATUS_NOT_INITIALIZED: return "cuSPARSE shim was not initialized";
  case CUSPARSE_STATUS_ALLOC_FAILED: return "resource allocation failed";
  case CUSPARSE_STATUS_INVALID_VALUE: return "invalid value";
  case CUSPARSE_STATUS_ARCH_MISMATCH: return "architecture mismatch";
  case CUSPARSE_STATUS_MAPPING_ERROR: return "memory mapping error";
  case CUSPARSE_STATUS_EXECUTION_FAILED: return "execution failed";
  case CUSPARSE_STATUS_INTERNAL_ERROR: return "internal error";
  case CUSPARSE_STATUS_MATRIX_TYPE_NOT_SUPPORTED: return "matrix type not supported";
  case CUSPARSE_STATUS_ZERO_PIVOT: return "zero pivot";
  case CUSPARSE_STATUS_NOT_SUPPORTED: return "operation not supported by this Apple Silicon compatibility slice";
  case CUSPARSE_STATUS_INSUFFICIENT_RESOURCES: return "insufficient resources";
  default: return "unrecognized error code";
  }
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseSetPointerMode(
    cusparseHandle_t handle,
    cusparsePointerMode_t mode) {
  struct cusparseContext *ctx = psyche_cusparse_context(handle);
  if (ctx == 0) {
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  if (mode != CUSPARSE_POINTER_MODE_HOST && mode != CUSPARSE_POINTER_MODE_DEVICE) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  ctx->pointer_mode = mode;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseGetPointerMode(
    cusparseHandle_t handle,
    cusparsePointerMode_t *mode) {
  struct cusparseContext *ctx = psyche_cusparse_context(handle);
  if (mode == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (ctx == 0) {
    *mode = CUSPARSE_POINTER_MODE_HOST;
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  *mode = ctx->pointer_mode;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseSetStream(cusparseHandle_t handle, void *streamId) {
  struct cusparseContext *ctx = psyche_cusparse_context(handle);
  if (ctx == 0) {
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  ctx->stream = streamId;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseGetStream(cusparseHandle_t handle, void **streamId) {
  struct cusparseContext *ctx = psyche_cusparse_context(handle);
  if (streamId == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (ctx == 0) {
    *streamId = 0;
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  *streamId = ctx->stream;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseCreateCsr(
    cusparseSpMatDescr_t *spMatDescr,
    int64_t rows,
    int64_t cols,
    int64_t nnz,
    void *csrRowOffsets,
    void *csrColInd,
    void *csrValues,
    cusparseIndexType_t csrRowOffsetsType,
    cusparseIndexType_t csrColIndType,
    cusparseIndexBase_t idxBase,
    cudaDataType valueType) {
  struct cusparseSpMatStruct *descriptor = 0;
  if (spMatDescr == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  *spMatDescr = 0;
  if (rows < 0 || cols < 0 || nnz < 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (!psyche_cusparse_valid_index_type(csrRowOffsetsType) || !psyche_cusparse_valid_index_type(csrColIndType)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (idxBase != CUSPARSE_INDEX_BASE_ZERO && idxBase != CUSPARSE_INDEX_BASE_ONE) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (!psyche_cusparse_valid_data_type(valueType)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  descriptor = (struct cusparseSpMatStruct *)calloc(1, sizeof(*descriptor));
  if (descriptor == 0) {
    return CUSPARSE_STATUS_ALLOC_FAILED;
  }
  descriptor->format = CUSPARSE_FORMAT_CSR;
  descriptor->rows = rows;
  descriptor->cols = cols;
  descriptor->nnz = nnz;
  descriptor->row_offsets = csrRowOffsets;
  descriptor->col_indices = csrColInd;
  descriptor->values = csrValues;
  descriptor->row_offsets_type = csrRowOffsetsType;
  descriptor->col_indices_type = csrColIndType;
  descriptor->index_base = idxBase;
  descriptor->value_type = valueType;
  psyche_cusparse_register_object(&descriptor->base, PSYCHE_CUSPARSE_OBJECT_SPMAT);
  *spMatDescr = (cusparseSpMatDescr_t)descriptor;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseDestroySpMat(cusparseConstSpMatDescr_t spMatDescr) {
  if (!psyche_cusparse_destroy_object(spMatDescr, PSYCHE_CUSPARSE_OBJECT_SPMAT)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseCreateDnVec(
    cusparseDnVecDescr_t *dnVecDescr,
    int64_t size,
    void *values,
    cudaDataType valueType) {
  struct cusparseDnVecStruct *descriptor = 0;
  if (dnVecDescr == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  *dnVecDescr = 0;
  if (size < 0 || !psyche_cusparse_valid_data_type(valueType)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  descriptor = (struct cusparseDnVecStruct *)calloc(1, sizeof(*descriptor));
  if (descriptor == 0) {
    return CUSPARSE_STATUS_ALLOC_FAILED;
  }
  descriptor->size = size;
  descriptor->values = values;
  descriptor->value_type = valueType;
  psyche_cusparse_register_object(&descriptor->base, PSYCHE_CUSPARSE_OBJECT_DNVEC);
  *dnVecDescr = (cusparseDnVecDescr_t)descriptor;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseDestroyDnVec(cusparseConstDnVecDescr_t dnVecDescr) {
  if (!psyche_cusparse_destroy_object(dnVecDescr, PSYCHE_CUSPARSE_OBJECT_DNVEC)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  return CUSPARSE_STATUS_SUCCESS;
}

static cusparseStatus_t psyche_cusparse_create_dnmat_descriptor(
    struct cusparseDnMatStruct **descriptor_out,
    int64_t rows,
    int64_t cols,
    int64_t ld,
    void *values,
    cudaDataType valueType,
    cusparseOrder_t order) {
  struct cusparseDnMatStruct *descriptor = 0;
  size_t elements = 0;
  if (descriptor_out == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  *descriptor_out = 0;
  if (!psyche_cusparse_valid_data_type(valueType) || !psyche_cusparse_valid_order(order)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (!psyche_cusparse_dnmat_element_span(rows, cols, ld, order, &elements)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  if (!psyche_cusparse_aligned(values, psyche_cusparse_data_type_alignment(valueType))) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  (void)elements;
  descriptor = (struct cusparseDnMatStruct *)calloc(1, sizeof(*descriptor));
  if (descriptor == 0) {
    return CUSPARSE_STATUS_ALLOC_FAILED;
  }
  descriptor->rows = rows;
  descriptor->cols = cols;
  descriptor->ld = ld;
  descriptor->values = values;
  descriptor->value_type = valueType;
  descriptor->order = order;
  psyche_cusparse_register_object(&descriptor->base, PSYCHE_CUSPARSE_OBJECT_DNMAT);
  *descriptor_out = descriptor;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseCreateDnMat(
    cusparseDnMatDescr_t *dnMatDescr,
    int64_t rows,
    int64_t cols,
    int64_t ld,
    void *values,
    cudaDataType valueType,
    cusparseOrder_t order) {
  struct cusparseDnMatStruct *descriptor = 0;
  cusparseStatus_t status = CUSPARSE_STATUS_SUCCESS;
  if (dnMatDescr == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  *dnMatDescr = 0;
  status = psyche_cusparse_create_dnmat_descriptor(
      &descriptor,
      rows,
      cols,
      ld,
      values,
      valueType,
      order);
  if (status != CUSPARSE_STATUS_SUCCESS) {
    return status;
  }
  *dnMatDescr = (cusparseDnMatDescr_t)descriptor;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseCreateConstDnMat(
    cusparseConstDnMatDescr_t *dnMatDescr,
    int64_t rows,
    int64_t cols,
    int64_t ld,
    const void *values,
    cudaDataType valueType,
    cusparseOrder_t order) {
  struct cusparseDnMatStruct *descriptor = 0;
  cusparseStatus_t status = CUSPARSE_STATUS_SUCCESS;
  if (dnMatDescr == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  *dnMatDescr = 0;
  status = psyche_cusparse_create_dnmat_descriptor(
      &descriptor,
      rows,
      cols,
      ld,
      (void *)values,
      valueType,
      order);
  if (status != CUSPARSE_STATUS_SUCCESS) {
    return status;
  }
  *dnMatDescr = (cusparseConstDnMatDescr_t)descriptor;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseDestroyDnMat(cusparseConstDnMatDescr_t dnMatDescr) {
  if (!psyche_cusparse_destroy_object(dnMatDescr, PSYCHE_CUSPARSE_OBJECT_DNMAT)) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseSpMV_bufferSize(
    cusparseHandle_t handle,
    cusparseOperation_t opA,
    const void *alpha,
    cusparseConstSpMatDescr_t matA,
    cusparseConstDnVecDescr_t vecX,
    const void *beta,
    cusparseDnVecDescr_t vecY,
    cudaDataType computeType,
    cusparseSpMVAlg_t alg,
    size_t *bufferSize) {
  PsycheCusparseSpMVPlan plan;
  cusparseStatus_t status;
  if (bufferSize == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  *bufferSize = 0;
  status = psyche_cusparse_validate_spmv_plan(
      handle,
      opA,
      alpha,
      matA,
      vecX,
      beta,
      vecY,
      computeType,
      alg,
      &plan,
      0);
  if (status != CUSPARSE_STATUS_SUCCESS) {
    return status;
  }
  *bufferSize = 0;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseSpMV(
    cusparseHandle_t handle,
    cusparseOperation_t opA,
    const void *alpha,
    cusparseConstSpMatDescr_t matA,
    cusparseConstDnVecDescr_t vecX,
    const void *beta,
    cusparseDnVecDescr_t vecY,
    cudaDataType computeType,
    cusparseSpMVAlg_t alg,
    void *externalBuffer) {
  PsycheCusparseSpMVPlan plan;
  cusparseStatus_t status = CUSPARSE_STATUS_SUCCESS;
  (void)externalBuffer;
  if (!psyche_cusparse_simulated_memory_enabled()) {
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  status = psyche_cusparse_validate_spmv_plan(
      handle,
      opA,
      alpha,
      matA,
      vecX,
      beta,
      vecY,
      computeType,
      alg,
      &plan,
      1);
  if (status != CUSPARSE_STATUS_SUCCESS) {
    return status;
  }
  if (plan.rows == 0) {
    return CUSPARSE_STATUS_SUCCESS;
  }
#if defined(__APPLE__)
  if (psyche_cusparse_metal_enabled()) {
    const unsigned int block_dim_x = 128u;
    size_t grid_dim_x_size = 0;
    CUresult metal_result = CUDA_ERROR_INVALID_VALUE;
    if (
        plan.index_type != CUSPARSE_INDEX_32I ||
        plan.rows > UINT_MAX ||
        plan.cols > UINT_MAX ||
        plan.nnz > UINT_MAX) {
      metal_result = CUDA_ERROR_NOT_SUPPORTED;
    } else {
      grid_dim_x_size = ((size_t)plan.rows_u + (size_t)block_dim_x - 1u) / (size_t)block_dim_x;
    }
    if (metal_result != CUDA_ERROR_NOT_SUPPORTED && grid_dim_x_size <= UINT_MAX) {
      metal_result = psyche_cuda_metal_launch_cusparse_spmv_csr_f32(
          (const int32_t *)plan.row_offsets,
          (const int32_t *)plan.col_indices,
          plan.values,
          plan.x,
          plan.y,
          plan.y,
          plan.alpha,
          plan.beta,
          plan.rows_u,
          plan.cols_u,
          plan.nnz_u,
          plan.index_base,
          plan.row_offsets_bytes,
          plan.col_indices_bytes,
          plan.values_bytes,
          plan.x_bytes,
          plan.y_bytes,
          (unsigned int)grid_dim_x_size,
          block_dim_x);
    }
    if (metal_result == CUDA_SUCCESS) {
      return CUSPARSE_STATUS_SUCCESS;
    }
    if (psyche_cusparse_metal_required() || !psyche_cusparse_metal_preferred_can_fallback(metal_result)) {
      return psyche_cusparse_status_from_cuda_result(metal_result);
    }
  }
#endif
  return psyche_cusparse_spmv_cpu(&plan);
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseSpMM_bufferSize(
    cusparseHandle_t handle,
    cusparseOperation_t opA,
    cusparseOperation_t opB,
    const void *alpha,
    cusparseConstSpMatDescr_t matA,
    cusparseConstDnMatDescr_t matB,
    const void *beta,
    cusparseDnMatDescr_t matC,
    cudaDataType computeType,
    cusparseSpMMAlg_t alg,
    size_t *bufferSize) {
  PsycheCusparseSpMMPlan plan;
  cusparseStatus_t status;
  if (bufferSize == 0) {
    return CUSPARSE_STATUS_INVALID_VALUE;
  }
  *bufferSize = 0;
  status = psyche_cusparse_validate_spmm_plan(
      handle,
      opA,
      opB,
      alpha,
      matA,
      matB,
      beta,
      matC,
      computeType,
      alg,
      &plan,
      0);
  if (status != CUSPARSE_STATUS_SUCCESS) {
    return status;
  }
#if defined(__APPLE__)
  if (psyche_cusparse_metal_required()) {
    unsigned int grid_dim_x = 0u;
    CUresult metal_result = psyche_cusparse_spmm_metal_preflight(&plan, 128u, &grid_dim_x);
    if (metal_result != CUDA_SUCCESS) {
      return psyche_cusparse_status_from_cuda_result(metal_result);
    }
  }
#endif
  *bufferSize = 0;
  return CUSPARSE_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cusparseStatus_t cusparseSpMM(
    cusparseHandle_t handle,
    cusparseOperation_t opA,
    cusparseOperation_t opB,
    const void *alpha,
    cusparseConstSpMatDescr_t matA,
    cusparseConstDnMatDescr_t matB,
    const void *beta,
    cusparseDnMatDescr_t matC,
    cudaDataType computeType,
    cusparseSpMMAlg_t alg,
    void *externalBuffer) {
  PsycheCusparseSpMMPlan plan;
  cusparseStatus_t status = CUSPARSE_STATUS_SUCCESS;
  (void)externalBuffer;
  if (!psyche_cusparse_simulated_memory_enabled()) {
    return CUSPARSE_STATUS_NOT_INITIALIZED;
  }
  status = psyche_cusparse_validate_spmm_plan(
      handle,
      opA,
      opB,
      alpha,
      matA,
      matB,
      beta,
      matC,
      computeType,
      alg,
      &plan,
      1);
  if (status != CUSPARSE_STATUS_SUCCESS) {
    return status;
  }
  if (plan.output_elements == 0) {
    return CUSPARSE_STATUS_SUCCESS;
  }
#if defined(__APPLE__)
  if (psyche_cusparse_metal_enabled()) {
    const unsigned int block_dim_x = 128u;
    unsigned int grid_dim_x = 0u;
    CUresult metal_result = psyche_cusparse_spmm_metal_preflight(&plan, block_dim_x, &grid_dim_x);
    if (metal_result == CUDA_SUCCESS) {
      metal_result = psyche_cuda_metal_launch_cusparse_spmm_csr_f32(
          (const int32_t *)plan.row_offsets,
          (const int32_t *)plan.col_indices,
          plan.values,
          plan.b,
          plan.c,
          plan.c,
          plan.alpha,
          plan.beta,
          (unsigned int)plan.rows,
          (unsigned int)plan.cols,
          (unsigned int)plan.nnz,
          (unsigned int)plan.n,
          (unsigned int)plan.b_ld,
          (unsigned int)plan.c_ld,
          (unsigned int)plan.b_order,
          (unsigned int)plan.c_order,
          plan.index_base,
          plan.row_offsets_bytes,
          plan.col_indices_bytes,
          plan.values_bytes,
          plan.b_bytes,
          plan.c_bytes,
          plan.output_bytes,
          grid_dim_x,
          block_dim_x);
    }
    if (metal_result == CUDA_SUCCESS) {
      return CUSPARSE_STATUS_SUCCESS;
    }
    if (psyche_cusparse_metal_required() || !psyche_cusparse_metal_preferred_can_fallback(metal_result)) {
      return psyche_cusparse_status_from_cuda_result(metal_result);
    }
  }
#endif
  return psyche_cusparse_spmm_cpu(&plan);
}
