#define _POSIX_C_SOURCE 200809L

#include "cuda_compat_stub.h"

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(__APPLE__)
#define ACCELERATE_NEW_LAPACK 1
#include <Accelerate/Accelerate.h>
#endif

#define PSYCHE_CUBLASLT_HANDLE_MAGIC 0x707379636c744844ULL
#define PSYCHE_CUBLASLT_DESC_MAGIC 0x706c7444u
#define PSYCHE_CUBLASLT_TAIL_MAGIC 0x7461696c70737963ULL
#define PSYCHE_CUBLASLT_VERSION 0
#define PSYCHE_CUBLASLT_KIND_LAYOUT 1u
#define PSYCHE_CUBLASLT_KIND_MATMUL_DESC 2u
#define PSYCHE_CUBLASLT_KIND_PREFERENCE 3u
#define PSYCHE_CUBLASLT_KIND_ALGO 4u
#define PSYCHE_CUBLASLT_KIND_TRANSFORM_DESC 5u

typedef struct PsycheCublasLtContext {
  uint64_t magic;
  struct PsycheCublasLtContext *next;
} PsycheCublasLtContext;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t kind;
  uint32_t owns_allocation;
} PsycheCublasLtHeader;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t kind;
  uint32_t owns_allocation;
  uint32_t type;
  uint32_t order;
  uint32_t batch_count;
  uint32_t batch_mode;
  uint64_t rows;
  uint64_t cols;
  int64_t ld;
  int64_t stride;
} PsycheCublasLtMatrixLayout;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t kind;
  uint32_t owns_allocation;
  int32_t compute_type;
  int32_t scale_type;
  int32_t pointer_mode;
  int32_t transa;
  int32_t transb;
  int32_t transc;
  int32_t fill_mode;
  uint32_t epilogue;
  uintptr_t bias_pointer;
  int64_t bias_stride;
  int32_t bias_data_type;
  int32_t sm_count_target;
  uint32_t reserved0;
  uintptr_t aux_pointer;
  int64_t aux_ld;
  int64_t aux_stride;
  int32_t aux_data_type;
  uint32_t reserved1;
  uint64_t tail_magic;
} PsycheCublasLtMatmulDesc;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t kind;
  uint32_t owns_allocation;
  int32_t scale_type;
  int32_t pointer_mode;
  int32_t transa;
  int32_t transb;
  uint64_t tail_magic;
} PsycheCublasLtMatrixTransformDesc;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t kind;
  uint32_t owns_allocation;
  uint64_t max_workspace_bytes;
  uint32_t search_mode;
  uint32_t reserved0;
  uint64_t tail_magic;
} PsycheCublasLtPreference;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t kind;
  int32_t algo_id;
  int32_t compute_type;
  int32_t scale_type;
  int32_t atype;
  int32_t btype;
  int32_t ctype;
  int32_t dtype;
  uint32_t reserved0;
  uint64_t tail_magic;
} PsycheCublasLtAlgo;

_Static_assert(sizeof(PsycheCublasLtMatrixLayout) <= sizeof(cublasLtMatrixLayoutOpaque_t), "cuBLASLt layout descriptor must fit public opaque ABI");
_Static_assert(sizeof(PsycheCublasLtMatmulDesc) <= sizeof(cublasLtMatmulDescOpaque_t), "cuBLASLt matmul descriptor must fit public opaque ABI");
_Static_assert(sizeof(PsycheCublasLtMatrixTransformDesc) <= sizeof(cublasLtMatrixTransformDescOpaque_t), "cuBLASLt transform descriptor must fit public opaque ABI");
_Static_assert(sizeof(PsycheCublasLtPreference) <= sizeof(cublasLtMatmulPreferenceOpaque_t), "cuBLASLt preference descriptor must fit public opaque ABI");
_Static_assert(sizeof(PsycheCublasLtAlgo) <= sizeof(cublasLtMatmulAlgo_t), "cuBLASLt algo descriptor must fit public opaque ABI");

static pthread_mutex_t psyche_cublaslt_handle_mutex = PTHREAD_MUTEX_INITIALIZER;
static PsycheCublasLtContext *psyche_cublaslt_handles = 0;
static pthread_mutex_t psyche_cublaslt_warning_mutex = PTHREAD_MUTEX_INITIALIZER;
static int psyche_cublaslt_warned_operand_bgrad_d_output = 0;

static int psyche_cublaslt_env_truthy(const char *value) {
  if (value == 0 || value[0] == '\0') {
    return 0;
  }
  return
      strcmp(value, "1") == 0 ||
      strcasecmp(value, "true") == 0 ||
      strcasecmp(value, "yes") == 0 ||
      strcasecmp(value, "on") == 0;
}

static int psyche_cublaslt_simulated_memory_enabled(void) {
  return psyche_cublaslt_env_truthy(getenv("PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY"));
}

static void psyche_cublaslt_register_context(PsycheCublasLtContext *ctx) {
  pthread_mutex_lock(&psyche_cublaslt_handle_mutex);
  ctx->next = psyche_cublaslt_handles;
  psyche_cublaslt_handles = ctx;
  pthread_mutex_unlock(&psyche_cublaslt_handle_mutex);
}

static PsycheCublasLtContext *psyche_cublaslt_context(cublasLtHandle_t handle) {
  PsycheCublasLtContext *record = 0;
  pthread_mutex_lock(&psyche_cublaslt_handle_mutex);
  record = psyche_cublaslt_handles;
  while (record != 0) {
    if ((cublasLtHandle_t)record == handle) {
      break;
    }
    record = record->next;
  }
  if (record != 0 && record->magic != PSYCHE_CUBLASLT_HANDLE_MAGIC) {
    record = 0;
  }
  pthread_mutex_unlock(&psyche_cublaslt_handle_mutex);
  return record;
}

static PsycheCublasLtContext *psyche_cublaslt_unregister_context(cublasLtHandle_t handle) {
  PsycheCublasLtContext *record = 0;
  PsycheCublasLtContext **slot = 0;
  pthread_mutex_lock(&psyche_cublaslt_handle_mutex);
  slot = &psyche_cublaslt_handles;
  while (*slot != 0) {
    if ((cublasLtHandle_t)(*slot) == handle) {
      record = *slot;
      *slot = record->next;
      break;
    }
    slot = &(*slot)->next;
  }
  if (record != 0 && record->magic != PSYCHE_CUBLASLT_HANDLE_MAGIC) {
    record = 0;
  }
  pthread_mutex_unlock(&psyche_cublaslt_handle_mutex);
  return record;
}

static int psyche_cublaslt_valid_op(cublasOperation_t op) {
  return op == CUBLAS_OP_N || op == CUBLAS_OP_T || op == CUBLAS_OP_C;
}

static int psyche_cublaslt_valid_real_data_type(cudaDataType_t type) {
  return type == CUDA_R_32F || type == CUDA_R_64F;
}

static int psyche_cublaslt_data_type_size(cudaDataType_t type, size_t *bytes) {
  if (bytes == 0) {
    return 0;
  }
  switch (type) {
  case CUDA_R_32F:
    *bytes = sizeof(float);
    return 1;
  case CUDA_R_64F:
    *bytes = sizeof(double);
    return 1;
  default:
    return 0;
  }
}

static int psyche_cublaslt_compute_matches_data(
    cublasComputeType_t compute_type,
    cudaDataType_t scale_type,
    cudaDataType_t data_type) {
  if (data_type == CUDA_R_32F) {
    return
        scale_type == CUDA_R_32F &&
        (compute_type == CUBLAS_COMPUTE_32F || compute_type == CUBLAS_COMPUTE_32F_PEDANTIC);
  }
  if (data_type == CUDA_R_64F) {
    return
        scale_type == CUDA_R_64F &&
        (compute_type == CUBLAS_COMPUTE_64F || compute_type == CUBLAS_COMPUTE_64F_PEDANTIC);
  }
  return 0;
}

static cublasStatus_t psyche_cublaslt_validate_desc_header(
    const void *ptr,
    uint32_t kind) {
  const PsycheCublasLtHeader *header = (const PsycheCublasLtHeader *)ptr;
  if (ptr == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (header->magic != PSYCHE_CUBLASLT_DESC_MAGIC || header->version != PSYCHE_CUBLASLT_VERSION || header->kind != kind) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (kind == PSYCHE_CUBLASLT_KIND_MATMUL_DESC &&
      ((const PsycheCublasLtMatmulDesc *)ptr)->tail_magic != PSYCHE_CUBLASLT_TAIL_MAGIC) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (kind == PSYCHE_CUBLASLT_KIND_TRANSFORM_DESC &&
      ((const PsycheCublasLtMatrixTransformDesc *)ptr)->tail_magic != PSYCHE_CUBLASLT_TAIL_MAGIC) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (kind == PSYCHE_CUBLASLT_KIND_PREFERENCE &&
      ((const PsycheCublasLtPreference *)ptr)->tail_magic != PSYCHE_CUBLASLT_TAIL_MAGIC) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_get_attr_value(
    const void *source,
    size_t source_size,
    void *buf,
    size_t sizeInBytes,
    size_t *sizeWritten) {
  if (sizeInBytes == 0) {
    if (sizeWritten == 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    *sizeWritten = source_size;
    return CUBLAS_STATUS_SUCCESS;
  }
  if (buf == 0 || sizeInBytes != source_size) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  memcpy(buf, source, source_size);
  if (sizeWritten != 0) {
    *sizeWritten = source_size;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_set_u32(uint32_t *target, const void *buf, size_t sizeInBytes) {
  if (buf == 0 || sizeInBytes != sizeof(*target)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  memcpy(target, buf, sizeof(*target));
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_set_i32(int32_t *target, const void *buf, size_t sizeInBytes) {
  if (buf == 0 || sizeInBytes != sizeof(*target)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  memcpy(target, buf, sizeof(*target));
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_set_i64(int64_t *target, const void *buf, size_t sizeInBytes) {
  if (buf == 0 || sizeInBytes != sizeof(*target)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  memcpy(target, buf, sizeof(*target));
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_set_u64(uint64_t *target, const void *buf, size_t sizeInBytes) {
  if (buf == 0 || sizeInBytes != sizeof(*target)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  memcpy(target, buf, sizeof(*target));
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_set_uintptr(uintptr_t *target, const void *buf, size_t sizeInBytes) {
  if (buf == 0 || sizeInBytes != sizeof(void *)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  memcpy(target, buf, sizeof(void *));
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_init_layout(
    cublasLtMatrixLayout_t matLayout,
    size_t size,
    cudaDataType_t type,
    uint64_t rows,
    uint64_t cols,
    int64_t ld,
    uint32_t owns_allocation) {
  PsycheCublasLtMatrixLayout *layout = (PsycheCublasLtMatrixLayout *)matLayout;
  if (layout == 0 || size < sizeof(*layout)) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  if (ld < 0 || ((rows > 0 || cols > 0) && ld == 0)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  memset(layout, 0, sizeof(*layout));
  layout->magic = PSYCHE_CUBLASLT_DESC_MAGIC;
  layout->version = PSYCHE_CUBLASLT_VERSION;
  layout->kind = PSYCHE_CUBLASLT_KIND_LAYOUT;
  layout->owns_allocation = owns_allocation;
  layout->type = (uint32_t)type;
  layout->order = CUBLASLT_ORDER_COL;
  layout->batch_count = 1;
  layout->batch_mode = CUBLASLT_BATCH_MODE_STRIDED;
  layout->rows = rows;
  layout->cols = cols;
  layout->ld = ld;
  layout->stride = 0;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_init_matmul_desc(
    cublasLtMatmulDesc_t matmulDesc,
    size_t size,
    cublasComputeType_t computeType,
    cudaDataType_t scaleType,
    uint32_t owns_allocation) {
  PsycheCublasLtMatmulDesc *desc = (PsycheCublasLtMatmulDesc *)matmulDesc;
  if (desc == 0 || size < sizeof(*desc)) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  memset(desc, 0, sizeof(*desc));
  desc->magic = PSYCHE_CUBLASLT_DESC_MAGIC;
  desc->version = PSYCHE_CUBLASLT_VERSION;
  desc->kind = PSYCHE_CUBLASLT_KIND_MATMUL_DESC;
  desc->owns_allocation = owns_allocation;
  desc->compute_type = (int32_t)computeType;
  desc->scale_type = (int32_t)scaleType;
  desc->pointer_mode = CUBLASLT_POINTER_MODE_HOST;
  desc->transa = CUBLAS_OP_N;
  desc->transb = CUBLAS_OP_N;
  desc->transc = CUBLAS_OP_N;
  desc->fill_mode = CUBLAS_FILL_MODE_FULL;
  desc->epilogue = CUBLASLT_EPILOGUE_DEFAULT;
  desc->bias_data_type = -1;
  desc->aux_data_type = -1;
  desc->tail_magic = PSYCHE_CUBLASLT_TAIL_MAGIC;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_init_transform_desc(
    cublasLtMatrixTransformDesc_t transformDesc,
    size_t size,
    cudaDataType_t scaleType,
    uint32_t owns_allocation) {
  PsycheCublasLtMatrixTransformDesc *desc = (PsycheCublasLtMatrixTransformDesc *)transformDesc;
  if (desc == 0 || size < sizeof(*desc)) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  if (!psyche_cublaslt_valid_real_data_type(scaleType)) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  memset(desc, 0, sizeof(*desc));
  desc->magic = PSYCHE_CUBLASLT_DESC_MAGIC;
  desc->version = PSYCHE_CUBLASLT_VERSION;
  desc->kind = PSYCHE_CUBLASLT_KIND_TRANSFORM_DESC;
  desc->owns_allocation = owns_allocation;
  desc->scale_type = (int32_t)scaleType;
  desc->pointer_mode = CUBLASLT_POINTER_MODE_HOST;
  desc->transa = CUBLAS_OP_N;
  desc->transb = CUBLAS_OP_N;
  desc->tail_magic = PSYCHE_CUBLASLT_TAIL_MAGIC;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_init_preference(
    cublasLtMatmulPreference_t pref,
    size_t size,
    uint32_t owns_allocation) {
  PsycheCublasLtPreference *preference = (PsycheCublasLtPreference *)pref;
  if (preference == 0 || size < sizeof(*preference)) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  memset(preference, 0, sizeof(*preference));
  preference->magic = PSYCHE_CUBLASLT_DESC_MAGIC;
  preference->version = PSYCHE_CUBLASLT_VERSION;
  preference->kind = PSYCHE_CUBLASLT_KIND_PREFERENCE;
  preference->owns_allocation = owns_allocation;
  preference->max_workspace_bytes = 0;
  preference->search_mode = 0;
  preference->tail_magic = PSYCHE_CUBLASLT_TAIL_MAGIC;
  return CUBLAS_STATUS_SUCCESS;
}

static void psyche_cublaslt_init_algo(
    cublasLtMatmulAlgo_t *algo,
    cublasComputeType_t computeType,
    cudaDataType_t scaleType,
    cudaDataType_t Atype,
    cudaDataType_t Btype,
    cudaDataType_t Ctype,
    cudaDataType_t Dtype,
    int algoId) {
  PsycheCublasLtAlgo *psyche_algo = (PsycheCublasLtAlgo *)algo;
  memset(psyche_algo, 0, sizeof(*psyche_algo));
  psyche_algo->magic = PSYCHE_CUBLASLT_DESC_MAGIC;
  psyche_algo->version = PSYCHE_CUBLASLT_VERSION;
  psyche_algo->kind = PSYCHE_CUBLASLT_KIND_ALGO;
  psyche_algo->algo_id = algoId;
  psyche_algo->compute_type = (int32_t)computeType;
  psyche_algo->scale_type = (int32_t)scaleType;
  psyche_algo->atype = (int32_t)Atype;
  psyche_algo->btype = (int32_t)Btype;
  psyche_algo->ctype = (int32_t)Ctype;
  psyche_algo->dtype = (int32_t)Dtype;
  psyche_algo->tail_magic = PSYCHE_CUBLASLT_TAIL_MAGIC;
}

static cublasStatus_t psyche_cublaslt_validate_algo(const cublasLtMatmulAlgo_t *algo) {
  const PsycheCublasLtAlgo *psyche_algo = (const PsycheCublasLtAlgo *)algo;
  if (algo == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (
      psyche_algo->magic != PSYCHE_CUBLASLT_DESC_MAGIC ||
      psyche_algo->version != PSYCHE_CUBLASLT_VERSION ||
      psyche_algo->kind != PSYCHE_CUBLASLT_KIND_ALGO ||
      psyche_algo->tail_magic != PSYCHE_CUBLASLT_TAIL_MAGIC ||
      psyche_algo->algo_id != 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_validate_layout_shape(
    const PsycheCublasLtMatrixLayout *layout) {
  if (layout->batch_mode != CUBLASLT_BATCH_MODE_STRIDED && layout->batch_mode != CUBLASLT_BATCH_MODE_POINTER_ARRAY) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (layout->order != CUBLASLT_ORDER_COL && layout->order != CUBLASLT_ORDER_ROW) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if ((int32_t)layout->batch_count < 0 || layout->stride < 0 || layout->ld < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (layout->order == CUBLASLT_ORDER_COL && layout->rows > 0 && (uint64_t)layout->ld < layout->rows) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (layout->order == CUBLASLT_ORDER_ROW && layout->cols > 0 && (uint64_t)layout->ld < layout->cols) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (!psyche_cublaslt_valid_real_data_type((cudaDataType_t)layout->type)) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static uint64_t psyche_cublaslt_op_rows(const PsycheCublasLtMatrixLayout *layout, cublasOperation_t op) {
  return op == CUBLAS_OP_N ? layout->rows : layout->cols;
}

static uint64_t psyche_cublaslt_op_cols(const PsycheCublasLtMatrixLayout *layout, cublasOperation_t op) {
  return op == CUBLAS_OP_N ? layout->cols : layout->rows;
}

static int psyche_cublaslt_batch_compatible(uint32_t count, uint32_t target) {
  if (target == 0) {
    return count == 0 || count == 1;
  }
  return count == 1 || count == target;
}

static int psyche_cublaslt_pointer_array_mode(uint32_t batch_mode) {
  return batch_mode == CUBLASLT_BATCH_MODE_POINTER_ARRAY;
}

static int psyche_cublaslt_scalar_is_zero(const void *scalar, cudaDataType_t data_type) {
  if (scalar == 0) {
    return 0;
  }
  if (data_type == CUDA_R_32F) {
    return *(const float *)scalar == 0.0f;
  }
  if (data_type == CUDA_R_64F) {
    return *(const double *)scalar == 0.0;
  }
  return 0;
}

static int psyche_cublaslt_supported_epilogue(uint32_t epilogue) {
  return
      epilogue == CUBLASLT_EPILOGUE_DEFAULT ||
      epilogue == CUBLASLT_EPILOGUE_RELU ||
      epilogue == CUBLASLT_EPILOGUE_RELU_AUX ||
      epilogue == CUBLASLT_EPILOGUE_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_RELU_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_RELU_AUX_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_DRELU ||
      epilogue == CUBLASLT_EPILOGUE_DRELU_BGRAD ||
      epilogue == CUBLASLT_EPILOGUE_GELU ||
      epilogue == CUBLASLT_EPILOGUE_GELU_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_GELU_AUX ||
      epilogue == CUBLASLT_EPILOGUE_GELU_AUX_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_DGELU ||
      epilogue == CUBLASLT_EPILOGUE_DGELU_BGRAD ||
      epilogue == CUBLASLT_EPILOGUE_BGRADA ||
      epilogue == CUBLASLT_EPILOGUE_BGRADB;
}

static int psyche_cublaslt_epilogue_uses_bias_input(uint32_t epilogue) {
  return
      epilogue == CUBLASLT_EPILOGUE_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_RELU_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_RELU_AUX_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_GELU_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_GELU_AUX_BIAS;
}

static int psyche_cublaslt_epilogue_writes_output_bias_gradient(uint32_t epilogue) {
  return epilogue == CUBLASLT_EPILOGUE_DRELU_BGRAD || epilogue == CUBLASLT_EPILOGUE_DGELU_BGRAD;
}

static int psyche_cublaslt_epilogue_writes_bgrada(uint32_t epilogue) {
  return epilogue == CUBLASLT_EPILOGUE_BGRADA;
}

static int psyche_cublaslt_epilogue_writes_bgradb(uint32_t epilogue) {
  return epilogue == CUBLASLT_EPILOGUE_BGRADB;
}

static int psyche_cublaslt_epilogue_writes_operand_bias_gradient(uint32_t epilogue) {
  return psyche_cublaslt_epilogue_writes_bgrada(epilogue) || psyche_cublaslt_epilogue_writes_bgradb(epilogue);
}

static int psyche_cublaslt_epilogue_writes_bias_gradient(uint32_t epilogue) {
  return
      psyche_cublaslt_epilogue_writes_output_bias_gradient(epilogue) ||
      psyche_cublaslt_epilogue_writes_operand_bias_gradient(epilogue);
}

static int psyche_cublaslt_epilogue_uses_bias(uint32_t epilogue) {
  return
      psyche_cublaslt_epilogue_uses_bias_input(epilogue) ||
      psyche_cublaslt_epilogue_writes_bias_gradient(epilogue);
}

static int psyche_cublaslt_epilogue_uses_relu(uint32_t epilogue) {
  return
      epilogue == CUBLASLT_EPILOGUE_RELU ||
      epilogue == CUBLASLT_EPILOGUE_RELU_AUX ||
      epilogue == CUBLASLT_EPILOGUE_RELU_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_RELU_AUX_BIAS;
}

static int psyche_cublaslt_epilogue_uses_gelu(uint32_t epilogue) {
  return
      epilogue == CUBLASLT_EPILOGUE_GELU ||
      epilogue == CUBLASLT_EPILOGUE_GELU_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_GELU_AUX ||
      epilogue == CUBLASLT_EPILOGUE_GELU_AUX_BIAS;
}

static int psyche_cublaslt_epilogue_uses_aux(uint32_t epilogue) {
  return
      epilogue == CUBLASLT_EPILOGUE_RELU_AUX ||
      epilogue == CUBLASLT_EPILOGUE_RELU_AUX_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_DRELU ||
      epilogue == CUBLASLT_EPILOGUE_DRELU_BGRAD ||
      epilogue == CUBLASLT_EPILOGUE_GELU_AUX ||
      epilogue == CUBLASLT_EPILOGUE_GELU_AUX_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_DGELU ||
      epilogue == CUBLASLT_EPILOGUE_DGELU_BGRAD;
}

static int psyche_cublaslt_epilogue_uses_relu_aux_mask(uint32_t epilogue) {
  return
      epilogue == CUBLASLT_EPILOGUE_RELU_AUX ||
      epilogue == CUBLASLT_EPILOGUE_RELU_AUX_BIAS ||
      epilogue == CUBLASLT_EPILOGUE_DRELU ||
      epilogue == CUBLASLT_EPILOGUE_DRELU_BGRAD;
}

static int psyche_cublaslt_epilogue_writes_relu_aux_mask(uint32_t epilogue) {
  return epilogue == CUBLASLT_EPILOGUE_RELU_AUX || epilogue == CUBLASLT_EPILOGUE_RELU_AUX_BIAS;
}

static int psyche_cublaslt_epilogue_uses_drelu(uint32_t epilogue) {
  return epilogue == CUBLASLT_EPILOGUE_DRELU || epilogue == CUBLASLT_EPILOGUE_DRELU_BGRAD;
}

static int psyche_cublaslt_epilogue_writes_gelu_aux(uint32_t epilogue) {
  return epilogue == CUBLASLT_EPILOGUE_GELU_AUX || epilogue == CUBLASLT_EPILOGUE_GELU_AUX_BIAS;
}

static int psyche_cublaslt_epilogue_uses_dgelu(uint32_t epilogue) {
  return epilogue == CUBLASLT_EPILOGUE_DGELU || epilogue == CUBLASLT_EPILOGUE_DGELU_BGRAD;
}

static int psyche_cublaslt_bias_type_matches_data(
    const PsycheCublasLtMatmulDesc *desc,
    cudaDataType_t data_type) {
  return desc->bias_data_type == -1 || desc->bias_data_type == (int32_t)data_type;
}

static int psyche_cublaslt_aux_type_matches_data(
    const PsycheCublasLtMatmulDesc *desc,
    cudaDataType_t data_type) {
  return desc->aux_data_type == -1 || desc->aux_data_type == (int32_t)data_type;
}

static uint64_t psyche_cublaslt_bias_gradient_length(
    const PsycheCublasLtMatmulDesc *desc,
    const PsycheCublasLtMatrixLayout *Ddesc) {
  if (psyche_cublaslt_epilogue_writes_bgradb(desc->epilogue)) {
    return Ddesc->cols;
  }
  return Ddesc->rows;
}

static void psyche_cublaslt_warn_operand_bgrad_d_output(uint32_t epilogue) {
  if (!psyche_cublaslt_epilogue_writes_operand_bias_gradient(epilogue)) {
    return;
  }
  pthread_mutex_lock(&psyche_cublaslt_warning_mutex);
  if (!psyche_cublaslt_warned_operand_bgrad_d_output) {
    fprintf(stderr, "Psyche CUDA compat: cuBLASLt BGRADA/BGRADB keep D as raw DEFAULT matmul output; NVIDIA D-output parity is unverified.\n");
    psyche_cublaslt_warned_operand_bgrad_d_output = 1;
  }
  pthread_mutex_unlock(&psyche_cublaslt_warning_mutex);
}

static cublasStatus_t psyche_cublaslt_validate_matmul_config(
    cublasLtHandle_t handle,
    cublasLtMatmulDesc_t computeDesc,
    cublasLtMatrixLayout_t Adesc,
    cublasLtMatrixLayout_t Bdesc,
    cublasLtMatrixLayout_t Cdesc,
    cublasLtMatrixLayout_t Ddesc,
    cudaDataType_t *data_type_out,
    uint64_t *m_out,
    uint64_t *n_out,
    uint64_t *k_out,
    uint32_t *batch_count_out) {
  const PsycheCublasLtMatmulDesc *desc = (const PsycheCublasLtMatmulDesc *)computeDesc;
  const PsycheCublasLtMatrixLayout *a_layout = (const PsycheCublasLtMatrixLayout *)Adesc;
  const PsycheCublasLtMatrixLayout *b_layout = (const PsycheCublasLtMatrixLayout *)Bdesc;
  const PsycheCublasLtMatrixLayout *c_layout = (const PsycheCublasLtMatrixLayout *)Cdesc;
  const PsycheCublasLtMatrixLayout *d_layout = (const PsycheCublasLtMatrixLayout *)Ddesc;
  cudaDataType_t data_type = CUDA_R_32F;
  uint64_t a_rows = 0;
  uint64_t a_cols = 0;
  uint64_t b_rows = 0;
  uint64_t b_cols = 0;
  uint32_t batch_count = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;

  if (psyche_cublaslt_context(handle) == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  status = psyche_cublaslt_validate_desc_header(computeDesc, PSYCHE_CUBLASLT_KIND_MATMUL_DESC);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_validate_desc_header(Adesc, PSYCHE_CUBLASLT_KIND_LAYOUT);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_validate_desc_header(Bdesc, PSYCHE_CUBLASLT_KIND_LAYOUT);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_validate_desc_header(Cdesc, PSYCHE_CUBLASLT_KIND_LAYOUT);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_validate_desc_header(Ddesc, PSYCHE_CUBLASLT_KIND_LAYOUT);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }

  if (!psyche_cublaslt_valid_op((cublasOperation_t)desc->transa) || !psyche_cublaslt_valid_op((cublasOperation_t)desc->transb)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (desc->pointer_mode != CUBLASLT_POINTER_MODE_HOST) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cublaslt_supported_epilogue(desc->epilogue) || desc->transc != CUBLAS_OP_N || desc->fill_mode != CUBLAS_FILL_MODE_FULL) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }

  status = psyche_cublaslt_validate_layout_shape(a_layout);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_validate_layout_shape(b_layout);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_validate_layout_shape(c_layout);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_validate_layout_shape(d_layout);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }

  data_type = (cudaDataType_t)d_layout->type;
  if (
      a_layout->type != d_layout->type ||
      b_layout->type != d_layout->type ||
      c_layout->type != d_layout->type) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cublaslt_compute_matches_data((cublasComputeType_t)desc->compute_type, (cudaDataType_t)desc->scale_type, data_type)) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }

  a_rows = psyche_cublaslt_op_rows(a_layout, (cublasOperation_t)desc->transa);
  a_cols = psyche_cublaslt_op_cols(a_layout, (cublasOperation_t)desc->transa);
  b_rows = psyche_cublaslt_op_rows(b_layout, (cublasOperation_t)desc->transb);
  b_cols = psyche_cublaslt_op_cols(b_layout, (cublasOperation_t)desc->transb);
  if (a_cols != b_rows || a_rows != d_layout->rows || b_cols != d_layout->cols) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (c_layout->rows != d_layout->rows || c_layout->cols != d_layout->cols) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  /* Accelerate CBLAS takes 32-bit int dimensions and leading dimensions. */
  if (
      d_layout->rows > (uint64_t)INT_MAX ||
      d_layout->cols > (uint64_t)INT_MAX ||
      a_cols > (uint64_t)INT_MAX ||
      a_layout->ld > (int64_t)INT_MAX ||
      b_layout->ld > (int64_t)INT_MAX ||
      c_layout->ld > (int64_t)INT_MAX ||
      d_layout->ld > (int64_t)INT_MAX) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }

  if (
      a_layout->batch_mode != d_layout->batch_mode ||
      b_layout->batch_mode != d_layout->batch_mode ||
      c_layout->batch_mode != d_layout->batch_mode) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (psyche_cublaslt_pointer_array_mode(d_layout->batch_mode)) {
    if (desc->epilogue != CUBLASLT_EPILOGUE_DEFAULT) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    batch_count = d_layout->batch_count;
    if (
        a_layout->batch_count != batch_count ||
        b_layout->batch_count != batch_count ||
        c_layout->batch_count != batch_count) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
  } else {
    batch_count = d_layout->batch_count;
    if (
        !psyche_cublaslt_batch_compatible(a_layout->batch_count, batch_count) ||
        !psyche_cublaslt_batch_compatible(b_layout->batch_count, batch_count) ||
        !psyche_cublaslt_batch_compatible(c_layout->batch_count, batch_count)) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (batch_count > 1 && d_layout->stride == 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
  }
  if (psyche_cublaslt_epilogue_uses_bias(desc->epilogue)) {
    size_t bias_element_size = 0;
    uint64_t bias_vector_length = d_layout->rows;
    if (desc->bias_pointer == 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (!psyche_cublaslt_bias_type_matches_data(desc, data_type)) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    if (!psyche_cublaslt_data_type_size(data_type, &bias_element_size)) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    if ((desc->bias_pointer % (uintptr_t)bias_element_size) != 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (desc->bias_stride < 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (psyche_cublaslt_epilogue_writes_bias_gradient(desc->epilogue)) {
      bias_vector_length = psyche_cublaslt_bias_gradient_length(desc, d_layout);
    }
    if (desc->bias_stride > 0 && (uint64_t)desc->bias_stride < bias_vector_length) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (psyche_cublaslt_epilogue_writes_bias_gradient(desc->epilogue) && batch_count > 1 && desc->bias_stride == 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
  }
  if (psyche_cublaslt_epilogue_uses_aux(desc->epilogue)) {
    size_t aux_element_size = 0;
    uint64_t aux_matrix_elements = 0;
    if (desc->aux_pointer == 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (desc->aux_ld <= 0 || (uint64_t)desc->aux_ld < d_layout->rows) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (desc->aux_stride < 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (psyche_cublaslt_epilogue_uses_relu_aux_mask(desc->epilogue)) {
      if (desc->aux_data_type != -1) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      /* cuBLASLt specifies ReLU AUX leading dimensions and strides in bits. */
      if ((desc->aux_ld % 128) != 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      if (desc->aux_ld > (int64_t)INT_MAX) {
        return CUBLAS_STATUS_NOT_SUPPORTED;
      }
      if (d_layout->cols > 0 && (uint64_t)desc->aux_ld > UINT64_MAX / d_layout->cols) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      aux_matrix_elements = (uint64_t)desc->aux_ld * d_layout->cols;
      if (desc->aux_stride > 0) {
        if ((desc->aux_stride % 128) != 0 || (uint64_t)desc->aux_stride < aux_matrix_elements) {
          return CUBLAS_STATUS_INVALID_VALUE;
        }
      }
      if (batch_count > 1 && desc->aux_stride == 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
    } else {
      if (!psyche_cublaslt_aux_type_matches_data(desc, data_type)) {
        return CUBLAS_STATUS_NOT_SUPPORTED;
      }
      if (!psyche_cublaslt_data_type_size(data_type, &aux_element_size)) {
        return CUBLAS_STATUS_NOT_SUPPORTED;
      }
      if ((desc->aux_pointer % (uintptr_t)aux_element_size) != 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      /* cuBLASLt specifies GELU AUX leading dimension divisibility in elements, not bytes. */
      if ((desc->aux_ld % 8) != 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      if (desc->aux_ld > (int64_t)INT_MAX) {
        return CUBLAS_STATUS_NOT_SUPPORTED;
      }
      if (d_layout->cols > 0 && (uint64_t)desc->aux_ld > UINT64_MAX / d_layout->cols) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      aux_matrix_elements = (uint64_t)desc->aux_ld * d_layout->cols;
      if (desc->aux_stride > 0) {
        if ((desc->aux_stride % 8) != 0 || (uint64_t)desc->aux_stride < aux_matrix_elements) {
          return CUBLAS_STATUS_INVALID_VALUE;
        }
      }
      if (batch_count > 1 && desc->aux_stride == 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
    }
  }
  if (data_type_out != 0) {
    *data_type_out = data_type;
  }
  if (m_out != 0) {
    *m_out = d_layout->rows;
  }
  if (n_out != 0) {
    *n_out = d_layout->cols;
  }
  if (k_out != 0) {
    *k_out = a_cols;
  }
  if (batch_count_out != 0) {
    *batch_count_out = batch_count;
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasOperation_t psyche_cublaslt_real_op(cublasOperation_t op) {
  return op == CUBLAS_OP_C ? CUBLAS_OP_T : op;
}

static int psyche_cublaslt_all_column_major(
    const PsycheCublasLtMatrixLayout *Adesc,
    const PsycheCublasLtMatrixLayout *Bdesc,
    const PsycheCublasLtMatrixLayout *Cdesc,
    const PsycheCublasLtMatrixLayout *Ddesc) {
  return
      Adesc->order == CUBLASLT_ORDER_COL &&
      Bdesc->order == CUBLASLT_ORDER_COL &&
      Cdesc->order == CUBLASLT_ORDER_COL &&
      Ddesc->order == CUBLASLT_ORDER_COL;
}

#if defined(__APPLE__)
static enum CBLAS_TRANSPOSE psyche_cublaslt_accelerate_trans(cublasOperation_t op) {
  return psyche_cublaslt_real_op(op) == CUBLAS_OP_N ? CblasNoTrans : CblasTrans;
}
#endif

static cublasStatus_t psyche_cublaslt_temp_bytes(uint64_t rows, uint64_t cols, size_t element_size, size_t *bytes) {
  if (bytes == 0 || element_size == 0 || rows > SIZE_MAX || cols > SIZE_MAX) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if ((size_t)rows != 0 && (size_t)cols > SIZE_MAX / (size_t)rows) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if ((size_t)rows * (size_t)cols > SIZE_MAX / element_size) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *bytes = (size_t)rows * (size_t)cols * element_size;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_layout_matrix_bytes(
    const PsycheCublasLtMatrixLayout *layout,
    size_t element_size,
    size_t *bytes) {
  uint64_t ld = 0;
  uint64_t major = 0;
  uint64_t minor = 0;
  uint64_t major_offset = 0;
  uint64_t max_index = 0;
  if (layout == 0 || bytes == 0 || element_size == 0 || layout->ld < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *bytes = 0;
  if (layout->rows == 0 || layout->cols == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  ld = (uint64_t)layout->ld;
  if (layout->order == CUBLASLT_ORDER_ROW) {
    major = layout->rows - 1;
    minor = layout->cols - 1;
  } else {
    major = layout->cols - 1;
    minor = layout->rows - 1;
  }
  if (ld != 0 && major > UINT64_MAX / ld) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  major_offset = major * ld;
  if (major_offset > UINT64_MAX - minor) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  max_index = major_offset + minor;
  if (max_index == UINT64_MAX || max_index + 1 > SIZE_MAX / element_size) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *bytes = (size_t)(max_index + 1) * element_size;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_aux_matrix_bytes(
    const PsycheCublasLtMatmulDesc *desc,
    uint64_t rows,
    uint64_t cols,
    size_t element_size,
    size_t *bytes) {
  uint64_t aux_ld = 0;
  uint64_t aux_elements = 0;
  uint64_t col_offset = 0;
  uint64_t max_index = 0;
  if (desc == 0 || bytes == 0 || element_size == 0 || desc->aux_ld < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *bytes = 0;
  if (rows == 0 || cols == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  aux_ld = (uint64_t)desc->aux_ld;
  if (psyche_cublaslt_epilogue_uses_relu_aux_mask(desc->epilogue)) {
    if (cols > UINT64_MAX / aux_ld) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    aux_elements = cols * aux_ld;
    if (aux_elements == 0) {
      return CUBLAS_STATUS_SUCCESS;
    }
    if ((aux_elements - 1) / 8 > SIZE_MAX - 1) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    *bytes = (size_t)(((aux_elements - 1) / 8) + 1);
    return CUBLAS_STATUS_SUCCESS;
  }
  if (cols - 1 > UINT64_MAX / aux_ld) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  col_offset = (cols - 1) * aux_ld;
  if (col_offset > UINT64_MAX - (rows - 1)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  max_index = col_offset + (rows - 1);
  if (max_index == UINT64_MAX || max_index + 1 > SIZE_MAX / element_size) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *bytes = (size_t)(max_index + 1) * element_size;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_ranges_overlap(
    const void *left,
    size_t left_bytes,
    const void *right,
    size_t right_bytes,
    int *overlap) {
  uintptr_t left_start = (uintptr_t)left;
  uintptr_t right_start = (uintptr_t)right;
  uintptr_t left_end = 0;
  uintptr_t right_end = 0;
  if (overlap == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *overlap = 0;
  if (left == 0 || right == 0 || left_bytes == 0 || right_bytes == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (left_bytes > UINTPTR_MAX - left_start || right_bytes > UINTPTR_MAX - right_start) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  left_end = left_start + left_bytes;
  right_end = right_start + right_bytes;
  *overlap = left_start < right_end && right_start < left_end;
  return CUBLAS_STATUS_SUCCESS;
}

static const char *psyche_cublaslt_batch_ptr(
    const void *base,
    const PsycheCublasLtMatrixLayout *layout,
    uint32_t batch,
    size_t element_size) {
  uint32_t effective_batch = layout->batch_count == 1 ? 0u : batch;
  const void *entry = 0;
  if (base == 0) {
    return 0;
  }
  if (psyche_cublaslt_pointer_array_mode(layout->batch_mode)) {
    /* Host bridge convention: pointer-array mode receives a CPU-addressable array of native host pointers. */
    memcpy(&entry, (const char *)base + (size_t)effective_batch * sizeof(entry), sizeof(entry));
    return (const char *)entry;
  }
  return (const char *)base + (size_t)effective_batch * (size_t)layout->stride * element_size;
}

static char *psyche_cublaslt_batch_mut_ptr(
    void *base,
    const PsycheCublasLtMatrixLayout *layout,
    uint32_t batch,
    size_t element_size) {
  uint32_t effective_batch = layout->batch_count == 1 ? 0u : batch;
  void *entry = 0;
  if (base == 0) {
    return 0;
  }
  if (psyche_cublaslt_pointer_array_mode(layout->batch_mode)) {
    /* Host bridge convention: pointer-array mode receives a CPU-addressable array of native host pointers. */
    memcpy(&entry, (const char *)base + (size_t)effective_batch * sizeof(entry), sizeof(entry));
    return (char *)entry;
  }
  return (char *)base + (size_t)effective_batch * (size_t)layout->stride * element_size;
}

static cublasStatus_t psyche_cublaslt_bias_batch_ptr(
    const PsycheCublasLtMatmulDesc *desc,
    uint32_t batch,
    size_t element_size,
    const void **bias) {
  size_t byte_stride = 0;
  size_t byte_offset = 0;
  if (bias == 0 || element_size == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *bias = 0;
  if (!psyche_cublaslt_epilogue_uses_bias(desc->epilogue)) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (desc->bias_pointer == 0 || desc->bias_stride < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (desc->bias_stride == 0 || batch == 0) {
    *bias = (const void *)desc->bias_pointer;
    return CUBLAS_STATUS_SUCCESS;
  }
  if ((uint64_t)desc->bias_stride > SIZE_MAX / element_size) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  byte_stride = (size_t)desc->bias_stride * element_size;
  if ((size_t)batch > SIZE_MAX / byte_stride) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  byte_offset = (size_t)batch * byte_stride;
  *bias = (const char *)desc->bias_pointer + byte_offset;
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_aux_batch_ptr(
    const PsycheCublasLtMatmulDesc *desc,
    uint32_t batch,
    size_t element_size,
    void **aux) {
  size_t byte_stride = 0;
  size_t byte_offset = 0;
  if (aux == 0 || element_size == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *aux = 0;
  if (!psyche_cublaslt_epilogue_uses_aux(desc->epilogue)) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (desc->aux_pointer == 0 || desc->aux_ld <= 0 || desc->aux_stride < 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (desc->aux_stride == 0 || batch == 0) {
    *aux = (void *)desc->aux_pointer;
    return CUBLAS_STATUS_SUCCESS;
  }
  if (psyche_cublaslt_epilogue_uses_relu_aux_mask(desc->epilogue)) {
    uint64_t byte_stride64 = (uint64_t)desc->aux_stride / 8;
    if (byte_stride64 > SIZE_MAX) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    byte_stride = (size_t)byte_stride64;
  } else {
    if ((uint64_t)desc->aux_stride > SIZE_MAX / element_size) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    byte_stride = (size_t)desc->aux_stride * element_size;
  }
  if ((size_t)batch > SIZE_MAX / byte_stride) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  byte_offset = (size_t)batch * byte_stride;
  *aux = (char *)desc->aux_pointer + byte_offset;
  return CUBLAS_STATUS_SUCCESS;
}

static size_t psyche_cublaslt_relu_aux_byte_index(int64_t aux_ld, uint64_t row, uint64_t col) {
  uint64_t bit_index = row + col * (uint64_t)aux_ld;
  return (size_t)(bit_index / 8);
}

static unsigned char psyche_cublaslt_relu_aux_bit_mask(int64_t aux_ld, uint64_t row, uint64_t col) {
  uint64_t bit_index = row + col * (uint64_t)aux_ld;
  return (unsigned char)(1u << (unsigned)(bit_index & 7u));
}

static void psyche_cublaslt_relu_aux_write_bit(unsigned char *aux, int64_t aux_ld, uint64_t row, uint64_t col, int active) {
  size_t byte_index = psyche_cublaslt_relu_aux_byte_index(aux_ld, row, col);
  unsigned char bit_mask = psyche_cublaslt_relu_aux_bit_mask(aux_ld, row, col);
  if (active) {
    aux[byte_index] = (unsigned char)(aux[byte_index] | bit_mask);
  } else {
    aux[byte_index] = (unsigned char)(aux[byte_index] & (unsigned char)~bit_mask);
  }
}

static int psyche_cublaslt_relu_aux_read_bit(const unsigned char *aux, int64_t aux_ld, uint64_t row, uint64_t col) {
  size_t byte_index = psyche_cublaslt_relu_aux_byte_index(aux_ld, row, col);
  unsigned char bit_mask = psyche_cublaslt_relu_aux_bit_mask(aux_ld, row, col);
  return (aux[byte_index] & bit_mask) != 0;
}

static int psyche_cublaslt_s_relu_active(float value) {
  return value > 0.0f;
}

static int psyche_cublaslt_d_relu_active(double value) {
  return value > 0.0;
}

static float psyche_cublaslt_s_relu(float value) {
  /* NaN and negative zero are not greater than zero, matching cuBLASLt's ReLU clamp. */
  return psyche_cublaslt_s_relu_active(value) ? value : 0.0f;
}

static double psyche_cublaslt_d_relu(double value) {
  /* NaN and negative zero are not greater than zero, matching cuBLASLt's ReLU clamp. */
  return psyche_cublaslt_d_relu_active(value) ? value : 0.0;
}

static float psyche_cublaslt_s_gelu(float value) {
  /* cuBLASLt documents this tanh approximation for GELU epilogues. */
  if (isnan(value)) {
    return value;
  }
  if (isinf(value)) {
    return value > 0.0f ? value : 0.0f;
  }
  return 0.5f * value * (1.0f + tanhf(0.7978845608028654f * (value + 0.044715f * value * value * value)));
}

static double psyche_cublaslt_d_gelu(double value) {
  /* cuBLASLt documents this tanh approximation for GELU epilogues. */
  if (isnan(value)) {
    return value;
  }
  if (isinf(value)) {
    return value > 0.0 ? value : 0.0;
  }
  return 0.5 * value * (1.0 + tanh(0.79788456080286535588 * (value + 0.044715 * value * value * value)));
}

static float psyche_cublaslt_s_gelu_grad(float value) {
  float x2 = 0.0f;
  float inner = 0.0f;
  float t = 0.0f;
  float sech2 = 0.0f;
  if (isnan(value)) {
    return value;
  }
  if (isinf(value)) {
    return value > 0.0f ? 1.0f : 0.0f;
  }
  x2 = value * value;
  inner = 0.7978845608028654f * (value + 0.044715f * value * x2);
  t = tanhf(inner);
  sech2 = 1.0f - (t * t);
  return
      (0.5f * (1.0f + t)) +
      (0.5f * value * sech2 * 0.7978845608028654f * (1.0f + (3.0f * 0.044715f * x2)));
}

static double psyche_cublaslt_d_gelu_grad(double value) {
  double x2 = 0.0;
  double inner = 0.0;
  double t = 0.0;
  double sech2 = 0.0;
  if (isnan(value)) {
    return value;
  }
  if (isinf(value)) {
    return value > 0.0 ? 1.0 : 0.0;
  }
  x2 = value * value;
  inner = 0.79788456080286535588 * (value + 0.044715 * value * x2);
  t = tanh(inner);
  sech2 = 1.0 - (t * t);
  return
      (0.5 * (1.0 + t)) +
      (0.5 * value * sech2 * 0.79788456080286535588 * (1.0 + (3.0 * 0.044715 * x2)));
}

static size_t psyche_cublaslt_matrix_index(
    const PsycheCublasLtMatrixLayout *layout,
    uint64_t row,
    uint64_t col) {
  if (layout->order == CUBLASLT_ORDER_ROW) {
    return (size_t)row * (size_t)layout->ld + (size_t)col;
  }
  return (size_t)row + (size_t)col * (size_t)layout->ld;
}

static float psyche_cublaslt_s_raw_read(
    const float *matrix,
    const PsycheCublasLtMatrixLayout *layout,
    uint64_t row,
    uint64_t col) {
  return matrix[psyche_cublaslt_matrix_index(layout, row, col)];
}

static double psyche_cublaslt_d_raw_read(
    const double *matrix,
    const PsycheCublasLtMatrixLayout *layout,
    uint64_t row,
    uint64_t col) {
  return matrix[psyche_cublaslt_matrix_index(layout, row, col)];
}

static void psyche_cublaslt_s_raw_write(
    float *matrix,
    const PsycheCublasLtMatrixLayout *layout,
    uint64_t row,
    uint64_t col,
    float value) {
  matrix[psyche_cublaslt_matrix_index(layout, row, col)] = value;
}

static void psyche_cublaslt_d_raw_write(
    double *matrix,
    const PsycheCublasLtMatrixLayout *layout,
    uint64_t row,
    uint64_t col,
    double value) {
  matrix[psyche_cublaslt_matrix_index(layout, row, col)] = value;
}

static float psyche_cublaslt_s_read(
    const float *matrix,
    const PsycheCublasLtMatrixLayout *layout,
    cublasOperation_t op,
    uint64_t row,
    uint64_t col) {
  if (psyche_cublaslt_real_op(op) == CUBLAS_OP_N) {
    return psyche_cublaslt_s_raw_read(matrix, layout, row, col);
  }
  return psyche_cublaslt_s_raw_read(matrix, layout, col, row);
}

static double psyche_cublaslt_d_read(
    const double *matrix,
    const PsycheCublasLtMatrixLayout *layout,
    cublasOperation_t op,
    uint64_t row,
    uint64_t col) {
  if (psyche_cublaslt_real_op(op) == CUBLAS_OP_N) {
    return psyche_cublaslt_d_raw_read(matrix, layout, row, col);
  }
  return psyche_cublaslt_d_raw_read(matrix, layout, col, row);
}

static cublasStatus_t psyche_cublaslt_sgemm_one(
    const PsycheCublasLtMatmulDesc *desc,
    uint32_t batch,
    const float *alpha,
    const float *A,
    const PsycheCublasLtMatrixLayout *Adesc,
    const float *B,
    const PsycheCublasLtMatrixLayout *Bdesc,
    const float *beta,
    const float *C,
    const PsycheCublasLtMatrixLayout *Cdesc,
    float *D,
    const PsycheCublasLtMatrixLayout *Ddesc) {
  uint64_t row = 0;
  uint64_t col = 0;
  uint64_t inner = 0;
  uint64_t m = Ddesc->rows;
  uint64_t n = Ddesc->cols;
  uint64_t k = psyche_cublaslt_op_cols(Adesc, (cublasOperation_t)desc->transa);
  size_t tmp_bytes = 0;
  float *tmp = 0;
  const float *bias = 0;
  void *aux = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (*alpha != 0.0f && k > 0 && (A == 0 || B == 0)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublaslt_epilogue_writes_bgrada(desc->epilogue) && A == 0 && m > 0 && k > 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublaslt_epilogue_writes_bgradb(desc->epilogue) && B == 0 && n > 0 && k > 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (*beta != 0.0f && C == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (m == 0 || n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (D == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublaslt_aux_batch_ptr(desc, batch, sizeof(*tmp), &aux);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (float *)malloc(tmp_bytes);
  if (tmp == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  if (*beta != 0.0f) {
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        tmp[(size_t)row + (size_t)col * (size_t)m] = psyche_cublaslt_s_raw_read(C, Cdesc, row, col);
      }
    }
  } else {
    memset(tmp, 0, tmp_bytes);
  }
#if defined(__APPLE__)
  if (*alpha != 0.0f && k > 0 && psyche_cublaslt_all_column_major(Adesc, Bdesc, Cdesc, Ddesc)) {
    /* Accelerate computes the raw GEMM core; cuBLASLt epilogues are applied below. */
    cblas_sgemm(
        CblasColMajor,
        psyche_cublaslt_accelerate_trans((cublasOperation_t)desc->transa),
        psyche_cublaslt_accelerate_trans((cublasOperation_t)desc->transb),
        (int)m,
        (int)n,
        (int)k,
        *alpha,
        A,
        (int)Adesc->ld,
        B,
        (int)Bdesc->ld,
        *beta,
        tmp,
        (int)m);
  } else
#endif
  {
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        float acc = 0.0f;
        if (*alpha != 0.0f && k > 0) {
          for (inner = 0; inner < k; inner++) {
            acc +=
                psyche_cublaslt_s_read(A, Adesc, (cublasOperation_t)desc->transa, row, inner) *
                psyche_cublaslt_s_read(B, Bdesc, (cublasOperation_t)desc->transb, inner, col);
          }
        }
        tmp[(size_t)row + (size_t)col * (size_t)m] =
            (*alpha * acc) + (*beta == 0.0f ? 0.0f : *beta * tmp[(size_t)row + (size_t)col * (size_t)m]);
      }
    }
  }
  status = psyche_cublaslt_bias_batch_ptr(desc, batch, sizeof(*bias), (const void **)&bias);
  if (status != CUBLAS_STATUS_SUCCESS) {
    free(tmp);
    return status;
  }
  if (bias != 0 && psyche_cublaslt_epilogue_uses_bias_input(desc->epilogue)) {
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        tmp[(size_t)row + (size_t)col * (size_t)m] += bias[row];
      }
    }
  }
  if (psyche_cublaslt_epilogue_writes_output_bias_gradient(desc->epilogue)) {
    float *bias_gradient = (float *)bias;
    for (row = 0; row < m; row++) {
      double sum = 0.0;
      for (col = 0; col < n; col++) {
        sum += (double)tmp[(size_t)row + (size_t)col * (size_t)m];
      }
      bias_gradient[row] = (float)sum;
    }
  }
  if (psyche_cublaslt_epilogue_writes_bgrada(desc->epilogue)) {
    float *bias_gradient = (float *)bias;
    /* cuBLASLt BGRADA is a source-operand reduction over k, not an alpha/beta-scaled D reduction. */
    for (row = 0; row < m; row++) {
      double sum = 0.0;
      for (inner = 0; inner < k; inner++) {
        sum += (double)psyche_cublaslt_s_read(A, Adesc, (cublasOperation_t)desc->transa, row, inner);
      }
      bias_gradient[row] = (float)sum;
    }
  }
  if (psyche_cublaslt_epilogue_writes_bgradb(desc->epilogue)) {
    float *bias_gradient = (float *)bias;
    /* cuBLASLt BGRADB is a source-operand reduction over k, not an alpha/beta-scaled D reduction. */
    for (col = 0; col < n; col++) {
      double sum = 0.0;
      for (inner = 0; inner < k; inner++) {
        sum += (double)psyche_cublaslt_s_read(B, Bdesc, (cublasOperation_t)desc->transb, inner, col);
      }
      bias_gradient[col] = (float)sum;
    }
  }
  if (aux != 0 && psyche_cublaslt_epilogue_writes_gelu_aux(desc->epilogue)) {
    float *gelu_aux = (float *)aux;
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        /* cuBLASLt models GELU AUX as a logical column-major m-by-n matrix, independent of D's layout order. */
        gelu_aux[(size_t)row + (size_t)col * (size_t)desc->aux_ld] = tmp[(size_t)row + (size_t)col * (size_t)m];
      }
    }
  }
  if (psyche_cublaslt_epilogue_uses_relu(desc->epilogue)) {
    unsigned char *relu_aux = (unsigned char *)aux;
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        float value = tmp[(size_t)row + (size_t)col * (size_t)m];
        int active = psyche_cublaslt_s_relu_active(value);
        if (relu_aux != 0 && psyche_cublaslt_epilogue_writes_relu_aux_mask(desc->epilogue)) {
          psyche_cublaslt_relu_aux_write_bit(relu_aux, desc->aux_ld, row, col, active);
        }
        tmp[(size_t)row + (size_t)col * (size_t)m] = psyche_cublaslt_s_relu(value);
      }
    }
  }
  if (psyche_cublaslt_epilogue_uses_gelu(desc->epilogue)) {
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        tmp[(size_t)row + (size_t)col * (size_t)m] = psyche_cublaslt_s_gelu(tmp[(size_t)row + (size_t)col * (size_t)m]);
      }
    }
  }
  if (psyche_cublaslt_epilogue_uses_dgelu(desc->epilogue)) {
    const float *gelu_aux = (const float *)aux;
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        const float x = gelu_aux[(size_t)row + (size_t)col * (size_t)desc->aux_ld];
        tmp[(size_t)row + (size_t)col * (size_t)m] *= psyche_cublaslt_s_gelu_grad(x);
      }
    }
  }
  if (psyche_cublaslt_epilogue_uses_drelu(desc->epilogue)) {
    const unsigned char *relu_aux = (const unsigned char *)aux;
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        if (!psyche_cublaslt_relu_aux_read_bit(relu_aux, desc->aux_ld, row, col)) {
          tmp[(size_t)row + (size_t)col * (size_t)m] = 0.0f;
        }
      }
    }
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      psyche_cublaslt_s_raw_write(D, Ddesc, row, col, tmp[(size_t)row + (size_t)col * (size_t)m]);
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_dgemm_one(
    const PsycheCublasLtMatmulDesc *desc,
    uint32_t batch,
    const double *alpha,
    const double *A,
    const PsycheCublasLtMatrixLayout *Adesc,
    const double *B,
    const PsycheCublasLtMatrixLayout *Bdesc,
    const double *beta,
    const double *C,
    const PsycheCublasLtMatrixLayout *Cdesc,
    double *D,
    const PsycheCublasLtMatrixLayout *Ddesc) {
  uint64_t row = 0;
  uint64_t col = 0;
  uint64_t inner = 0;
  uint64_t m = Ddesc->rows;
  uint64_t n = Ddesc->cols;
  uint64_t k = psyche_cublaslt_op_cols(Adesc, (cublasOperation_t)desc->transa);
  size_t tmp_bytes = 0;
  double *tmp = 0;
  const double *bias = 0;
  void *aux = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (*alpha != 0.0 && k > 0 && (A == 0 || B == 0)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublaslt_epilogue_writes_bgrada(desc->epilogue) && A == 0 && m > 0 && k > 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublaslt_epilogue_writes_bgradb(desc->epilogue) && B == 0 && n > 0 && k > 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (*beta != 0.0 && C == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (m == 0 || n == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (D == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublaslt_aux_batch_ptr(desc, batch, sizeof(*tmp), &aux);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_temp_bytes(m, n, sizeof(*tmp), &tmp_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  tmp = (double *)malloc(tmp_bytes);
  if (tmp == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  if (*beta != 0.0) {
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        tmp[(size_t)row + (size_t)col * (size_t)m] = psyche_cublaslt_d_raw_read(C, Cdesc, row, col);
      }
    }
  } else {
    memset(tmp, 0, tmp_bytes);
  }
#if defined(__APPLE__)
  if (*alpha != 0.0 && k > 0 && psyche_cublaslt_all_column_major(Adesc, Bdesc, Cdesc, Ddesc)) {
    /* Accelerate computes the raw GEMM core; cuBLASLt epilogues are applied below. */
    cblas_dgemm(
        CblasColMajor,
        psyche_cublaslt_accelerate_trans((cublasOperation_t)desc->transa),
        psyche_cublaslt_accelerate_trans((cublasOperation_t)desc->transb),
        (int)m,
        (int)n,
        (int)k,
        *alpha,
        A,
        (int)Adesc->ld,
        B,
        (int)Bdesc->ld,
        *beta,
        tmp,
        (int)m);
  } else
#endif
  {
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        double acc = 0.0;
        if (*alpha != 0.0 && k > 0) {
          for (inner = 0; inner < k; inner++) {
            acc +=
                psyche_cublaslt_d_read(A, Adesc, (cublasOperation_t)desc->transa, row, inner) *
                psyche_cublaslt_d_read(B, Bdesc, (cublasOperation_t)desc->transb, inner, col);
          }
        }
        tmp[(size_t)row + (size_t)col * (size_t)m] =
            (*alpha * acc) + (*beta == 0.0 ? 0.0 : *beta * tmp[(size_t)row + (size_t)col * (size_t)m]);
      }
    }
  }
  status = psyche_cublaslt_bias_batch_ptr(desc, batch, sizeof(*bias), (const void **)&bias);
  if (status != CUBLAS_STATUS_SUCCESS) {
    free(tmp);
    return status;
  }
  if (bias != 0 && psyche_cublaslt_epilogue_uses_bias_input(desc->epilogue)) {
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        tmp[(size_t)row + (size_t)col * (size_t)m] += bias[row];
      }
    }
  }
  if (psyche_cublaslt_epilogue_writes_output_bias_gradient(desc->epilogue)) {
    double *bias_gradient = (double *)bias;
    for (row = 0; row < m; row++) {
      double sum = 0.0;
      for (col = 0; col < n; col++) {
        sum += tmp[(size_t)row + (size_t)col * (size_t)m];
      }
      bias_gradient[row] = sum;
    }
  }
  if (psyche_cublaslt_epilogue_writes_bgrada(desc->epilogue)) {
    double *bias_gradient = (double *)bias;
    /* cuBLASLt BGRADA is a source-operand reduction over k, not an alpha/beta-scaled D reduction. */
    for (row = 0; row < m; row++) {
      double sum = 0.0;
      for (inner = 0; inner < k; inner++) {
        sum += psyche_cublaslt_d_read(A, Adesc, (cublasOperation_t)desc->transa, row, inner);
      }
      bias_gradient[row] = sum;
    }
  }
  if (psyche_cublaslt_epilogue_writes_bgradb(desc->epilogue)) {
    double *bias_gradient = (double *)bias;
    /* cuBLASLt BGRADB is a source-operand reduction over k, not an alpha/beta-scaled D reduction. */
    for (col = 0; col < n; col++) {
      double sum = 0.0;
      for (inner = 0; inner < k; inner++) {
        sum += psyche_cublaslt_d_read(B, Bdesc, (cublasOperation_t)desc->transb, inner, col);
      }
      bias_gradient[col] = sum;
    }
  }
  if (aux != 0 && psyche_cublaslt_epilogue_writes_gelu_aux(desc->epilogue)) {
    double *gelu_aux = (double *)aux;
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        /* cuBLASLt models GELU AUX as a logical column-major m-by-n matrix, independent of D's layout order. */
        gelu_aux[(size_t)row + (size_t)col * (size_t)desc->aux_ld] = tmp[(size_t)row + (size_t)col * (size_t)m];
      }
    }
  }
  if (psyche_cublaslt_epilogue_uses_relu(desc->epilogue)) {
    unsigned char *relu_aux = (unsigned char *)aux;
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        double value = tmp[(size_t)row + (size_t)col * (size_t)m];
        int active = psyche_cublaslt_d_relu_active(value);
        if (relu_aux != 0 && psyche_cublaslt_epilogue_writes_relu_aux_mask(desc->epilogue)) {
          psyche_cublaslt_relu_aux_write_bit(relu_aux, desc->aux_ld, row, col, active);
        }
        tmp[(size_t)row + (size_t)col * (size_t)m] = psyche_cublaslt_d_relu(value);
      }
    }
  }
  if (psyche_cublaslt_epilogue_uses_gelu(desc->epilogue)) {
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        tmp[(size_t)row + (size_t)col * (size_t)m] = psyche_cublaslt_d_gelu(tmp[(size_t)row + (size_t)col * (size_t)m]);
      }
    }
  }
  if (psyche_cublaslt_epilogue_uses_dgelu(desc->epilogue)) {
    const double *gelu_aux = (const double *)aux;
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        const double x = gelu_aux[(size_t)row + (size_t)col * (size_t)desc->aux_ld];
        tmp[(size_t)row + (size_t)col * (size_t)m] *= psyche_cublaslt_d_gelu_grad(x);
      }
    }
  }
  if (psyche_cublaslt_epilogue_uses_drelu(desc->epilogue)) {
    const unsigned char *relu_aux = (const unsigned char *)aux;
    for (col = 0; col < n; col++) {
      for (row = 0; row < m; row++) {
        if (!psyche_cublaslt_relu_aux_read_bit(relu_aux, desc->aux_ld, row, col)) {
          tmp[(size_t)row + (size_t)col * (size_t)m] = 0.0;
        }
      }
    }
  }
  for (col = 0; col < n; col++) {
    for (row = 0; row < m; row++) {
      psyche_cublaslt_d_raw_write(D, Ddesc, row, col, tmp[(size_t)row + (size_t)col * (size_t)m]);
    }
  }
  free(tmp);
  return CUBLAS_STATUS_SUCCESS;
}

static int psyche_cublaslt_layout_matrix_values_equal(
    const PsycheCublasLtMatrixLayout *left,
    const PsycheCublasLtMatrixLayout *right) {
  if (left == 0 || right == 0) {
    return 0;
  }
  return
      left->type == right->type &&
      left->order == right->order &&
      left->rows == right->rows &&
      left->cols == right->cols &&
      left->ld == right->ld;
}

static int psyche_cublaslt_layout_values_equal(
    const PsycheCublasLtMatrixLayout *left,
    const PsycheCublasLtMatrixLayout *right) {
  return
      psyche_cublaslt_layout_matrix_values_equal(left, right) &&
      left->batch_count == right->batch_count &&
      left->batch_mode == right->batch_mode &&
      left->stride == right->stride;
}

static cublasStatus_t psyche_cublaslt_layout_batch_span_bytes(
    const PsycheCublasLtMatrixLayout *layout,
    size_t matrix_bytes,
    size_t element_size,
    size_t *span_bytes) {
  uint64_t stride_elements = 0;
  size_t stride_bytes = 0;
  size_t batch_offset_bytes = 0;
  uint32_t batch_count = 0;
  if (layout == 0 || span_bytes == 0 || element_size == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *span_bytes = 0;
  batch_count = layout->batch_count;
  if (batch_count == 0 || matrix_bytes == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  *span_bytes = matrix_bytes;
  if (batch_count == 1 || layout->stride == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  stride_elements = (uint64_t)layout->stride;
  if (stride_elements > SIZE_MAX / element_size) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  stride_bytes = (size_t)stride_elements * element_size;
  if ((size_t)(batch_count - 1) > SIZE_MAX / stride_bytes) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  batch_offset_bytes = (size_t)(batch_count - 1) * stride_bytes;
  if (batch_offset_bytes > SIZE_MAX - matrix_bytes) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *span_bytes = batch_offset_bytes + matrix_bytes;
  return CUBLAS_STATUS_SUCCESS;
}

static double psyche_cublaslt_read_as_double(
    const void *matrix,
    const PsycheCublasLtMatrixLayout *layout,
    cublasOperation_t op,
    uint64_t row,
    uint64_t col) {
  if ((cudaDataType_t)layout->type == CUDA_R_32F) {
    return (double)psyche_cublaslt_s_read((const float *)matrix, layout, op, row, col);
  }
  return psyche_cublaslt_d_read((const double *)matrix, layout, op, row, col);
}

static void psyche_cublaslt_write_from_double(
    void *matrix,
    const PsycheCublasLtMatrixLayout *layout,
    uint64_t row,
    uint64_t col,
    double value) {
  if ((cudaDataType_t)layout->type == CUDA_R_32F) {
    psyche_cublaslt_s_raw_write((float *)matrix, layout, row, col, (float)value);
    return;
  }
  psyche_cublaslt_d_raw_write((double *)matrix, layout, row, col, value);
}

static cublasStatus_t psyche_cublaslt_transform_check_strided_overlap(
    const void *source,
    const PsycheCublasLtMatrixLayout *source_layout,
    cublasOperation_t source_op,
    size_t source_matrix_bytes,
    size_t source_element_size,
    const void *output,
    const PsycheCublasLtMatrixLayout *output_layout,
    size_t output_matrix_bytes,
    size_t output_element_size,
    int *overlap_out) {
  size_t source_span = 0;
  size_t output_span = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (overlap_out == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *overlap_out = 0;
  if (source == 0 || output == 0 || source_matrix_bytes == 0 || output_matrix_bytes == 0) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (
      source == output &&
      psyche_cublaslt_real_op(source_op) == CUBLAS_OP_N &&
      source_element_size == output_element_size &&
      psyche_cublaslt_layout_values_equal(source_layout, output_layout)) {
    return CUBLAS_STATUS_SUCCESS;
  }
  status = psyche_cublaslt_layout_batch_span_bytes(source_layout, source_matrix_bytes, source_element_size, &source_span);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_layout_batch_span_bytes(output_layout, output_matrix_bytes, output_element_size, &output_span);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  return psyche_cublaslt_ranges_overlap(source, source_span, output, output_span, overlap_out);
}

static cublasStatus_t psyche_cublaslt_transform_check_pointer_array_overlap(
    const void *source_base,
    const PsycheCublasLtMatrixLayout *source_layout,
    cublasOperation_t source_op,
    size_t source_matrix_bytes,
    size_t source_element_size,
    void *output_base,
    const PsycheCublasLtMatrixLayout *output_layout,
    size_t output_matrix_bytes,
    size_t output_element_size,
    uint32_t batch_count,
    int *overlap_out) {
  uint32_t batch = 0;
  if (overlap_out == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *overlap_out = 0;
  for (batch = 0; batch < batch_count; batch++) {
    const void *source = psyche_cublaslt_batch_ptr(source_base, source_layout, batch, source_element_size);
    void *output = psyche_cublaslt_batch_mut_ptr(output_base, output_layout, batch, output_element_size);
    int overlap = 0;
    cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
    if (source_matrix_bytes != 0 && source == 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (output_matrix_bytes != 0 && output == 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (
        source == output &&
        psyche_cublaslt_real_op(source_op) == CUBLAS_OP_N &&
        source_element_size == output_element_size &&
        psyche_cublaslt_layout_matrix_values_equal(source_layout, output_layout)) {
      continue;
    }
    status = psyche_cublaslt_ranges_overlap(source, source_matrix_bytes, output, output_matrix_bytes, &overlap);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (overlap) {
      *overlap_out = 1;
      return CUBLAS_STATUS_SUCCESS;
    }
  }
  return CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_transform_check_source_overlap(
    const void *source_base,
    const PsycheCublasLtMatrixLayout *source_layout,
    cublasOperation_t source_op,
    size_t source_matrix_bytes,
    size_t source_element_size,
    void *output_base,
    const PsycheCublasLtMatrixLayout *output_layout,
    size_t output_matrix_bytes,
    size_t output_element_size,
    uint32_t batch_count) {
  int overlap = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (psyche_cublaslt_pointer_array_mode(output_layout->batch_mode)) {
    status = psyche_cublaslt_transform_check_pointer_array_overlap(
        source_base,
        source_layout,
        source_op,
        source_matrix_bytes,
        source_element_size,
        output_base,
        output_layout,
        output_matrix_bytes,
        output_element_size,
        batch_count,
        &overlap);
  } else {
    status = psyche_cublaslt_transform_check_strided_overlap(
        source_base,
        source_layout,
        source_op,
        source_matrix_bytes,
        source_element_size,
        output_base,
        output_layout,
        output_matrix_bytes,
        output_element_size,
        &overlap);
  }
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  return overlap ? CUBLAS_STATUS_NOT_SUPPORTED : CUBLAS_STATUS_SUCCESS;
}

static cublasStatus_t psyche_cublaslt_validate_transform_source(
    const PsycheCublasLtMatrixLayout *source_layout,
    cublasOperation_t source_op,
    const PsycheCublasLtMatrixLayout *output_layout,
    uint32_t output_batch_count) {
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  status = psyche_cublaslt_validate_layout_shape(source_layout);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (source_layout->batch_mode != output_layout->batch_mode) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (psyche_cublaslt_op_rows(source_layout, source_op) != output_layout->rows ||
      psyche_cublaslt_op_cols(source_layout, source_op) != output_layout->cols) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (psyche_cublaslt_pointer_array_mode(output_layout->batch_mode)) {
    return source_layout->batch_count == output_batch_count ? CUBLAS_STATUS_SUCCESS : CUBLAS_STATUS_INVALID_VALUE;
  }
  return psyche_cublaslt_batch_compatible(source_layout->batch_count, output_batch_count) ?
      CUBLAS_STATUS_SUCCESS :
      CUBLAS_STATUS_INVALID_VALUE;
}

static cublasStatus_t psyche_cublaslt_transform_preflight_pointers(
    const void *A,
    const PsycheCublasLtMatrixLayout *a_layout,
    int alpha_zero,
    size_t a_matrix_bytes,
    size_t a_element_size,
    const void *B,
    const PsycheCublasLtMatrixLayout *b_layout,
    int beta_zero,
    size_t b_matrix_bytes,
    size_t b_element_size,
    void *C,
    const PsycheCublasLtMatrixLayout *c_layout,
    size_t c_matrix_bytes,
    size_t c_element_size,
    uint32_t batch_count) {
  uint32_t batch = 0;
  for (batch = 0; batch < batch_count; batch++) {
    void *c_batch = psyche_cublaslt_batch_mut_ptr(C, c_layout, batch, c_element_size);
    if (c_matrix_bytes != 0 && c_batch == 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (!alpha_zero) {
      const void *a_batch = psyche_cublaslt_batch_ptr(A, a_layout, batch, a_element_size);
      if (a_matrix_bytes != 0 && a_batch == 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
    }
    if (!beta_zero) {
      const void *b_batch = psyche_cublaslt_batch_ptr(B, b_layout, batch, b_element_size);
      if (b_matrix_bytes != 0 && b_batch == 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
    }
  }
  return CUBLAS_STATUS_SUCCESS;
}

static void psyche_cublaslt_transform_one(
    const PsycheCublasLtMatrixTransformDesc *desc,
    const void *alpha,
    const void *A,
    const PsycheCublasLtMatrixLayout *Adesc,
    const void *beta,
    const void *B,
    const PsycheCublasLtMatrixLayout *Bdesc,
    void *C,
    const PsycheCublasLtMatrixLayout *Cdesc,
    int alpha_zero,
    int beta_zero) {
  uint64_t row = 0;
  uint64_t col = 0;
  for (col = 0; col < Cdesc->cols; col++) {
    for (row = 0; row < Cdesc->rows; row++) {
      if ((cudaDataType_t)desc->scale_type == CUDA_R_32F) {
        float a = alpha_zero ? 0.0f : (float)psyche_cublaslt_read_as_double(A, Adesc, (cublasOperation_t)desc->transa, row, col);
        float b = beta_zero ? 0.0f : (float)psyche_cublaslt_read_as_double(B, Bdesc, (cublasOperation_t)desc->transb, row, col);
        float result = (*(const float *)alpha * a) + (*(const float *)beta * b);
        psyche_cublaslt_write_from_double(C, Cdesc, row, col, (double)result);
      } else {
        double a = alpha_zero ? 0.0 : psyche_cublaslt_read_as_double(A, Adesc, (cublasOperation_t)desc->transa, row, col);
        double b = beta_zero ? 0.0 : psyche_cublaslt_read_as_double(B, Bdesc, (cublasOperation_t)desc->transb, row, col);
        double result = (*(const double *)alpha * a) + (*(const double *)beta * b);
        psyche_cublaslt_write_from_double(C, Cdesc, row, col, result);
      }
    }
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtCreate(cublasLtHandle_t *lightHandle) {
  PsycheCublasLtContext *ctx = 0;
  if (lightHandle == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *lightHandle = 0;
  if (!psyche_cublaslt_simulated_memory_enabled()) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  ctx = (PsycheCublasLtContext *)malloc(sizeof(*ctx));
  if (ctx == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  ctx->magic = PSYCHE_CUBLASLT_HANDLE_MAGIC;
  ctx->next = 0;
  psyche_cublaslt_register_context(ctx);
  *lightHandle = (cublasLtHandle_t)ctx;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtDestroy(cublasLtHandle_t lightHandle) {
  PsycheCublasLtContext *ctx = psyche_cublaslt_unregister_context(lightHandle);
  if (ctx == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  ctx->magic = 0;
  free(ctx);
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API const char *cublasLtGetStatusName(cublasStatus_t status) {
  switch (status) {
  case CUBLAS_STATUS_SUCCESS:
    return "CUBLAS_STATUS_SUCCESS";
  case CUBLAS_STATUS_NOT_INITIALIZED:
    return "CUBLAS_STATUS_NOT_INITIALIZED";
  case CUBLAS_STATUS_ALLOC_FAILED:
    return "CUBLAS_STATUS_ALLOC_FAILED";
  case CUBLAS_STATUS_INVALID_VALUE:
    return "CUBLAS_STATUS_INVALID_VALUE";
  case CUBLAS_STATUS_ARCH_MISMATCH:
    return "CUBLAS_STATUS_ARCH_MISMATCH";
  case CUBLAS_STATUS_MAPPING_ERROR:
    return "CUBLAS_STATUS_MAPPING_ERROR";
  case CUBLAS_STATUS_EXECUTION_FAILED:
    return "CUBLAS_STATUS_EXECUTION_FAILED";
  case CUBLAS_STATUS_INTERNAL_ERROR:
    return "CUBLAS_STATUS_INTERNAL_ERROR";
  case CUBLAS_STATUS_NOT_SUPPORTED:
    return "CUBLAS_STATUS_NOT_SUPPORTED";
  case CUBLAS_STATUS_LICENSE_ERROR:
    return "CUBLAS_STATUS_LICENSE_ERROR";
  default:
    return "CUBLAS_STATUS_UNKNOWN";
  }
}

PSYCHE_CUDA_STUB_API const char *cublasLtGetStatusString(cublasStatus_t status) {
  switch (status) {
  case CUBLAS_STATUS_SUCCESS:
    return "the operation completed successfully";
  case CUBLAS_STATUS_NOT_INITIALIZED:
    return "cuBLASLt was not initialized";
  case CUBLAS_STATUS_ALLOC_FAILED:
    return "resource allocation failed";
  case CUBLAS_STATUS_INVALID_VALUE:
    return "invalid value";
  case CUBLAS_STATUS_NOT_SUPPORTED:
    return "operation is not supported by the Psyche cuBLASLt shim";
  default:
    return cublasLtGetStatusName(status);
  }
}

PSYCHE_CUDA_STUB_API size_t cublasLtGetVersion(void) {
  return PSYCHE_CUBLASLT_VERSION;
}

PSYCHE_CUDA_STUB_API size_t cublasLtGetCudartVersion(void) {
  return 0;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtGetProperty(libraryPropertyType type, int *value) {
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
    return CUBLAS_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixLayoutInit_internal(
    cublasLtMatrixLayout_t matLayout,
    size_t size,
    cudaDataType_t type,
    uint64_t rows,
    uint64_t cols,
    int64_t ld) {
  return psyche_cublaslt_init_layout(matLayout, size, type, rows, cols, ld, 0);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixLayoutCreate(
    cublasLtMatrixLayout_t *matLayout,
    cudaDataType_t type,
    uint64_t rows,
    uint64_t cols,
    int64_t ld) {
  cublasLtMatrixLayout_t layout = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (matLayout == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *matLayout = 0;
  layout = (cublasLtMatrixLayout_t)calloc(1, sizeof(cublasLtMatrixLayoutOpaque_t));
  if (layout == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  status = psyche_cublaslt_init_layout(layout, sizeof(cublasLtMatrixLayoutOpaque_t), type, rows, cols, ld, 1);
  if (status != CUBLAS_STATUS_SUCCESS) {
    free(layout);
    return status;
  }
  *matLayout = layout;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixLayoutDestroy(cublasLtMatrixLayout_t matLayout) {
  PsycheCublasLtMatrixLayout *layout = (PsycheCublasLtMatrixLayout *)matLayout;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(matLayout, PSYCHE_CUBLASLT_KIND_LAYOUT);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  layout->magic = 0;
  if (layout->owns_allocation) {
    free(layout);
  }
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixLayoutSetAttribute(
    cublasLtMatrixLayout_t matLayout,
    cublasLtMatrixLayoutAttribute_t attr,
    const void *buf,
    size_t sizeInBytes) {
  PsycheCublasLtMatrixLayout *layout = (PsycheCublasLtMatrixLayout *)matLayout;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(matLayout, PSYCHE_CUBLASLT_KIND_LAYOUT);
  uint32_t value32 = 0;
  int64_t value64 = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  switch (attr) {
  case CUBLASLT_MATRIX_LAYOUT_ORDER:
    status = psyche_cublaslt_set_u32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value32 != CUBLASLT_ORDER_COL && value32 != CUBLASLT_ORDER_ROW) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (value32 == CUBLASLT_ORDER_COL && layout->rows > 0 && (uint64_t)layout->ld < layout->rows) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (value32 == CUBLASLT_ORDER_ROW && layout->cols > 0 && (uint64_t)layout->ld < layout->cols) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    layout->order = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT:
    status = psyche_cublaslt_set_u32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    layout->batch_count = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET:
    status = psyche_cublaslt_set_i64(&value64, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value64 < 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    layout->stride = value64;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATRIX_LAYOUT_BATCH_MODE:
    status = psyche_cublaslt_set_u32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value32 != CUBLASLT_BATCH_MODE_STRIDED && value32 != CUBLASLT_BATCH_MODE_POINTER_ARRAY) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    layout->batch_mode = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATRIX_LAYOUT_PLANE_OFFSET:
    status = psyche_cublaslt_set_i64(&value64, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    return value64 == 0 ? CUBLAS_STATUS_SUCCESS : CUBLAS_STATUS_NOT_SUPPORTED;
  case CUBLASLT_MATRIX_LAYOUT_TYPE:
  case CUBLASLT_MATRIX_LAYOUT_ROWS:
  case CUBLASLT_MATRIX_LAYOUT_COLS:
  case CUBLASLT_MATRIX_LAYOUT_LD:
    return CUBLAS_STATUS_NOT_SUPPORTED;
  default:
    return CUBLAS_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixLayoutGetAttribute(
    cublasLtMatrixLayout_t matLayout,
    cublasLtMatrixLayoutAttribute_t attr,
    void *buf,
    size_t sizeInBytes,
    size_t *sizeWritten) {
  const PsycheCublasLtMatrixLayout *layout = (const PsycheCublasLtMatrixLayout *)matLayout;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(matLayout, PSYCHE_CUBLASLT_KIND_LAYOUT);
  int64_t zero64 = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  switch (attr) {
  case CUBLASLT_MATRIX_LAYOUT_TYPE:
    return psyche_cublaslt_get_attr_value(&layout->type, sizeof(layout->type), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_LAYOUT_ORDER:
    return psyche_cublaslt_get_attr_value(&layout->order, sizeof(layout->order), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_LAYOUT_ROWS:
    return psyche_cublaslt_get_attr_value(&layout->rows, sizeof(layout->rows), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_LAYOUT_COLS:
    return psyche_cublaslt_get_attr_value(&layout->cols, sizeof(layout->cols), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_LAYOUT_LD:
    return psyche_cublaslt_get_attr_value(&layout->ld, sizeof(layout->ld), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT:
    return psyche_cublaslt_get_attr_value(&layout->batch_count, sizeof(layout->batch_count), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET:
    return psyche_cublaslt_get_attr_value(&layout->stride, sizeof(layout->stride), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_LAYOUT_PLANE_OFFSET:
    return psyche_cublaslt_get_attr_value(&zero64, sizeof(zero64), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_LAYOUT_BATCH_MODE:
    return psyche_cublaslt_get_attr_value(&layout->batch_mode, sizeof(layout->batch_mode), buf, sizeInBytes, sizeWritten);
  default:
    return CUBLAS_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixTransformDescInit_internal(
    cublasLtMatrixTransformDesc_t transformDesc,
    size_t size,
    cudaDataType_t scaleType) {
  return psyche_cublaslt_init_transform_desc(transformDesc, size, scaleType, 0);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixTransformDescCreate(
    cublasLtMatrixTransformDesc_t *transformDesc,
    cudaDataType_t scaleType) {
  cublasLtMatrixTransformDesc_t desc = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (transformDesc == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *transformDesc = 0;
  desc = (cublasLtMatrixTransformDesc_t)calloc(1, sizeof(cublasLtMatrixTransformDescOpaque_t));
  if (desc == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  status = psyche_cublaslt_init_transform_desc(desc, sizeof(cublasLtMatrixTransformDescOpaque_t), scaleType, 1);
  if (status != CUBLAS_STATUS_SUCCESS) {
    free(desc);
    return status;
  }
  *transformDesc = desc;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixTransformDescDestroy(
    cublasLtMatrixTransformDesc_t transformDesc) {
  PsycheCublasLtMatrixTransformDesc *desc = (PsycheCublasLtMatrixTransformDesc *)transformDesc;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(transformDesc, PSYCHE_CUBLASLT_KIND_TRANSFORM_DESC);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  desc->magic = 0;
  if (desc->owns_allocation) {
    free(desc);
  }
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixTransformDescSetAttribute(
    cublasLtMatrixTransformDesc_t transformDesc,
    cublasLtMatrixTransformDescAttributes_t attr,
    const void *buf,
    size_t sizeInBytes) {
  PsycheCublasLtMatrixTransformDesc *desc = (PsycheCublasLtMatrixTransformDesc *)transformDesc;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(transformDesc, PSYCHE_CUBLASLT_KIND_TRANSFORM_DESC);
  int32_t value32 = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  switch (attr) {
  case CUBLASLT_MATRIX_TRANSFORM_DESC_SCALE_TYPE:
    status = psyche_cublaslt_set_i32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (!psyche_cublaslt_valid_real_data_type((cudaDataType_t)value32)) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    desc->scale_type = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATRIX_TRANSFORM_DESC_POINTER_MODE:
    status = psyche_cublaslt_set_i32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value32 != CUBLASLT_POINTER_MODE_HOST && value32 != CUBLASLT_POINTER_MODE_DEVICE) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    desc->pointer_mode = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATRIX_TRANSFORM_DESC_TRANSA:
    status = psyche_cublaslt_set_i32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (!psyche_cublaslt_valid_op((cublasOperation_t)value32)) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    desc->transa = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATRIX_TRANSFORM_DESC_TRANSB:
    status = psyche_cublaslt_set_i32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (!psyche_cublaslt_valid_op((cublasOperation_t)value32)) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    desc->transb = value32;
    return CUBLAS_STATUS_SUCCESS;
  default:
    return CUBLAS_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixTransformDescGetAttribute(
    cublasLtMatrixTransformDesc_t transformDesc,
    cublasLtMatrixTransformDescAttributes_t attr,
    void *buf,
    size_t sizeInBytes,
    size_t *sizeWritten) {
  const PsycheCublasLtMatrixTransformDesc *desc = (const PsycheCublasLtMatrixTransformDesc *)transformDesc;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(transformDesc, PSYCHE_CUBLASLT_KIND_TRANSFORM_DESC);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  switch (attr) {
  case CUBLASLT_MATRIX_TRANSFORM_DESC_SCALE_TYPE:
    return psyche_cublaslt_get_attr_value(&desc->scale_type, sizeof(desc->scale_type), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_TRANSFORM_DESC_POINTER_MODE:
    return psyche_cublaslt_get_attr_value(&desc->pointer_mode, sizeof(desc->pointer_mode), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_TRANSFORM_DESC_TRANSA:
    return psyche_cublaslt_get_attr_value(&desc->transa, sizeof(desc->transa), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATRIX_TRANSFORM_DESC_TRANSB:
    return psyche_cublaslt_get_attr_value(&desc->transb, sizeof(desc->transb), buf, sizeInBytes, sizeWritten);
  default:
    return CUBLAS_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatrixTransform(
    cublasLtHandle_t lightHandle,
    cublasLtMatrixTransformDesc_t transformDesc,
    const void *alpha,
    const void *A,
    cublasLtMatrixLayout_t Adesc,
    const void *beta,
    const void *B,
    cublasLtMatrixLayout_t Bdesc,
    void *C,
    cublasLtMatrixLayout_t Cdesc,
    cudaStream_t stream) {
  const PsycheCublasLtMatrixTransformDesc *desc = (const PsycheCublasLtMatrixTransformDesc *)transformDesc;
  const PsycheCublasLtMatrixLayout *a_layout = (const PsycheCublasLtMatrixLayout *)Adesc;
  const PsycheCublasLtMatrixLayout *b_layout = (const PsycheCublasLtMatrixLayout *)Bdesc;
  const PsycheCublasLtMatrixLayout *c_layout = (const PsycheCublasLtMatrixLayout *)Cdesc;
  cudaDataType_t scale_type = CUDA_R_32F;
  uint32_t batch_count = 0;
  size_t a_element_size = 0;
  size_t b_element_size = 0;
  size_t c_element_size = 0;
  size_t a_matrix_bytes = 0;
  size_t b_matrix_bytes = 0;
  size_t c_matrix_bytes = 0;
  int alpha_zero = 0;
  int beta_zero = 0;
  uint32_t batch = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  (void)stream;

  if (psyche_cublaslt_context(lightHandle) == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  status = psyche_cublaslt_validate_desc_header(transformDesc, PSYCHE_CUBLASLT_KIND_TRANSFORM_DESC);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_validate_desc_header(Cdesc, PSYCHE_CUBLASLT_KIND_LAYOUT);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (alpha == 0 || beta == 0 || C == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (!psyche_cublaslt_valid_op((cublasOperation_t)desc->transa) ||
      !psyche_cublaslt_valid_op((cublasOperation_t)desc->transb)) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (desc->pointer_mode != CUBLASLT_POINTER_MODE_HOST) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  scale_type = (cudaDataType_t)desc->scale_type;
  if (!psyche_cublaslt_valid_real_data_type(scale_type)) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  if (scale_type == CUDA_R_32F) {
    alpha_zero = *(const float *)alpha == 0.0f;
    beta_zero = *(const float *)beta == 0.0f;
  } else {
    alpha_zero = *(const double *)alpha == 0.0;
    beta_zero = *(const double *)beta == 0.0;
  }

  status = psyche_cublaslt_validate_layout_shape(c_layout);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  batch_count = c_layout->batch_count;
  if (!psyche_cublaslt_data_type_size((cudaDataType_t)c_layout->type, &c_element_size)) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  status = psyche_cublaslt_layout_matrix_bytes(c_layout, c_element_size, &c_matrix_bytes);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!psyche_cublaslt_pointer_array_mode(c_layout->batch_mode) && batch_count > 1 && c_layout->stride == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (!psyche_cublaslt_pointer_array_mode(c_layout->batch_mode) && batch_count > 1 && c_matrix_bytes != 0) {
    uint64_t stride_elements = (uint64_t)c_layout->stride;
    size_t stride_bytes = 0;
    if (stride_elements > SIZE_MAX / c_element_size) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    stride_bytes = (size_t)stride_elements * c_element_size;
    if (stride_bytes < c_matrix_bytes) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
  }

  if (!alpha_zero || A != 0) {
    status = psyche_cublaslt_validate_desc_header(Adesc, PSYCHE_CUBLASLT_KIND_LAYOUT);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    status = psyche_cublaslt_validate_layout_shape(a_layout);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (!psyche_cublaslt_data_type_size((cudaDataType_t)a_layout->type, &a_element_size)) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    status = psyche_cublaslt_layout_matrix_bytes(a_layout, a_element_size, &a_matrix_bytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (!alpha_zero) {
      if (A == 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      status = psyche_cublaslt_validate_transform_source(
          a_layout,
          (cublasOperation_t)desc->transa,
          c_layout,
          batch_count);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
    }
  } else {
    a_layout = 0;
  }

  if (!beta_zero || B != 0) {
    status = psyche_cublaslt_validate_desc_header(Bdesc, PSYCHE_CUBLASLT_KIND_LAYOUT);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    status = psyche_cublaslt_validate_layout_shape(b_layout);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (!psyche_cublaslt_data_type_size((cudaDataType_t)b_layout->type, &b_element_size)) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    status = psyche_cublaslt_layout_matrix_bytes(b_layout, b_element_size, &b_matrix_bytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (!beta_zero) {
      if (B == 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      status = psyche_cublaslt_validate_transform_source(
          b_layout,
          (cublasOperation_t)desc->transb,
          c_layout,
          batch_count);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
    }
  } else {
    b_layout = 0;
  }

  status = psyche_cublaslt_transform_preflight_pointers(
      A,
      a_layout,
      alpha_zero,
      a_matrix_bytes,
      a_element_size == 0 ? c_element_size : a_element_size,
      B,
      b_layout,
      beta_zero,
      b_matrix_bytes,
      b_element_size == 0 ? c_element_size : b_element_size,
      C,
      c_layout,
      c_matrix_bytes,
      c_element_size,
      batch_count);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (!alpha_zero) {
    status = psyche_cublaslt_transform_check_source_overlap(
        A,
        a_layout,
        (cublasOperation_t)desc->transa,
        a_matrix_bytes,
        a_element_size,
        C,
        c_layout,
        c_matrix_bytes,
        c_element_size,
        batch_count);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  if (!beta_zero) {
    status = psyche_cublaslt_transform_check_source_overlap(
        B,
        b_layout,
        (cublasOperation_t)desc->transb,
        b_matrix_bytes,
        b_element_size,
        C,
        c_layout,
        c_matrix_bytes,
        c_element_size,
        batch_count);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }

  for (batch = 0; batch < batch_count; batch++) {
    const void *a_batch = alpha_zero ? 0 : psyche_cublaslt_batch_ptr(A, a_layout, batch, a_element_size);
    const void *b_batch = beta_zero ? 0 : psyche_cublaslt_batch_ptr(B, b_layout, batch, b_element_size);
    void *c_batch = psyche_cublaslt_batch_mut_ptr(C, c_layout, batch, c_element_size);
    psyche_cublaslt_transform_one(
        desc,
        alpha,
        a_batch,
        a_layout,
        beta,
        b_batch,
        b_layout,
        c_batch,
        c_layout,
        alpha_zero,
        beta_zero);
  }
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulDescInit_internal(
    cublasLtMatmulDesc_t matmulDesc,
    size_t size,
    cublasComputeType_t computeType,
    cudaDataType_t scaleType) {
  return psyche_cublaslt_init_matmul_desc(matmulDesc, size, computeType, scaleType, 0);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulDescCreate(
    cublasLtMatmulDesc_t *matmulDesc,
    cublasComputeType_t computeType,
    cudaDataType_t scaleType) {
  cublasLtMatmulDesc_t desc = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (matmulDesc == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *matmulDesc = 0;
  desc = (cublasLtMatmulDesc_t)calloc(1, sizeof(cublasLtMatmulDescOpaque_t));
  if (desc == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  status = psyche_cublaslt_init_matmul_desc(desc, sizeof(cublasLtMatmulDescOpaque_t), computeType, scaleType, 1);
  if (status != CUBLAS_STATUS_SUCCESS) {
    free(desc);
    return status;
  }
  *matmulDesc = desc;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulDescDestroy(cublasLtMatmulDesc_t matmulDesc) {
  PsycheCublasLtMatmulDesc *desc = (PsycheCublasLtMatmulDesc *)matmulDesc;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(matmulDesc, PSYCHE_CUBLASLT_KIND_MATMUL_DESC);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  desc->magic = 0;
  if (desc->owns_allocation) {
    free(desc);
  }
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulDescSetAttribute(
    cublasLtMatmulDesc_t matmulDesc,
    cublasLtMatmulDescAttributes_t attr,
    const void *buf,
    size_t sizeInBytes) {
  PsycheCublasLtMatmulDesc *desc = (PsycheCublasLtMatmulDesc *)matmulDesc;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(matmulDesc, PSYCHE_CUBLASLT_KIND_MATMUL_DESC);
  int32_t value32 = 0;
  uint32_t valueu32 = 0;
  uintptr_t valueptr = 0;
  int64_t value64 = 0;
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  switch (attr) {
  case CUBLASLT_MATMUL_DESC_POINTER_MODE:
    status = psyche_cublaslt_set_i32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value32 != CUBLASLT_POINTER_MODE_HOST) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    desc->pointer_mode = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_TRANSA:
    status = psyche_cublaslt_set_i32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (!psyche_cublaslt_valid_op((cublasOperation_t)value32)) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    desc->transa = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_TRANSB:
    status = psyche_cublaslt_set_i32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (!psyche_cublaslt_valid_op((cublasOperation_t)value32)) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    desc->transb = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_TRANSC:
    status = psyche_cublaslt_set_i32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value32 != CUBLAS_OP_N) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    desc->transc = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_EPILOGUE:
    status = psyche_cublaslt_set_u32(&valueu32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (!psyche_cublaslt_supported_epilogue(valueu32)) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    desc->epilogue = valueu32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_BIAS_POINTER:
    status = psyche_cublaslt_set_uintptr(&valueptr, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    desc->bias_pointer = valueptr;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_BIAS_BATCH_STRIDE:
    status = psyche_cublaslt_set_i64(&value64, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value64 < 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    desc->bias_stride = value64;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_BIAS_DATA_TYPE:
    status = psyche_cublaslt_set_i32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value32 != -1 && value32 != CUDA_R_32F && value32 != CUDA_R_64F) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    desc->bias_data_type = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_POINTER:
    status = psyche_cublaslt_set_uintptr(&valueptr, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    desc->aux_pointer = valueptr;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_LD:
    status = psyche_cublaslt_set_i64(&value64, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value64 < 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    desc->aux_ld = value64;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_BATCH_STRIDE:
    status = psyche_cublaslt_set_i64(&value64, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value64 < 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    desc->aux_stride = value64;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_DATA_TYPE:
    status = psyche_cublaslt_set_i32(&value32, buf, sizeInBytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
    if (value32 != -1 && value32 != CUDA_R_32F && value32 != CUDA_R_64F) {
      return CUBLAS_STATUS_NOT_SUPPORTED;
    }
    desc->aux_data_type = value32;
    return CUBLAS_STATUS_SUCCESS;
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_SCALE_POINTER:
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_AMAX_POINTER:
    return CUBLAS_STATUS_NOT_SUPPORTED;
  case CUBLASLT_MATMUL_DESC_SM_COUNT_TARGET:
    return psyche_cublaslt_set_i32(&desc->sm_count_target, buf, sizeInBytes);
  case CUBLASLT_MATMUL_DESC_COMPUTE_TYPE:
  case CUBLASLT_MATMUL_DESC_SCALE_TYPE:
    return CUBLAS_STATUS_NOT_SUPPORTED;
  default:
    return CUBLAS_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulDescGetAttribute(
    cublasLtMatmulDesc_t matmulDesc,
    cublasLtMatmulDescAttributes_t attr,
    void *buf,
    size_t sizeInBytes,
    size_t *sizeWritten) {
  const PsycheCublasLtMatmulDesc *desc = (const PsycheCublasLtMatmulDesc *)matmulDesc;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(matmulDesc, PSYCHE_CUBLASLT_KIND_MATMUL_DESC);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  switch (attr) {
  case CUBLASLT_MATMUL_DESC_COMPUTE_TYPE:
    return psyche_cublaslt_get_attr_value(&desc->compute_type, sizeof(desc->compute_type), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_SCALE_TYPE:
    return psyche_cublaslt_get_attr_value(&desc->scale_type, sizeof(desc->scale_type), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_POINTER_MODE:
    return psyche_cublaslt_get_attr_value(&desc->pointer_mode, sizeof(desc->pointer_mode), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_TRANSA:
    return psyche_cublaslt_get_attr_value(&desc->transa, sizeof(desc->transa), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_TRANSB:
    return psyche_cublaslt_get_attr_value(&desc->transb, sizeof(desc->transb), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_TRANSC:
    return psyche_cublaslt_get_attr_value(&desc->transc, sizeof(desc->transc), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_FILL_MODE:
    return psyche_cublaslt_get_attr_value(&desc->fill_mode, sizeof(desc->fill_mode), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_EPILOGUE:
    return psyche_cublaslt_get_attr_value(&desc->epilogue, sizeof(desc->epilogue), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_BIAS_POINTER:
    return psyche_cublaslt_get_attr_value(&desc->bias_pointer, sizeof(void *), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_BIAS_BATCH_STRIDE:
    return psyche_cublaslt_get_attr_value(&desc->bias_stride, sizeof(desc->bias_stride), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_BIAS_DATA_TYPE:
    return psyche_cublaslt_get_attr_value(&desc->bias_data_type, sizeof(desc->bias_data_type), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_POINTER:
    return psyche_cublaslt_get_attr_value(&desc->aux_pointer, sizeof(void *), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_LD:
    return psyche_cublaslt_get_attr_value(&desc->aux_ld, sizeof(desc->aux_ld), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_BATCH_STRIDE:
    return psyche_cublaslt_get_attr_value(&desc->aux_stride, sizeof(desc->aux_stride), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_DATA_TYPE:
    return psyche_cublaslt_get_attr_value(&desc->aux_data_type, sizeof(desc->aux_data_type), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_SCALE_POINTER:
  case CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_AMAX_POINTER:
    return CUBLAS_STATUS_NOT_SUPPORTED;
  case CUBLASLT_MATMUL_DESC_SM_COUNT_TARGET:
    return psyche_cublaslt_get_attr_value(&desc->sm_count_target, sizeof(desc->sm_count_target), buf, sizeInBytes, sizeWritten);
  default:
    return CUBLAS_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulPreferenceInit_internal(
    cublasLtMatmulPreference_t pref,
    size_t size) {
  return psyche_cublaslt_init_preference(pref, size, 0);
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulPreferenceCreate(cublasLtMatmulPreference_t *pref) {
  cublasLtMatmulPreference_t preference = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (pref == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *pref = 0;
  preference = (cublasLtMatmulPreference_t)calloc(1, sizeof(cublasLtMatmulPreferenceOpaque_t));
  if (preference == 0) {
    return CUBLAS_STATUS_ALLOC_FAILED;
  }
  status = psyche_cublaslt_init_preference(preference, sizeof(cublasLtMatmulPreferenceOpaque_t), 1);
  if (status != CUBLAS_STATUS_SUCCESS) {
    free(preference);
    return status;
  }
  *pref = preference;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulPreferenceDestroy(cublasLtMatmulPreference_t pref) {
  PsycheCublasLtPreference *preference = (PsycheCublasLtPreference *)pref;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(pref, PSYCHE_CUBLASLT_KIND_PREFERENCE);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  preference->magic = 0;
  if (preference->owns_allocation) {
    free(preference);
  }
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulPreferenceSetAttribute(
    cublasLtMatmulPreference_t pref,
    cublasLtMatmulPreferenceAttributes_t attr,
    const void *buf,
    size_t sizeInBytes) {
  PsycheCublasLtPreference *preference = (PsycheCublasLtPreference *)pref;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(pref, PSYCHE_CUBLASLT_KIND_PREFERENCE);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  switch (attr) {
  case CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES:
    return psyche_cublaslt_set_u64(&preference->max_workspace_bytes, buf, sizeInBytes);
  case CUBLASLT_MATMUL_PREF_SEARCH_MODE:
    return psyche_cublaslt_set_u32(&preference->search_mode, buf, sizeInBytes);
  default:
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulPreferenceGetAttribute(
    cublasLtMatmulPreference_t pref,
    cublasLtMatmulPreferenceAttributes_t attr,
    void *buf,
    size_t sizeInBytes,
    size_t *sizeWritten) {
  const PsycheCublasLtPreference *preference = (const PsycheCublasLtPreference *)pref;
  cublasStatus_t status = psyche_cublaslt_validate_desc_header(pref, PSYCHE_CUBLASLT_KIND_PREFERENCE);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  switch (attr) {
  case CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES:
    return psyche_cublaslt_get_attr_value(&preference->max_workspace_bytes, sizeof(preference->max_workspace_bytes), buf, sizeInBytes, sizeWritten);
  case CUBLASLT_MATMUL_PREF_SEARCH_MODE:
    return psyche_cublaslt_get_attr_value(&preference->search_mode, sizeof(preference->search_mode), buf, sizeInBytes, sizeWritten);
  default:
    return CUBLAS_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulAlgoGetIds(
    cublasLtHandle_t lightHandle,
    cublasComputeType_t computeType,
    cudaDataType_t scaleType,
    cudaDataType_t Atype,
    cudaDataType_t Btype,
    cudaDataType_t Ctype,
    cudaDataType_t Dtype,
    int requestedAlgoCount,
    int algoIdsArray[],
    int *returnAlgoCount) {
  if (psyche_cublaslt_context(lightHandle) == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (requestedAlgoCount <= 0 || returnAlgoCount == 0 || algoIdsArray == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *returnAlgoCount = 0;
  if (
      Atype == Dtype &&
      Btype == Dtype &&
      Ctype == Dtype &&
      psyche_cublaslt_compute_matches_data(computeType, scaleType, Dtype)) {
    algoIdsArray[0] = 0;
    *returnAlgoCount = 1;
  }
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulAlgoInit(
    cublasLtHandle_t lightHandle,
    cublasComputeType_t computeType,
    cudaDataType_t scaleType,
    cudaDataType_t Atype,
    cudaDataType_t Btype,
    cudaDataType_t Ctype,
    cudaDataType_t Dtype,
    int algoId,
    cublasLtMatmulAlgo_t *algo) {
  if (psyche_cublaslt_context(lightHandle) == 0) {
    return CUBLAS_STATUS_NOT_INITIALIZED;
  }
  if (algo == 0 || algoId != 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (
      Atype != Dtype ||
      Btype != Dtype ||
      Ctype != Dtype ||
      !psyche_cublaslt_compute_matches_data(computeType, scaleType, Dtype)) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  psyche_cublaslt_init_algo(algo, computeType, scaleType, Atype, Btype, Ctype, Dtype, algoId);
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulAlgoGetHeuristic(
    cublasLtHandle_t lightHandle,
    cublasLtMatmulDesc_t operationDesc,
    cublasLtMatrixLayout_t Adesc,
    cublasLtMatrixLayout_t Bdesc,
    cublasLtMatrixLayout_t Cdesc,
    cublasLtMatrixLayout_t Ddesc,
    cublasLtMatmulPreference_t preference,
    int requestedAlgoCount,
    cublasLtMatmulHeuristicResult_t heuristicResultsArray[],
    int *returnAlgoCount) {
  cudaDataType_t data_type = CUDA_R_32F;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  (void)preference;
  if (requestedAlgoCount <= 0 || returnAlgoCount == 0 || heuristicResultsArray == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *returnAlgoCount = 0;
  status = psyche_cublaslt_validate_matmul_config(
      lightHandle, operationDesc, Adesc, Bdesc, Cdesc, Ddesc, &data_type, 0, 0, 0, 0);
  if (status == CUBLAS_STATUS_NOT_SUPPORTED) {
    return CUBLAS_STATUS_SUCCESS;
  }
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  memset(&heuristicResultsArray[0], 0, sizeof(heuristicResultsArray[0]));
  psyche_cublaslt_init_algo(
      &heuristicResultsArray[0].algo,
      (cublasComputeType_t)((PsycheCublasLtMatmulDesc *)operationDesc)->compute_type,
      (cudaDataType_t)((PsycheCublasLtMatmulDesc *)operationDesc)->scale_type,
      data_type,
      data_type,
      data_type,
      data_type,
      0);
  heuristicResultsArray[0].workspaceSize = 0;
  heuristicResultsArray[0].state = CUBLAS_STATUS_SUCCESS;
  heuristicResultsArray[0].wavesCount = 1.0f;
  *returnAlgoCount = 1;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmulAlgoCheck(
    cublasLtHandle_t lightHandle,
    cublasLtMatmulDesc_t operationDesc,
    cublasLtMatrixLayout_t Adesc,
    cublasLtMatrixLayout_t Bdesc,
    cublasLtMatrixLayout_t Cdesc,
    cublasLtMatrixLayout_t Ddesc,
    const cublasLtMatmulAlgo_t *algo,
    cublasLtMatmulHeuristicResult_t *result) {
  cudaDataType_t data_type = CUDA_R_32F;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  if (result == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  status = psyche_cublaslt_validate_algo(algo);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_validate_matmul_config(
      lightHandle, operationDesc, Adesc, Bdesc, Cdesc, Ddesc, &data_type, 0, 0, 0, 0);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  memset(result, 0, sizeof(*result));
  psyche_cublaslt_init_algo(
      &result->algo,
      (cublasComputeType_t)((PsycheCublasLtMatmulDesc *)operationDesc)->compute_type,
      (cudaDataType_t)((PsycheCublasLtMatmulDesc *)operationDesc)->scale_type,
      data_type,
      data_type,
      data_type,
      data_type,
      0);
  result->workspaceSize = 0;
  result->state = CUBLAS_STATUS_SUCCESS;
  result->wavesCount = 1.0f;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtMatmul(
    cublasLtHandle_t lightHandle,
    cublasLtMatmulDesc_t computeDesc,
    const void *alpha,
    const void *A,
    cublasLtMatrixLayout_t Adesc,
    const void *B,
    cublasLtMatrixLayout_t Bdesc,
    const void *beta,
    const void *C,
    cublasLtMatrixLayout_t Cdesc,
    void *D,
    cublasLtMatrixLayout_t Ddesc,
    const cublasLtMatmulAlgo_t *algo,
    void *workspace,
    size_t workspaceSizeInBytes,
    cudaStream_t stream) {
  const PsycheCublasLtMatmulDesc *desc = (const PsycheCublasLtMatmulDesc *)computeDesc;
  const PsycheCublasLtMatrixLayout *a_layout = (const PsycheCublasLtMatrixLayout *)Adesc;
  const PsycheCublasLtMatrixLayout *b_layout = (const PsycheCublasLtMatrixLayout *)Bdesc;
  const PsycheCublasLtMatrixLayout *c_layout = (const PsycheCublasLtMatrixLayout *)Cdesc;
  const PsycheCublasLtMatrixLayout *d_layout = (const PsycheCublasLtMatrixLayout *)Ddesc;
  cudaDataType_t data_type = CUDA_R_32F;
  uint64_t m = 0;
  uint64_t n = 0;
  uint64_t k = 0;
  uint32_t batch_count = 0;
  size_t element_size = 0;
  size_t a_matrix_bytes = 0;
  size_t b_matrix_bytes = 0;
  size_t d_matrix_bytes = 0;
  size_t aux_matrix_bytes = 0;
  size_t bias_vector_bytes = 0;
  uint32_t batch = 0;
  cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
  (void)workspace;
  (void)workspaceSizeInBytes;
  (void)stream;
  status = psyche_cublaslt_validate_algo(algo);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cublaslt_validate_matmul_config(
      lightHandle, computeDesc, Adesc, Bdesc, Cdesc, Ddesc, &data_type, &m, &n, &k, &batch_count);
  if (status != CUBLAS_STATUS_SUCCESS) {
    return status;
  }
  if (alpha == 0 || beta == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  if (!psyche_cublaslt_data_type_size(data_type, &element_size)) {
    return CUBLAS_STATUS_NOT_SUPPORTED;
  }
  psyche_cublaslt_warn_operand_bgrad_d_output(desc->epilogue);
  if (psyche_cublaslt_epilogue_uses_aux(desc->epilogue) || psyche_cublaslt_epilogue_writes_bias_gradient(desc->epilogue)) {
    status = psyche_cublaslt_layout_matrix_bytes(d_layout, element_size, &d_matrix_bytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  if (psyche_cublaslt_epilogue_uses_aux(desc->epilogue)) {
    status = psyche_cublaslt_aux_matrix_bytes(desc, d_layout->rows, d_layout->cols, element_size, &aux_matrix_bytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  if (psyche_cublaslt_epilogue_writes_bias_gradient(desc->epilogue)) {
    status = psyche_cublaslt_temp_bytes(psyche_cublaslt_bias_gradient_length(desc, d_layout), 1, element_size, &bias_vector_bytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  if (psyche_cublaslt_epilogue_writes_bgrada(desc->epilogue)) {
    status = psyche_cublaslt_layout_matrix_bytes(a_layout, element_size, &a_matrix_bytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  if (psyche_cublaslt_epilogue_writes_bgradb(desc->epilogue)) {
    status = psyche_cublaslt_layout_matrix_bytes(b_layout, element_size, &b_matrix_bytes);
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  if (psyche_cublaslt_pointer_array_mode(d_layout->batch_mode)) {
    int alpha_zero = psyche_cublaslt_scalar_is_zero(alpha, data_type);
    int beta_zero = psyche_cublaslt_scalar_is_zero(beta, data_type);
    for (batch = 0; batch < batch_count; batch++) {
      const void *a_batch = psyche_cublaslt_batch_ptr(A, a_layout, batch, element_size);
      const void *b_batch = psyche_cublaslt_batch_ptr(B, b_layout, batch, element_size);
      const void *c_batch = C == 0 ? 0 : psyche_cublaslt_batch_ptr(C, c_layout, batch, element_size);
      void *d_batch = D == 0 ? 0 : psyche_cublaslt_batch_mut_ptr(D, d_layout, batch, element_size);
      if (!alpha_zero && k > 0 && (a_batch == 0 || b_batch == 0)) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      if (!beta_zero && c_batch == 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      if (m > 0 && n > 0 && d_batch == 0) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
    }
  }
  for (batch = 0; batch < batch_count; batch++) {
    const void *a_batch = psyche_cublaslt_batch_ptr(A, a_layout, batch, element_size);
    const void *b_batch = psyche_cublaslt_batch_ptr(B, b_layout, batch, element_size);
    const void *c_batch = C == 0 ? 0 : psyche_cublaslt_batch_ptr(C, c_layout, batch, element_size);
    void *d_batch = D == 0 ? 0 : psyche_cublaslt_batch_mut_ptr(D, d_layout, batch, element_size);
    void *aux_batch = 0;
    const void *bias_batch = 0;
    if (psyche_cublaslt_epilogue_writes_bgrada(desc->epilogue) && a_batch == 0 && a_matrix_bytes != 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (psyche_cublaslt_epilogue_writes_bgradb(desc->epilogue) && b_batch == 0 && b_matrix_bytes != 0) {
      return CUBLAS_STATUS_INVALID_VALUE;
    }
    if (aux_matrix_bytes != 0 && d_batch != 0) {
      int overlap = 0;
      status = psyche_cublaslt_aux_batch_ptr(desc, batch, element_size, &aux_batch);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      status = psyche_cublaslt_ranges_overlap(d_batch, d_matrix_bytes, aux_batch, aux_matrix_bytes, &overlap);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      if (overlap) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
    }
    if (bias_vector_bytes != 0 && d_batch != 0) {
      int overlap = 0;
      if (aux_batch == 0 && aux_matrix_bytes != 0) {
        status = psyche_cublaslt_aux_batch_ptr(desc, batch, element_size, &aux_batch);
        if (status != CUBLAS_STATUS_SUCCESS) {
          return status;
        }
      }
      status = psyche_cublaslt_bias_batch_ptr(desc, batch, element_size, &bias_batch);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      status = psyche_cublaslt_ranges_overlap(d_batch, d_matrix_bytes, bias_batch, bias_vector_bytes, &overlap);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      if (overlap) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
      if (a_matrix_bytes != 0) {
        status = psyche_cublaslt_ranges_overlap(a_batch, a_matrix_bytes, bias_batch, bias_vector_bytes, &overlap);
        if (status != CUBLAS_STATUS_SUCCESS) {
          return status;
        }
        if (overlap) {
          return CUBLAS_STATUS_INVALID_VALUE;
        }
      }
      if (b_matrix_bytes != 0) {
        status = psyche_cublaslt_ranges_overlap(b_batch, b_matrix_bytes, bias_batch, bias_vector_bytes, &overlap);
        if (status != CUBLAS_STATUS_SUCCESS) {
          return status;
        }
        if (overlap) {
          return CUBLAS_STATUS_INVALID_VALUE;
        }
      }
      status = psyche_cublaslt_ranges_overlap(aux_batch, aux_matrix_bytes, bias_batch, bias_vector_bytes, &overlap);
      if (status != CUBLAS_STATUS_SUCCESS) {
        return status;
      }
      if (overlap) {
        return CUBLAS_STATUS_INVALID_VALUE;
      }
    }
    if (data_type == CUDA_R_32F) {
      status = psyche_cublaslt_sgemm_one(
          desc,
          batch,
          (const float *)alpha,
          (const float *)a_batch,
          a_layout,
          (const float *)b_batch,
          b_layout,
          (const float *)beta,
          (const float *)c_batch,
          c_layout,
          (float *)d_batch,
          d_layout);
    } else {
      status = psyche_cublaslt_dgemm_one(
          desc,
          batch,
          (const double *)alpha,
          (const double *)a_batch,
          a_layout,
          (const double *)b_batch,
          b_layout,
          (const double *)beta,
          (const double *)c_batch,
          c_layout,
          (double *)d_batch,
          d_layout);
    }
    if (status != CUBLAS_STATUS_SUCCESS) {
      return status;
    }
  }
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API unsigned cublasLtDisableCpuInstructionsSetMask(unsigned mask) {
  (void)mask;
  return 0;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtHeuristicsCacheGetCapacity(size_t *capacity) {
  if (capacity == 0) {
    return CUBLAS_STATUS_INVALID_VALUE;
  }
  *capacity = 0;
  return CUBLAS_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cublasStatus_t cublasLtHeuristicsCacheSetCapacity(size_t capacity) {
  (void)capacity;
  return CUBLAS_STATUS_SUCCESS;
}
