#ifndef PSYCHE_CUDA_COMPAT_STUB_H
#define PSYCHE_CUDA_COMPAT_STUB_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define PSYCHE_CUDA_STUB_API __declspec(dllexport)
#else
#define PSYCHE_CUDA_STUB_API __attribute__((visibility("default")))
#endif

typedef int CUdevice;
typedef int CUresult;
typedef void *CUcontext;
typedef void *CUmodule;
typedef void *CUfunction;
typedef void *CUstream;
typedef void *CUevent;
typedef void *CUarray;
typedef void *CUmemoryPool;
typedef struct cublasContext *cublasHandle_t;
typedef struct cublasLtContext *cublasLtHandle_t;
typedef struct cusparseContext *cusparseHandle_t;
typedef struct cusparseSpMatStruct *cusparseSpMatDescr_t;
typedef const struct cusparseSpMatStruct *cusparseConstSpMatDescr_t;
typedef struct cusparseDnVecStruct *cusparseDnVecDescr_t;
typedef const struct cusparseDnVecStruct *cusparseConstDnVecDescr_t;
typedef struct cusparseDnMatStruct *cusparseDnMatDescr_t;
typedef const struct cusparseDnMatStruct *cusparseConstDnMatDescr_t;
typedef struct cusolverDnContext *cusolverDnHandle_t;
typedef struct cudnnContext *cudnnHandle_t;
typedef struct cudnnTensorStruct *cudnnTensorDescriptor_t;
typedef struct cudnnActivationStruct *cudnnActivationDescriptor_t;
typedef struct cudnnPoolingStruct *cudnnPoolingDescriptor_t;
typedef struct cudnnFilterStruct *cudnnFilterDescriptor_t;
typedef struct cudnnConvolutionStruct *cudnnConvolutionDescriptor_t;
typedef struct {
  float x;
  float y;
} cuComplex;
typedef struct {
  double x;
  double y;
} cuDoubleComplex;
typedef unsigned long long CUdeviceptr;
typedef unsigned long long cuuint64_t;
typedef int nvmlReturn_t;
typedef void *nvmlDevice_t;

enum {
  CUDA_SUCCESS = 0,
  CUDA_ERROR_INVALID_VALUE = 1,
  CUDA_ERROR_OUT_OF_MEMORY = 2,
  CUDA_ERROR_NOT_INITIALIZED = 3,
  CUDA_ERROR_DEINITIALIZED = 4,
  CUDA_ERROR_INVALID_CONFIGURATION = 9,
  CUDA_ERROR_INVALID_DEVICE_FUNCTION = 98,
  CUDA_ERROR_NO_DEVICE = 100,
  CUDA_ERROR_INVALID_DEVICE = 101,
  CUDA_ERROR_INVALID_HANDLE = 400,
  CUDA_ERROR_NOT_READY = 600,
  CUDA_ERROR_HOST_MEMORY_ALREADY_REGISTERED = 712,
  CUDA_ERROR_HOST_MEMORY_NOT_REGISTERED = 713,
  CUDA_ERROR_NOT_SUPPORTED = 801,
  CUDA_ERROR_UNKNOWN = 999
};

typedef enum {
  cudaSuccess = 0,
  cudaErrorInvalidValue = 1,
  cudaErrorMemoryAllocation = 2,
  cudaErrorInitializationError = 3,
  cudaErrorCudartUnloading = 4,
  cudaErrorInvalidConfiguration = 9,
  cudaErrorInvalidDeviceFunction = 98,
  cudaErrorNoDevice = 100,
  cudaErrorInvalidDevice = 101,
  cudaErrorInvalidResourceHandle = 400,
  cudaErrorNotReady = 600,
  cudaErrorHostMemoryAlreadyRegistered = 712,
  cudaErrorHostMemoryNotRegistered = 713,
  cudaErrorNotSupported = 801,
  cudaErrorUnknown = 999
} cudaError_t;

typedef enum {
  CUBLAS_STATUS_SUCCESS = 0,
  CUBLAS_STATUS_NOT_INITIALIZED = 1,
  CUBLAS_STATUS_ALLOC_FAILED = 3,
  CUBLAS_STATUS_INVALID_VALUE = 7,
  CUBLAS_STATUS_ARCH_MISMATCH = 8,
  CUBLAS_STATUS_MAPPING_ERROR = 11,
  CUBLAS_STATUS_EXECUTION_FAILED = 13,
  CUBLAS_STATUS_INTERNAL_ERROR = 14,
  CUBLAS_STATUS_NOT_SUPPORTED = 15,
  CUBLAS_STATUS_LICENSE_ERROR = 16
} cublasStatus_t;

typedef enum {
  CUDNN_STATUS_SUCCESS = 0,
  CUDNN_STATUS_NOT_INITIALIZED = 1,
  CUDNN_STATUS_ALLOC_FAILED = 2,
  CUDNN_STATUS_BAD_PARAM = 3,
  CUDNN_STATUS_INTERNAL_ERROR = 4,
  CUDNN_STATUS_INVALID_VALUE = 5,
  CUDNN_STATUS_ARCH_MISMATCH = 6,
  CUDNN_STATUS_MAPPING_ERROR = 7,
  CUDNN_STATUS_EXECUTION_FAILED = 8,
  CUDNN_STATUS_NOT_SUPPORTED = 9,
  CUDNN_STATUS_LICENSE_ERROR = 10,
  CUDNN_STATUS_RUNTIME_PREREQUISITE_MISSING = 11,
  CUDNN_STATUS_RUNTIME_IN_PROGRESS = 12,
  CUDNN_STATUS_RUNTIME_FP_OVERFLOW = 13,
  CUDNN_STATUS_VERSION_MISMATCH = 14
} cudnnStatus_t;

typedef enum {
  CUDNN_TENSOR_NCHW = 0,
  CUDNN_TENSOR_NHWC = 1,
  CUDNN_TENSOR_NCHW_VECT_C = 2
} cudnnTensorFormat_t;

typedef enum {
  CUDNN_DATA_FLOAT = 0,
  CUDNN_DATA_DOUBLE = 1,
  CUDNN_DATA_HALF = 2,
  CUDNN_DATA_INT8 = 3,
  CUDNN_DATA_INT32 = 4,
  CUDNN_DATA_INT8x4 = 5,
  CUDNN_DATA_UINT8 = 6,
  CUDNN_DATA_UINT8x4 = 7,
  CUDNN_DATA_INT8x32 = 8,
  CUDNN_DATA_BFLOAT16 = 9,
  CUDNN_DATA_INT64 = 10,
  CUDNN_DATA_BOOLEAN = 11,
  CUDNN_DATA_FP8_E4M3 = 12,
  CUDNN_DATA_FP8_E5M2 = 13,
  CUDNN_DATA_FAST_FLOAT_FOR_FP8 = 14
} cudnnDataType_t;

typedef enum {
  CUDNN_NOT_PROPAGATE_NAN = 0,
  CUDNN_PROPAGATE_NAN = 1
} cudnnNanPropagation_t;

typedef enum {
  CUDNN_BATCHNORM_PER_ACTIVATION = 0,
  CUDNN_BATCHNORM_SPATIAL = 1,
  CUDNN_BATCHNORM_SPATIAL_PERSISTENT = 2
} cudnnBatchNormMode_t;

#ifndef CUDNN_BN_MIN_EPSILON
#define CUDNN_BN_MIN_EPSILON 1e-5
#endif

typedef enum {
  CUDNN_ACTIVATION_SIGMOID = 0,
  CUDNN_ACTIVATION_RELU = 1,
  CUDNN_ACTIVATION_TANH = 2,
  CUDNN_ACTIVATION_CLIPPED_RELU = 3,
  CUDNN_ACTIVATION_ELU = 4,
  CUDNN_ACTIVATION_IDENTITY = 5,
  CUDNN_ACTIVATION_SWISH = 6
} cudnnActivationMode_t;

typedef enum {
  CUDNN_POOLING_MAX = 0,
  CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING = 1,
  CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING = 2,
  CUDNN_POOLING_MAX_DETERMINISTIC = 3
} cudnnPoolingMode_t;

typedef enum {
  CUDNN_CONVOLUTION = 0,
  CUDNN_CROSS_CORRELATION = 1
} cudnnConvolutionMode_t;

typedef enum {
  CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM = 0,
  CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM = 1,
  CUDNN_CONVOLUTION_FWD_ALGO_GEMM = 2,
  CUDNN_CONVOLUTION_FWD_ALGO_DIRECT = 3,
  CUDNN_CONVOLUTION_FWD_ALGO_FFT = 4,
  CUDNN_CONVOLUTION_FWD_ALGO_FFT_TILING = 5,
  CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD = 6,
  CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED = 7,
  CUDNN_CONVOLUTION_FWD_ALGO_COUNT = 8
} cudnnConvolutionFwdAlgo_t;

typedef enum {
  CUDNN_CONVOLUTION_FWD_NO_WORKSPACE = 0,
  CUDNN_CONVOLUTION_FWD_PREFER_FASTEST = 1,
  CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT = 2
} cudnnConvolutionFwdPreference_t;

typedef enum {
  CUDNN_NON_DETERMINISTIC = 0,
  CUDNN_DETERMINISTIC = 1
} cudnnDeterminism_t;

typedef enum {
  CUDNN_DEFAULT_MATH = 0,
  CUDNN_TENSOR_OP_MATH = 1,
  CUDNN_TENSOR_OP_MATH_ALLOW_CONVERSION = 2,
  CUDNN_FMA_MATH = 3
} cudnnMathType_t;

typedef struct {
  cudnnConvolutionFwdAlgo_t algo;
  cudnnStatus_t status;
  float time;
  size_t memory;
  cudnnDeterminism_t determinism;
  cudnnMathType_t mathType;
  int reserved[3];
} cudnnConvolutionFwdAlgoPerf_t;

typedef enum {
  CUDNN_CONVOLUTION_BWD_DATA_ALGO_0 = 0,
  CUDNN_CONVOLUTION_BWD_DATA_ALGO_1 = 1,
  CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT = 2,
  CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT_TILING = 3,
  CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD = 4,
  CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD_NONFUSED = 5,
  CUDNN_CONVOLUTION_BWD_DATA_ALGO_COUNT = 6
} cudnnConvolutionBwdDataAlgo_t;

typedef enum {
  CUDNN_CONVOLUTION_BWD_DATA_NO_WORKSPACE = 0,
  CUDNN_CONVOLUTION_BWD_DATA_PREFER_FASTEST = 1,
  CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT = 2
} cudnnConvolutionBwdDataPreference_t;

typedef struct {
  cudnnConvolutionBwdDataAlgo_t algo;
  cudnnStatus_t status;
  float time;
  size_t memory;
  cudnnDeterminism_t determinism;
  cudnnMathType_t mathType;
  int reserved[3];
} cudnnConvolutionBwdDataAlgoPerf_t;

typedef enum {
  CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0 = 0,
  CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1 = 1,
  CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT = 2,
  CUDNN_CONVOLUTION_BWD_FILTER_ALGO_3 = 3,
  CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD = 4,
  CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD_NONFUSED = 5,
  CUDNN_CONVOLUTION_BWD_FILTER_ALGO_COUNT = 6
} cudnnConvolutionBwdFilterAlgo_t;

typedef enum {
  CUDNN_CONVOLUTION_BWD_FILTER_NO_WORKSPACE = 0,
  CUDNN_CONVOLUTION_BWD_FILTER_PREFER_FASTEST = 1,
  CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT = 2
} cudnnConvolutionBwdFilterPreference_t;

typedef struct {
  cudnnConvolutionBwdFilterAlgo_t algo;
  cudnnStatus_t status;
  float time;
  size_t memory;
  cudnnDeterminism_t determinism;
  cudnnMathType_t mathType;
  int reserved[3];
} cudnnConvolutionBwdFilterAlgoPerf_t;

typedef enum {
  CUDNN_SOFTMAX_FAST = 0,
  CUDNN_SOFTMAX_ACCURATE = 1,
  CUDNN_SOFTMAX_LOG = 2
} cudnnSoftmaxAlgorithm_t;

typedef enum {
  CUDNN_SOFTMAX_MODE_INSTANCE = 0,
  CUDNN_SOFTMAX_MODE_CHANNEL = 1
} cudnnSoftmaxMode_t;

typedef enum {
  CUBLAS_OP_N = 0,
  CUBLAS_OP_T = 1,
  CUBLAS_OP_C = 2
} cublasOperation_t;

typedef enum {
  CUSPARSE_OPERATION_NON_TRANSPOSE = 0,
  CUSPARSE_OPERATION_TRANSPOSE = 1,
  CUSPARSE_OPERATION_CONJUGATE_TRANSPOSE = 2
} cusparseOperation_t;

typedef enum {
  CUBLAS_FILL_MODE_LOWER = 0,
  CUBLAS_FILL_MODE_UPPER = 1,
  CUBLAS_FILL_MODE_FULL = 2
} cublasFillMode_t;

typedef enum {
  CUBLAS_SIDE_LEFT = 0,
  CUBLAS_SIDE_RIGHT = 1
} cublasSideMode_t;

typedef enum {
  CUBLAS_DIAG_NON_UNIT = 0,
  CUBLAS_DIAG_UNIT = 1
} cublasDiagType_t;

typedef enum {
  CUBLAS_POINTER_MODE_HOST = 0,
  CUBLAS_POINTER_MODE_DEVICE = 1
} cublasPointerMode_t;

typedef enum cudaDataType_t {
  CUDA_R_16F = 2,
  CUDA_C_16F = 6,
  CUDA_R_16BF = 14,
  CUDA_C_16BF = 15,
  CUDA_R_32F = 0,
  CUDA_C_32F = 4,
  CUDA_R_64F = 1,
  CUDA_C_64F = 5,
  CUDA_R_4I = 16,
  CUDA_C_4I = 17,
  CUDA_R_4U = 18,
  CUDA_C_4U = 19,
  CUDA_R_8I = 3,
  CUDA_C_8I = 7,
  CUDA_R_8U = 8,
  CUDA_C_8U = 9,
  CUDA_R_16I = 20,
  CUDA_C_16I = 21,
  CUDA_R_16U = 22,
  CUDA_C_16U = 23,
  CUDA_R_32I = 10,
  CUDA_C_32I = 11,
  CUDA_R_32U = 12,
  CUDA_C_32U = 13,
  CUDA_R_64I = 24,
  CUDA_C_64I = 25,
  CUDA_R_64U = 26,
  CUDA_C_64U = 27,
  CUDA_R_8F_E4M3 = 28,
  CUDA_R_8F_UE4M3 = 28,
  CUDA_R_8F_E5M2 = 29,
  CUDA_R_8F_UE8M0 = 30,
  CUDA_R_6F_E2M3 = 31,
  CUDA_R_6F_E3M2 = 32,
  CUDA_R_4F_E2M1 = 33
} cudaDataType_t;

typedef cudaDataType_t cudaDataType;

typedef enum {
  CUSPARSE_STATUS_SUCCESS = 0,
  CUSPARSE_STATUS_NOT_INITIALIZED = 1,
  CUSPARSE_STATUS_ALLOC_FAILED = 2,
  CUSPARSE_STATUS_INVALID_VALUE = 3,
  CUSPARSE_STATUS_ARCH_MISMATCH = 4,
  CUSPARSE_STATUS_MAPPING_ERROR = 5,
  CUSPARSE_STATUS_EXECUTION_FAILED = 6,
  CUSPARSE_STATUS_INTERNAL_ERROR = 7,
  CUSPARSE_STATUS_MATRIX_TYPE_NOT_SUPPORTED = 8,
  CUSPARSE_STATUS_ZERO_PIVOT = 9,
  CUSPARSE_STATUS_NOT_SUPPORTED = 10,
  CUSPARSE_STATUS_INSUFFICIENT_RESOURCES = 11
} cusparseStatus_t;

typedef enum {
  CUSOLVER_STATUS_SUCCESS = 0,
  CUSOLVER_STATUS_NOT_INITIALIZED = 1,
  CUSOLVER_STATUS_ALLOC_FAILED = 2,
  CUSOLVER_STATUS_INVALID_VALUE = 3,
  CUSOLVER_STATUS_ARCH_MISMATCH = 4,
  CUSOLVER_STATUS_MAPPING_ERROR = 5,
  CUSOLVER_STATUS_EXECUTION_FAILED = 6,
  CUSOLVER_STATUS_INTERNAL_ERROR = 7,
  CUSOLVER_STATUS_MATRIX_TYPE_NOT_SUPPORTED = 8,
  CUSOLVER_STATUS_NOT_SUPPORTED = 9,
  CUSOLVER_STATUS_ZERO_PIVOT = 10,
  CUSOLVER_STATUS_INVALID_LICENSE = 11,
  CUSOLVER_STATUS_IRS_PARAMS_NOT_INITIALIZED = 12,
  CUSOLVER_STATUS_IRS_PARAMS_INVALID = 13,
  CUSOLVER_STATUS_IRS_PARAMS_INVALID_PREC = 14,
  CUSOLVER_STATUS_IRS_PARAMS_INVALID_REFINE = 15,
  CUSOLVER_STATUS_IRS_PARAMS_INVALID_MAXITER = 16,
  CUSOLVER_STATUS_IRS_INTERNAL_ERROR = 20,
  CUSOLVER_STATUS_IRS_NOT_SUPPORTED = 21,
  CUSOLVER_STATUS_IRS_OUT_OF_RANGE = 22,
  CUSOLVER_STATUS_IRS_NRHS_NOT_SUPPORTED_FOR_REFINE_GMRES = 23,
  CUSOLVER_STATUS_IRS_INFOS_NOT_INITIALIZED = 25,
  CUSOLVER_STATUS_IRS_INFOS_NOT_DESTROYED = 26,
  CUSOLVER_STATUS_IRS_MATRIX_SINGULAR = 30,
  CUSOLVER_STATUS_INVALID_WORKSPACE = 31
} cusolverStatus_t;

typedef enum {
  CUSPARSE_INDEX_16U = 1,
  CUSPARSE_INDEX_32I = 2,
  CUSPARSE_INDEX_64I = 3
} cusparseIndexType_t;

typedef enum {
  CUSPARSE_INDEX_BASE_ZERO = 0,
  CUSPARSE_INDEX_BASE_ONE = 1
} cusparseIndexBase_t;

typedef enum {
  CUSPARSE_FORMAT_CSR = 1,
  CUSPARSE_FORMAT_CSC = 2,
  CUSPARSE_FORMAT_COO = 3,
  CUSPARSE_FORMAT_BLOCKED_ELL = 5,
  CUSPARSE_FORMAT_BSR = 6,
  CUSPARSE_FORMAT_SLICED_ELLPACK = 7,
  CUSPARSE_FORMAT_SLICED_ELL = CUSPARSE_FORMAT_SLICED_ELLPACK
} cusparseFormat_t;

typedef enum {
  CUSPARSE_ORDER_COL = 1,
  CUSPARSE_ORDER_ROW = 2
} cusparseOrder_t;

typedef enum {
  CUSPARSE_POINTER_MODE_HOST = 0,
  CUSPARSE_POINTER_MODE_DEVICE = 1
} cusparsePointerMode_t;

typedef enum {
  CUSPARSE_SPMV_ALG_DEFAULT = 0,
  CUSPARSE_SPMV_COO_ALG1 = 1,
  CUSPARSE_SPMV_COO_ALG2 = 2,
  CUSPARSE_SPMV_CSR_ALG1 = 3,
  CUSPARSE_SPMV_CSR_ALG2 = 4,
  CUSPARSE_SPMV_SELL_ALG1 = 5
} cusparseSpMVAlg_t;

typedef enum {
  CUSPARSE_SPMM_ALG_DEFAULT = 0,
  CUSPARSE_SPMM_COO_ALG1 = 1,
  CUSPARSE_SPMM_COO_ALG2 = 2,
  CUSPARSE_SPMM_COO_ALG3 = 3,
  CUSPARSE_SPMM_CSR_ALG1 = 4,
  CUSPARSE_SPMM_COO_ALG4 = 5,
  CUSPARSE_SPMM_CSR_ALG2 = 6,
  CUSPARSE_SPMM_CSR_ALG3 = 12,
  CUSPARSE_SPMM_BLOCKED_ELL_ALG1 = 13,
  CUSPARSE_SPMM_BSR_ALG1 = 14
} cusparseSpMMAlg_t;

typedef enum {
  CUBLAS_COMPUTE_16F = 64,
  CUBLAS_COMPUTE_16F_PEDANTIC = 65,
  CUBLAS_COMPUTE_32F = 68,
  CUBLAS_COMPUTE_32F_PEDANTIC = 69,
  CUBLAS_COMPUTE_64F = 70,
  CUBLAS_COMPUTE_64F_PEDANTIC = 71,
  CUBLAS_COMPUTE_32I = 72,
  CUBLAS_COMPUTE_32I_PEDANTIC = 73,
  CUBLAS_COMPUTE_32F_FAST_16F = 74,
  CUBLAS_COMPUTE_32F_FAST_16BF = 75,
  CUBLAS_COMPUTE_32F_FAST_TF32 = 77,
  CUBLAS_COMPUTE_32F_EMULATED_16BFX9 = 78
} cublasComputeType_t;

typedef struct {
  uint64_t data[8];
} cublasLtMatrixLayoutOpaque_t;

typedef cublasLtMatrixLayoutOpaque_t *cublasLtMatrixLayout_t;

typedef struct {
  uint64_t data[8];
} cublasLtMatmulAlgo_t;

typedef struct {
  uint64_t data[32];
} cublasLtMatmulDescOpaque_t;

typedef cublasLtMatmulDescOpaque_t *cublasLtMatmulDesc_t;

typedef struct {
  uint64_t data[8];
} cublasLtMatrixTransformDescOpaque_t;

typedef cublasLtMatrixTransformDescOpaque_t *cublasLtMatrixTransformDesc_t;

typedef struct {
  uint64_t data[8];
} cublasLtMatmulPreferenceOpaque_t;

typedef cublasLtMatmulPreferenceOpaque_t *cublasLtMatmulPreference_t;

typedef enum {
  CUBLASLT_POINTER_MODE_HOST = CUBLAS_POINTER_MODE_HOST,
  CUBLASLT_POINTER_MODE_DEVICE = CUBLAS_POINTER_MODE_DEVICE,
  CUBLASLT_POINTER_MODE_DEVICE_VECTOR = 2,
  CUBLASLT_POINTER_MODE_ALPHA_DEVICE_VECTOR_BETA_ZERO = 3,
  CUBLASLT_POINTER_MODE_ALPHA_DEVICE_VECTOR_BETA_HOST = 4
} cublasLtPointerMode_t;

typedef enum {
  CUBLASLT_ORDER_COL = 0,
  CUBLASLT_ORDER_ROW = 1,
  CUBLASLT_ORDER_COL32 = 2,
  CUBLASLT_ORDER_COL4_4R2_8C = 3,
  CUBLASLT_ORDER_COL32_2R_4R4 = 4
} cublasLtOrder_t;

typedef enum {
  CUBLASLT_BATCH_MODE_STRIDED = 0,
  CUBLASLT_BATCH_MODE_POINTER_ARRAY = 1
} cublasLtBatchMode_t;

typedef enum {
  CUBLASLT_MATRIX_LAYOUT_TYPE = 0,
  CUBLASLT_MATRIX_LAYOUT_ORDER = 1,
  CUBLASLT_MATRIX_LAYOUT_ROWS = 2,
  CUBLASLT_MATRIX_LAYOUT_COLS = 3,
  CUBLASLT_MATRIX_LAYOUT_LD = 4,
  CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT = 5,
  CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET = 6,
  CUBLASLT_MATRIX_LAYOUT_PLANE_OFFSET = 7,
  CUBLASLT_MATRIX_LAYOUT_BATCH_MODE = 8
} cublasLtMatrixLayoutAttribute_t;

typedef enum {
  CUBLASLT_MATMUL_DESC_COMPUTE_TYPE = 0,
  CUBLASLT_MATMUL_DESC_SCALE_TYPE = 1,
  CUBLASLT_MATMUL_DESC_POINTER_MODE = 2,
  CUBLASLT_MATMUL_DESC_TRANSA = 3,
  CUBLASLT_MATMUL_DESC_TRANSB = 4,
  CUBLASLT_MATMUL_DESC_TRANSC = 5,
  CUBLASLT_MATMUL_DESC_FILL_MODE = 6,
  CUBLASLT_MATMUL_DESC_EPILOGUE = 7,
  CUBLASLT_MATMUL_DESC_BIAS_POINTER = 8,
  CUBLASLT_MATMUL_DESC_BIAS_BATCH_STRIDE = 10,
  CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_POINTER = 11,
  CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_LD = 12,
  CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_BATCH_STRIDE = 13,
  CUBLASLT_MATMUL_DESC_ALPHA_VECTOR_BATCH_STRIDE = 14,
  CUBLASLT_MATMUL_DESC_SM_COUNT_TARGET = 15,
  CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_DATA_TYPE = 22,
  CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_SCALE_POINTER = 23,
  CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_AMAX_POINTER = 24,
  CUBLASLT_MATMUL_DESC_BIAS_DATA_TYPE = 26
} cublasLtMatmulDescAttributes_t;

typedef enum {
  CUBLASLT_MATRIX_TRANSFORM_DESC_SCALE_TYPE = 0,
  CUBLASLT_MATRIX_TRANSFORM_DESC_POINTER_MODE = 1,
  CUBLASLT_MATRIX_TRANSFORM_DESC_TRANSA = 2,
  CUBLASLT_MATRIX_TRANSFORM_DESC_TRANSB = 3
} cublasLtMatrixTransformDescAttributes_t;

typedef enum {
  CUBLASLT_EPILOGUE_DEFAULT = 1,
  CUBLASLT_EPILOGUE_RELU = 2,
  CUBLASLT_EPILOGUE_RELU_AUX = 130,
  CUBLASLT_EPILOGUE_BIAS = 4,
  CUBLASLT_EPILOGUE_RELU_BIAS = 6,
  CUBLASLT_EPILOGUE_RELU_AUX_BIAS = 134,
  CUBLASLT_EPILOGUE_DRELU = 136,
  CUBLASLT_EPILOGUE_DRELU_BGRAD = 152,
  CUBLASLT_EPILOGUE_GELU = 32,
  CUBLASLT_EPILOGUE_GELU_AUX = 160,
  CUBLASLT_EPILOGUE_GELU_BIAS = 36,
  CUBLASLT_EPILOGUE_GELU_AUX_BIAS = 164,
  CUBLASLT_EPILOGUE_DGELU = 192,
  CUBLASLT_EPILOGUE_DGELU_BGRAD = 208,
  CUBLASLT_EPILOGUE_BGRADA = 256,
  CUBLASLT_EPILOGUE_BGRADB = 512
} cublasLtEpilogue_t;

typedef enum {
  CUBLASLT_MATMUL_PREF_SEARCH_MODE = 0,
  CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES = 1,
  CUBLASLT_MATMUL_PREF_REDUCTION_SCHEME_MASK = 3,
  CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_A_BYTES = 5,
  CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_B_BYTES = 6,
  CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_C_BYTES = 7,
  CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_D_BYTES = 8,
  CUBLASLT_MATMUL_PREF_MAX_WAVES_COUNT = 9,
  CUBLASLT_MATMUL_PREF_IMPL_MASK = 12
} cublasLtMatmulPreferenceAttributes_t;

typedef struct {
  cublasLtMatmulAlgo_t algo;
  size_t workspaceSize;
  cublasStatus_t state;
  float wavesCount;
  int reserved[4];
} cublasLtMatmulHeuristicResult_t;

typedef enum {
  CUBLAS_DEFAULT_MATH = 0,
  CUBLAS_TENSOR_OP_MATH = 1,
  CUBLAS_PEDANTIC_MATH = 2,
  CUBLAS_TF32_TENSOR_OP_MATH = 3,
  CUBLAS_MATH_DISALLOW_REDUCED_PRECISION_REDUCTION = 16
} cublasMath_t;

typedef enum {
  CUBLAS_ATOMICS_NOT_ALLOWED = 0,
  CUBLAS_ATOMICS_ALLOWED = 1
} cublasAtomicsMode_t;

typedef enum {
  MAJOR_VERSION = 0,
  MINOR_VERSION = 1,
  PATCH_LEVEL = 2
} libraryPropertyType;

#define CU_STREAM_DEFAULT 0x00
#define CU_STREAM_NON_BLOCKING 0x01

#define CU_EVENT_DEFAULT 0x00
#define CU_EVENT_BLOCKING_SYNC 0x01
#define CU_EVENT_DISABLE_TIMING 0x02
#define CU_EVENT_INTERPROCESS 0x04

#define cudaStreamDefault 0x00
#define cudaStreamNonBlocking 0x01

#define cudaEventDefault 0x00
#define cudaEventBlockingSync 0x01
#define cudaEventDisableTiming 0x02
#define cudaEventInterprocess 0x04

typedef enum {
  CU_GET_PROC_ADDRESS_SUCCESS = 0,
  CU_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND = 1,
  CU_GET_PROC_ADDRESS_VERSION_NOT_SUFFICIENT = 2
} CUdriverProcAddressQueryResult;

#define CU_MEMHOSTALLOC_PORTABLE 0x01
#define CU_MEMHOSTALLOC_DEVICEMAP 0x02
#define CU_MEMHOSTALLOC_WRITECOMBINED 0x04

#define CU_MEMHOSTREGISTER_PORTABLE 0x01
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02
#define CU_MEMHOSTREGISTER_IOMEMORY 0x04
#define CU_MEMHOSTREGISTER_READ_ONLY 0x08

#define cudaHostAllocDefault 0x00
#define cudaHostAllocPortable 0x01
#define cudaHostAllocMapped 0x02
#define cudaHostAllocWriteCombined 0x04

#define cudaHostRegisterDefault 0x00
#define cudaHostRegisterPortable 0x01
#define cudaHostRegisterMapped 0x02
#define cudaHostRegisterIoMemory 0x04
#define cudaHostRegisterReadOnly 0x08

typedef enum {
  cudaMemcpyHostToHost = 0,
  cudaMemcpyHostToDevice = 1,
  cudaMemcpyDeviceToHost = 2,
  cudaMemcpyDeviceToDevice = 3,
  cudaMemcpyDefault = 4
} cudaMemcpyKind;

typedef enum {
  cudaMemoryTypeUnregistered = 0,
  cudaMemoryTypeHost = 1,
  cudaMemoryTypeDevice = 2,
  cudaMemoryTypeManaged = 3
} cudaMemoryType;

typedef enum {
  CU_MEM_ADVISE_SET_READ_MOSTLY = 1,
  CU_MEM_ADVISE_UNSET_READ_MOSTLY = 2,
  CU_MEM_ADVISE_SET_PREFERRED_LOCATION = 3,
  CU_MEM_ADVISE_UNSET_PREFERRED_LOCATION = 4,
  CU_MEM_ADVISE_SET_ACCESSED_BY = 5,
  CU_MEM_ADVISE_UNSET_ACCESSED_BY = 6
} CUmem_advise;

typedef enum {
  CU_MEM_RANGE_ATTRIBUTE_READ_MOSTLY = 1,
  CU_MEM_RANGE_ATTRIBUTE_PREFERRED_LOCATION = 2,
  CU_MEM_RANGE_ATTRIBUTE_ACCESSED_BY = 3,
  CU_MEM_RANGE_ATTRIBUTE_LAST_PREFETCH_LOCATION = 4,
  CU_MEM_RANGE_ATTRIBUTE_PREFERRED_LOCATION_TYPE = 5,
  CU_MEM_RANGE_ATTRIBUTE_PREFERRED_LOCATION_ID = 6,
  CU_MEM_RANGE_ATTRIBUTE_LAST_PREFETCH_LOCATION_TYPE = 7,
  CU_MEM_RANGE_ATTRIBUTE_LAST_PREFETCH_LOCATION_ID = 8
} CUmem_range_attribute;

typedef enum {
  CU_MEM_LOCATION_TYPE_INVALID = 0,
  CU_MEM_LOCATION_TYPE_NONE = 0,
  CU_MEM_LOCATION_TYPE_DEVICE = 1,
  CU_MEM_LOCATION_TYPE_HOST = 2,
  CU_MEM_LOCATION_TYPE_HOST_NUMA = 3,
  CU_MEM_LOCATION_TYPE_HOST_NUMA_CURRENT = 4,
  CU_MEM_LOCATION_TYPE_INVISIBLE = 5,
  CU_MEM_LOCATION_TYPE_MAX = 0x7FFFFFFF
} CUmemLocationType;

typedef enum {
  CU_MEM_ACCESS_FLAGS_PROT_NONE = 0,
  CU_MEM_ACCESS_FLAGS_PROT_READ = 1,
  CU_MEM_ACCESS_FLAGS_PROT_READWRITE = 3,
  CU_MEM_ACCESS_FLAGS_PROT_MAX = 0x7FFFFFFF
} CUmemAccess_flags;

typedef enum {
  CU_MEM_HANDLE_TYPE_NONE = 0,
  CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR = 1,
  CU_MEM_HANDLE_TYPE_WIN32 = 2,
  CU_MEM_HANDLE_TYPE_WIN32_KMT = 4,
  CU_MEM_HANDLE_TYPE_FABRIC = 8,
  CU_MEM_HANDLE_TYPE_MAX = 0x7FFFFFFF
} CUmemAllocationHandleType;

typedef enum {
  CU_MEM_ALLOCATION_TYPE_INVALID = 0,
  CU_MEM_ALLOCATION_TYPE_PINNED = 1,
  CU_MEM_ALLOCATION_TYPE_MANAGED = 2,
  CU_MEM_ALLOCATION_TYPE_MAX = 0x7FFFFFFF
} CUmemAllocationType;

typedef enum {
  CU_MEMPOOL_ATTR_REUSE_FOLLOW_EVENT_DEPENDENCIES = 1,
  CU_MEMPOOL_ATTR_REUSE_ALLOW_OPPORTUNISTIC = 2,
  CU_MEMPOOL_ATTR_REUSE_ALLOW_INTERNAL_DEPENDENCIES = 3,
  CU_MEMPOOL_ATTR_RELEASE_THRESHOLD = 4,
  CU_MEMPOOL_ATTR_RESERVED_MEM_CURRENT = 5,
  CU_MEMPOOL_ATTR_RESERVED_MEM_HIGH = 6,
  CU_MEMPOOL_ATTR_USED_MEM_CURRENT = 7,
  CU_MEMPOOL_ATTR_USED_MEM_HIGH = 8,
  CU_MEMPOOL_ATTR_ALLOCATION_TYPE = 9,
  CU_MEMPOOL_ATTR_EXPORT_HANDLE_TYPES = 10,
  CU_MEMPOOL_ATTR_LOCATION_ID = 11,
  CU_MEMPOOL_ATTR_LOCATION_TYPE = 12,
  CU_MEMPOOL_ATTR_MAX_POOL_SIZE = 13,
  CU_MEMPOOL_ATTR_HW_DECOMPRESS_ENABLED = 14
} CUmemPool_attribute;

typedef enum {
  CU_POINTER_ATTRIBUTE_CONTEXT = 1,
  CU_POINTER_ATTRIBUTE_MEMORY_TYPE = 2,
  CU_POINTER_ATTRIBUTE_DEVICE_POINTER = 3,
  CU_POINTER_ATTRIBUTE_HOST_POINTER = 4,
  CU_POINTER_ATTRIBUTE_P2P_TOKENS = 5,
  CU_POINTER_ATTRIBUTE_SYNC_MEMOPS = 6,
  CU_POINTER_ATTRIBUTE_BUFFER_ID = 7,
  CU_POINTER_ATTRIBUTE_IS_MANAGED = 8,
  CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL = 9,
  CU_POINTER_ATTRIBUTE_IS_LEGACY_CUDA_IPC_CAPABLE = 10,
  CU_POINTER_ATTRIBUTE_RANGE_START_ADDR = 11,
  CU_POINTER_ATTRIBUTE_RANGE_SIZE = 12,
  CU_POINTER_ATTRIBUTE_MAPPED = 13,
  CU_POINTER_ATTRIBUTE_ALLOWED_HANDLE_TYPES = 14,
  CU_POINTER_ATTRIBUTE_MEMPOOL_HANDLE = 15,
  CU_POINTER_ATTRIBUTE_IS_HW_DECOMPRESS_CAPABLE = 16
} CUpointer_attribute;

typedef enum {
  cudaMemAdviceSetReadMostly = 1,
  cudaMemAdviceUnsetReadMostly = 2,
  cudaMemAdviceSetPreferredLocation = 3,
  cudaMemAdviceUnsetPreferredLocation = 4,
  cudaMemAdviceSetAccessedBy = 5,
  cudaMemAdviceUnsetAccessedBy = 6
} cudaMemoryAdvise;

typedef enum {
  cudaMemRangeAttributeReadMostly = 1,
  cudaMemRangeAttributePreferredLocation = 2,
  cudaMemRangeAttributeAccessedBy = 3,
  cudaMemRangeAttributeLastPrefetchLocation = 4,
  cudaMemRangeAttributePreferredLocationType = 5,
  cudaMemRangeAttributePreferredLocationId = 6,
  cudaMemRangeAttributeLastPrefetchLocationType = 7,
  cudaMemRangeAttributeLastPrefetchLocationId = 8
} cudaMemRangeAttribute;

typedef enum {
  cudaMemLocationTypeInvalid = 0,
  cudaMemLocationTypeNone = 0,
  cudaMemLocationTypeDevice = 1,
  cudaMemLocationTypeHost = 2,
  cudaMemLocationTypeHostNuma = 3,
  cudaMemLocationTypeHostNumaCurrent = 4,
  cudaMemLocationTypeInvisible = 5
} cudaMemLocationType;

typedef enum {
  cudaMemAccessFlagsProtNone = 0,
  cudaMemAccessFlagsProtRead = 1,
  cudaMemAccessFlagsProtReadWrite = 3
} cudaMemAccessFlags;

typedef enum {
  cudaMemHandleTypeNone = 0,
  cudaMemHandleTypePosixFileDescriptor = 1,
  cudaMemHandleTypeWin32 = 2,
  cudaMemHandleTypeWin32Kmt = 4,
  cudaMemHandleTypeFabric = 8
} cudaMemAllocationHandleType;

typedef enum {
  cudaMemAllocationTypeInvalid = 0,
  cudaMemAllocationTypePinned = 1,
  cudaMemAllocationTypeManaged = 2,
  cudaMemAllocationTypeMax = 0x7FFFFFFF
} cudaMemAllocationType;

typedef enum {
  cudaMemPoolReuseFollowEventDependencies = 1,
  cudaMemPoolReuseAllowOpportunistic = 2,
  cudaMemPoolReuseAllowInternalDependencies = 3,
  cudaMemPoolAttrReleaseThreshold = 4,
  cudaMemPoolAttrReservedMemCurrent = 5,
  cudaMemPoolAttrReservedMemHigh = 6,
  cudaMemPoolAttrUsedMemCurrent = 7,
  cudaMemPoolAttrUsedMemHigh = 8,
  cudaMemPoolAttrAllocationType = 9,
  cudaMemPoolAttrExportHandleTypes = 10,
  cudaMemPoolAttrLocationId = 11,
  cudaMemPoolAttrLocationType = 12,
  cudaMemPoolAttrMaxPoolSize = 13,
  cudaMemPoolAttrHwDecompressEnabled = 14
} cudaMemPoolAttr;

#define CU_MEM_ATTACH_GLOBAL 0x01
#define CU_MEM_ATTACH_HOST 0x02
#define CU_MEM_ATTACH_SINGLE 0x04

#define cudaMemAttachGlobal 0x01
#define cudaMemAttachHost 0x02
#define cudaMemAttachSingle 0x04

#define CU_DEVICE_CPU -1
#define CU_DEVICE_INVALID -2
#define cudaCpuDeviceId -1
#define cudaInvalidDeviceId -2

typedef struct CUmemLocation_st {
  int id;
  CUmemLocationType type;
} CUmemLocation;

typedef struct cudaMemLocation {
  int id;
  cudaMemLocationType type;
} cudaMemLocation;

typedef struct CUmemAccessDesc_st {
  CUmemLocation location;
  CUmemAccess_flags flags;
} CUmemAccessDesc;

typedef struct cudaMemAccessDesc {
  cudaMemLocation location;
  cudaMemAccessFlags flags;
} cudaMemAccessDesc;

typedef struct CUmemPoolProps_st {
  CUmemAllocationType allocType;
  CUmemAllocationHandleType handleTypes;
  CUmemLocation location;
  size_t maxSize;
  unsigned char reserved[54];
  unsigned short usage;
  void *win32SecurityAttributes;
} CUmemPoolProps;

typedef struct cudaMemPoolProps {
  cudaMemAllocationType allocType;
  cudaMemAllocationHandleType handleTypes;
  cudaMemLocation location;
  size_t maxSize;
  unsigned char reserved[54];
  unsigned short usage;
  void *win32SecurityAttributes;
} cudaMemPoolProps;

typedef struct CUmemPoolPtrExportData_st {
  unsigned char reserved[64];
} CUmemPoolPtrExportData;

typedef struct cudaMemPoolPtrExportData {
  unsigned char reserved[64];
} cudaMemPoolPtrExportData;

typedef void *cudaMemPool_t;

typedef struct cudaPointerAttributes {
  int device;
  void *devicePointer;
  void *hostPointer;
  long reserved[8];
  cudaMemoryType type;
} cudaPointerAttributes;

typedef void *cudaArray_t;

typedef struct cudaPitchedPtr {
  void *ptr;
  size_t pitch;
  size_t xsize;
  size_t ysize;
} cudaPitchedPtr;

typedef struct cudaExtent {
  size_t width;
  size_t height;
  size_t depth;
} cudaExtent;

typedef struct cudaPos {
  size_t x;
  size_t y;
  size_t z;
} cudaPos;

typedef struct cudaMemcpy3DParms {
  cudaArray_t srcArray;
  cudaPos srcPos;
  cudaPitchedPtr srcPtr;
  cudaArray_t dstArray;
  cudaPos dstPos;
  cudaPitchedPtr dstPtr;
  cudaExtent extent;
  cudaMemcpyKind kind;
} cudaMemcpy3DParms;

typedef enum {
  CU_MEMORYTYPE_HOST = 0x01,
  CU_MEMORYTYPE_DEVICE = 0x02,
  CU_MEMORYTYPE_ARRAY = 0x03,
  CU_MEMORYTYPE_UNIFIED = 0x04
} CUmemorytype;

typedef struct CUDA_MEMCPY2D_st {
  size_t srcXInBytes;
  size_t srcY;
  CUmemorytype srcMemoryType;
  const void *srcHost;
  CUdeviceptr srcDevice;
  CUarray srcArray;
  size_t srcPitch;
  size_t dstXInBytes;
  size_t dstY;
  CUmemorytype dstMemoryType;
  void *dstHost;
  CUdeviceptr dstDevice;
  CUarray dstArray;
  size_t dstPitch;
  size_t WidthInBytes;
  size_t Height;
} CUDA_MEMCPY2D;

typedef struct CUDA_MEMCPY3D_st {
  size_t srcXInBytes;
  size_t srcY;
  size_t srcZ;
  size_t srcLOD;
  CUmemorytype srcMemoryType;
  const void *srcHost;
  CUdeviceptr srcDevice;
  CUarray srcArray;
  void *reserved0;
  size_t srcPitch;
  size_t srcHeight;
  size_t dstXInBytes;
  size_t dstY;
  size_t dstZ;
  size_t dstLOD;
  CUmemorytype dstMemoryType;
  void *dstHost;
  CUdeviceptr dstDevice;
  CUarray dstArray;
  void *reserved1;
  size_t dstPitch;
  size_t dstHeight;
  size_t WidthInBytes;
  size_t Height;
  size_t Depth;
} CUDA_MEMCPY3D;

#if defined(__cplusplus)
#define PSYCHE_CUDA_STUB_STATIC_ASSERT static_assert
#else
#define PSYCHE_CUDA_STUB_STATIC_ASSERT _Static_assert
#endif

PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(CUarray) == sizeof(void *), "CUarray ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(CUdeviceptr) == 8, "CUdeviceptr ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(CU_MEMORYTYPE_HOST == 1, "CU_MEMORYTYPE_HOST ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(CU_MEMORYTYPE_DEVICE == 2, "CU_MEMORYTYPE_DEVICE ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(CU_MEMORYTYPE_ARRAY == 3, "CU_MEMORYTYPE_ARRAY ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(CU_MEMORYTYPE_UNIFIED == 4, "CU_MEMORYTYPE_UNIFIED ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(CU_DEVICE_CPU == -1, "CU_DEVICE_CPU ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(CU_DEVICE_INVALID == -2, "CU_DEVICE_INVALID ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(cudaCpuDeviceId == -1, "cudaCpuDeviceId ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(cudaInvalidDeviceId == -2, "cudaInvalidDeviceId ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(cudaMemoryTypeUnregistered == 0, "cudaMemoryTypeUnregistered ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(cudaMemoryTypeHost == 1, "cudaMemoryTypeHost ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(cudaMemoryTypeDevice == 2, "cudaMemoryTypeDevice ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(cudaMemoryTypeManaged == 3, "cudaMemoryTypeManaged ABI drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(CUmemLocation) == 8, "CUmemLocation size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUmemLocation, id) == 0, "CUmemLocation id offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUmemLocation, type) == 4, "CUmemLocation type offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(cudaMemLocation) == 8, "cudaMemLocation size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemLocation, id) == 0, "cudaMemLocation id offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemLocation, type) == 4, "cudaMemLocation type offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(CUmemPoolProps) == 88, "CUmemPoolProps size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUmemPoolProps, allocType) == 0, "CUmemPoolProps allocType offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUmemPoolProps, handleTypes) == 4, "CUmemPoolProps handleTypes offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUmemPoolProps, location) == 8, "CUmemPoolProps location offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUmemPoolProps, maxSize) == 16, "CUmemPoolProps maxSize offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUmemPoolProps, reserved) == 24, "CUmemPoolProps reserved offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUmemPoolProps, usage) == 78, "CUmemPoolProps usage offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUmemPoolProps, win32SecurityAttributes) == 80, "CUmemPoolProps win32SecurityAttributes offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(cudaMemPoolProps) == 88, "cudaMemPoolProps size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemPoolProps, allocType) == 0, "cudaMemPoolProps allocType offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemPoolProps, handleTypes) == 4, "cudaMemPoolProps handleTypes offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemPoolProps, location) == 8, "cudaMemPoolProps location offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemPoolProps, maxSize) == 16, "cudaMemPoolProps maxSize offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemPoolProps, reserved) == 24, "cudaMemPoolProps reserved offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemPoolProps, usage) == 78, "cudaMemPoolProps usage offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemPoolProps, win32SecurityAttributes) == 80, "cudaMemPoolProps win32SecurityAttributes offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(CUmemPoolPtrExportData) == 64, "CUmemPoolPtrExportData size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(cudaMemPoolPtrExportData) == 64, "cudaMemPoolPtrExportData size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(cudaPointerAttributes) == 96, "cudaPointerAttributes size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaPointerAttributes, device) == 0, "cudaPointerAttributes device offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaPointerAttributes, devicePointer) == 8, "cudaPointerAttributes devicePointer offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaPointerAttributes, hostPointer) == 16, "cudaPointerAttributes hostPointer offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaPointerAttributes, reserved) == 24, "cudaPointerAttributes reserved offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaPointerAttributes, type) == 88, "cudaPointerAttributes type offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(CUDA_MEMCPY2D) == 128, "CUDA_MEMCPY2D size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, srcXInBytes) == 0, "CUDA_MEMCPY2D srcXInBytes offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, srcY) == 8, "CUDA_MEMCPY2D srcY offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, srcMemoryType) == 16, "CUDA_MEMCPY2D srcMemoryType offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, srcHost) == 24, "CUDA_MEMCPY2D srcHost offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, srcDevice) == 32, "CUDA_MEMCPY2D srcDevice offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, srcArray) == 40, "CUDA_MEMCPY2D srcArray offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, srcPitch) == 48, "CUDA_MEMCPY2D srcPitch offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, dstXInBytes) == 56, "CUDA_MEMCPY2D dstXInBytes offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, dstY) == 64, "CUDA_MEMCPY2D dstY offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, dstMemoryType) == 72, "CUDA_MEMCPY2D dstMemoryType offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, dstHost) == 80, "CUDA_MEMCPY2D dstHost offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, dstDevice) == 88, "CUDA_MEMCPY2D dstDevice offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, dstArray) == 96, "CUDA_MEMCPY2D dstArray offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, dstPitch) == 104, "CUDA_MEMCPY2D dstPitch offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, WidthInBytes) == 112, "CUDA_MEMCPY2D WidthInBytes offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY2D, Height) == 120, "CUDA_MEMCPY2D Height offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(CUDA_MEMCPY3D) == 200, "CUDA_MEMCPY3D size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, srcXInBytes) == 0, "CUDA_MEMCPY3D srcXInBytes offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, srcY) == 8, "CUDA_MEMCPY3D srcY offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, srcZ) == 16, "CUDA_MEMCPY3D srcZ offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, srcLOD) == 24, "CUDA_MEMCPY3D srcLOD offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, srcMemoryType) == 32, "CUDA_MEMCPY3D srcMemoryType offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, srcHost) == 40, "CUDA_MEMCPY3D srcHost offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, srcDevice) == 48, "CUDA_MEMCPY3D srcDevice offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, srcArray) == 56, "CUDA_MEMCPY3D srcArray offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, reserved0) == 64, "CUDA_MEMCPY3D reserved0 offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, srcPitch) == 72, "CUDA_MEMCPY3D srcPitch offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, srcHeight) == 80, "CUDA_MEMCPY3D srcHeight offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, dstXInBytes) == 88, "CUDA_MEMCPY3D dstXInBytes offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, dstY) == 96, "CUDA_MEMCPY3D dstY offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, dstZ) == 104, "CUDA_MEMCPY3D dstZ offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, dstLOD) == 112, "CUDA_MEMCPY3D dstLOD offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, dstMemoryType) == 120, "CUDA_MEMCPY3D dstMemoryType offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, dstHost) == 128, "CUDA_MEMCPY3D dstHost offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, dstDevice) == 136, "CUDA_MEMCPY3D dstDevice offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, dstArray) == 144, "CUDA_MEMCPY3D dstArray offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, reserved1) == 152, "CUDA_MEMCPY3D reserved1 offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, dstPitch) == 160, "CUDA_MEMCPY3D dstPitch offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, dstHeight) == 168, "CUDA_MEMCPY3D dstHeight offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, WidthInBytes) == 176, "CUDA_MEMCPY3D WidthInBytes offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, Height) == 184, "CUDA_MEMCPY3D Height offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(CUDA_MEMCPY3D, Depth) == 192, "CUDA_MEMCPY3D Depth offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(cudaPitchedPtr) == 32, "cudaPitchedPtr size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(cudaExtent) == 24, "cudaExtent size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(cudaPos) == 24, "cudaPos size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(sizeof(cudaMemcpy3DParms) == 160, "cudaMemcpy3DParms size drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemcpy3DParms, srcArray) == 0, "cudaMemcpy3DParms srcArray offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemcpy3DParms, srcPos) == 8, "cudaMemcpy3DParms srcPos offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemcpy3DParms, srcPtr) == 32, "cudaMemcpy3DParms srcPtr offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemcpy3DParms, dstArray) == 64, "cudaMemcpy3DParms dstArray offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemcpy3DParms, dstPos) == 72, "cudaMemcpy3DParms dstPos offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemcpy3DParms, dstPtr) == 96, "cudaMemcpy3DParms dstPtr offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemcpy3DParms, extent) == 128, "cudaMemcpy3DParms extent offset drift");
PSYCHE_CUDA_STUB_STATIC_ASSERT(offsetof(cudaMemcpy3DParms, kind) == 152, "cudaMemcpy3DParms kind offset drift");

typedef void *cudaStream_t;
typedef void *cudaEvent_t;

typedef struct {
  unsigned int x;
  unsigned int y;
  unsigned int z;
} dim3;

enum {
  NVML_SUCCESS = 0,
  NVML_ERROR_UNINITIALIZED = 1,
  NVML_ERROR_INVALID_ARGUMENT = 2,
  NVML_ERROR_NOT_SUPPORTED = 3,
  NVML_ERROR_NO_PERMISSION = 4,
  NVML_ERROR_ALREADY_INITIALIZED = 5,
  NVML_ERROR_NOT_FOUND = 6,
  NVML_ERROR_INSUFFICIENT_SIZE = 7,
  NVML_ERROR_INSUFFICIENT_POWER = 8,
  NVML_ERROR_DRIVER_NOT_LOADED = 9,
  NVML_ERROR_TIMEOUT = 10,
  NVML_ERROR_IRQ_ISSUE = 11,
  NVML_ERROR_LIBRARY_NOT_FOUND = 12,
  NVML_ERROR_FUNCTION_NOT_FOUND = 13,
  NVML_ERROR_CORRUPTED_INFOROM = 14,
  NVML_ERROR_GPU_IS_LOST = 15,
  NVML_ERROR_RESET_REQUIRED = 16,
  NVML_ERROR_OPERATING_SYSTEM = 17,
  NVML_ERROR_LIB_RM_VERSION_MISMATCH = 18,
  NVML_ERROR_IN_USE = 19,
  NVML_ERROR_MEMORY = 20,
  NVML_ERROR_NO_DATA = 21,
  NVML_ERROR_VGPU_ECC_NOT_SUPPORTED = 22,
  NVML_ERROR_INSUFFICIENT_RESOURCES = 23,
  NVML_ERROR_FREQ_NOT_SUPPORTED = 24,
  NVML_ERROR_ARGUMENT_VERSION_MISMATCH = 25,
  NVML_ERROR_DEPRECATED = 26,
  NVML_ERROR_NOT_READY = 27,
  NVML_ERROR_GPU_NOT_FOUND = 28,
  NVML_ERROR_INVALID_STATE = 29,
  NVML_ERROR_UNKNOWN = 999
};

enum {
  NVML_INIT_FLAG_NO_GPUS = 1u << 0,
  NVML_INIT_FLAG_NO_ATTACH = 1u << 1,
  NVML_INIT_FLAG_FORCE_INIT = 1u << 2
};

enum {
  NVML_DEVICE_NAME_BUFFER_SIZE = 64,
  NVML_DEVICE_UUID_BUFFER_SIZE = 80,
  NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE = 80,
  NVML_SYSTEM_NVML_VERSION_BUFFER_SIZE = 80,
  NVML_TEMPERATURE_GPU = 0
};

typedef struct nvmlMemory_st {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
} nvmlMemory_t;

typedef struct nvmlUtilization_st {
  unsigned int gpu;
  unsigned int memory;
} nvmlUtilization_t;

PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnCreate(cusolverDnHandle_t *handle);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDestroy(cusolverDnHandle_t handle);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnGetVersion(cusolverDnHandle_t handle, int *version);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnGetProperty(libraryPropertyType type, int *value);
PSYCHE_CUDA_STUB_API const char *cusolverGetErrorName(cusolverStatus_t status);
PSYCHE_CUDA_STUB_API const char *cusolverGetErrorString(cusolverStatus_t status);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSetStream(cusolverDnHandle_t handle, void *streamId);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnGetStream(cusolverDnHandle_t handle, void **streamId);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSgetrf_bufferSize(
    cusolverDnHandle_t handle,
    int m,
    int n,
    float *A,
    int lda,
    int *Lwork);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDgetrf_bufferSize(
    cusolverDnHandle_t handle,
    int m,
    int n,
    double *A,
    int lda,
    int *Lwork);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSgetrf(
    cusolverDnHandle_t handle,
    int m,
    int n,
    float *A,
    int lda,
    float *Workspace,
    int *devIpiv,
    int *devInfo);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDgetrf(
    cusolverDnHandle_t handle,
    int m,
    int n,
    double *A,
    int lda,
    double *Workspace,
    int *devIpiv,
    int *devInfo);
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
    int *devInfo);
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
    int *devInfo);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSpotrf_bufferSize(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    float *A,
    int lda,
    int *Lwork);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDpotrf_bufferSize(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    double *A,
    int lda,
    int *Lwork);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSpotrf(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    float *A,
    int lda,
    float *Workspace,
    int Lwork,
    int *devInfo);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDpotrf(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    double *A,
    int lda,
    double *Workspace,
    int Lwork,
    int *devInfo);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnSpotrs(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    int nrhs,
    const float *A,
    int lda,
    float *B,
    int ldb,
    int *devInfo);
PSYCHE_CUDA_STUB_API cusolverStatus_t cusolverDnDpotrs(
    cusolverDnHandle_t handle,
    cublasFillMode_t uplo,
    int n,
    int nrhs,
    const double *A,
    int lda,
    double *B,
    int ldb,
    int *devInfo);

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnConvolutionBiasActivationForward(
    cudnnHandle_t handle,
    const void *alpha1,
    const cudnnTensorDescriptor_t xDesc,
    const void *x,
    const cudnnFilterDescriptor_t wDesc,
    const void *w,
    const cudnnConvolutionDescriptor_t convDesc,
    cudnnConvolutionFwdAlgo_t algo,
    void *workSpace,
    size_t workSpaceSizeInBytes,
    const void *alpha2,
    const cudnnTensorDescriptor_t zDesc,
    const void *z,
    const cudnnTensorDescriptor_t biasDesc,
    const void *bias,
    const cudnnActivationDescriptor_t activationDesc,
    const cudnnTensorDescriptor_t yDesc,
    void *y);

PSYCHE_CUDA_STUB_API void psyche_cuda_runtime_kernel_vector_add_f32(void);
PSYCHE_CUDA_STUB_API void psyche_cuda_runtime_kernel_saxpy_f32(void);
PSYCHE_CUDA_STUB_API void psyche_cuda_runtime_kernel_scale_f32(void);
PSYCHE_CUDA_STUB_API void psyche_cuda_runtime_kernel_axpby_f32(void);

static inline const char *psyche_cuda_stub_error_name(int code) {
  switch (code) {
  case CUDA_SUCCESS:
    return "CUDA_SUCCESS";
  case CUDA_ERROR_INVALID_VALUE:
    return "CUDA_ERROR_INVALID_VALUE";
  case CUDA_ERROR_OUT_OF_MEMORY:
    return "CUDA_ERROR_OUT_OF_MEMORY";
  case CUDA_ERROR_NOT_INITIALIZED:
    return "CUDA_ERROR_NOT_INITIALIZED";
  case CUDA_ERROR_DEINITIALIZED:
    return "CUDA_ERROR_DEINITIALIZED";
  case CUDA_ERROR_INVALID_CONFIGURATION:
    return "CUDA_ERROR_INVALID_CONFIGURATION";
  case CUDA_ERROR_INVALID_DEVICE_FUNCTION:
    return "CUDA_ERROR_INVALID_DEVICE_FUNCTION";
  case CUDA_ERROR_NO_DEVICE:
    return "CUDA_ERROR_NO_DEVICE";
  case CUDA_ERROR_INVALID_DEVICE:
    return "CUDA_ERROR_INVALID_DEVICE";
  case CUDA_ERROR_INVALID_HANDLE:
    return "CUDA_ERROR_INVALID_HANDLE";
  case CUDA_ERROR_NOT_READY:
    return "CUDA_ERROR_NOT_READY";
  case CUDA_ERROR_HOST_MEMORY_ALREADY_REGISTERED:
    return "CUDA_ERROR_HOST_MEMORY_ALREADY_REGISTERED";
  case CUDA_ERROR_HOST_MEMORY_NOT_REGISTERED:
    return "CUDA_ERROR_HOST_MEMORY_NOT_REGISTERED";
  case CUDA_ERROR_NOT_SUPPORTED:
    return "CUDA_ERROR_NOT_SUPPORTED";
  default:
    return "CUDA_ERROR_UNKNOWN";
  }
}

static inline const char *psyche_cuda_stub_error_string(int code) {
  switch (code) {
  case CUDA_SUCCESS:
    return "no error";
  case CUDA_ERROR_INVALID_VALUE:
    return "invalid value";
  case CUDA_ERROR_OUT_OF_MEMORY:
    return "out of memory";
  case CUDA_ERROR_NOT_INITIALIZED:
    return "CUDA driver is not initialized";
  case CUDA_ERROR_DEINITIALIZED:
    return "CUDA driver is deinitialized";
  case CUDA_ERROR_INVALID_CONFIGURATION:
    return "invalid configuration argument";
  case CUDA_ERROR_INVALID_DEVICE_FUNCTION:
    return "invalid device function";
  case CUDA_ERROR_NO_DEVICE:
    return "no CUDA-capable device is detected";
  case CUDA_ERROR_INVALID_DEVICE:
    return "invalid device ordinal";
  case CUDA_ERROR_INVALID_HANDLE:
    return "invalid resource handle";
  case CUDA_ERROR_NOT_READY:
    return "operation is not ready";
  case CUDA_ERROR_HOST_MEMORY_ALREADY_REGISTERED:
    return "host memory is already registered";
  case CUDA_ERROR_HOST_MEMORY_NOT_REGISTERED:
    return "host memory is not registered";
  case CUDA_ERROR_NOT_SUPPORTED:
    return "operation is not supported by the Psyche CUDA compatibility stub";
  default:
    return "unknown CUDA error";
  }
}

#endif
