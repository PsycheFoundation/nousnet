#include "cuda_compat_stub.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const PSYCHE_CUDNN_LAST_ERROR =
    "Psyche cuDNN compatibility shim: discovery plus opt-in simulated activation/add/batchnorm/convolution/transform/pooling/softmax";

enum {
  PSYCHE_CUDNN_MAX_TENSOR_DIMS = 4,
  PSYCHE_CUDNN_BLOCK_THREADS = 256U,
};

#define PSYCHE_CUDNN_CONTEXT_MAGIC UINT64_C(0x5053594348444e48)
#define PSYCHE_CUDNN_TENSOR_MAGIC UINT64_C(0x5053594344544e53)
#define PSYCHE_CUDNN_ACTIVATION_MAGIC UINT64_C(0x5053594344414354)
#define PSYCHE_CUDNN_POOLING_MAGIC UINT64_C(0x5053594344504f4c)
#define PSYCHE_CUDNN_FILTER_MAGIC UINT64_C(0x505359434446494c)
#define PSYCHE_CUDNN_CONVOLUTION_MAGIC UINT64_C(0x5053594344434f4e)

struct cudnnContext {
  uint64_t magic;
  cudaStream_t stream;
  struct cudnnContext *next;
};

struct cudnnTensorStruct {
  uint64_t magic;
  int is_set;
  cudnnDataType_t data_type;
  int nb_dims;
  int dim[PSYCHE_CUDNN_MAX_TENSOR_DIMS];
  int stride[PSYCHE_CUDNN_MAX_TENSOR_DIMS];
  size_t element_count;
  struct cudnnTensorStruct *next;
};

struct cudnnActivationStruct {
  uint64_t magic;
  int is_set;
  cudnnActivationMode_t mode;
  cudnnNanPropagation_t nan_opt;
  double coef;
  struct cudnnActivationStruct *next;
};

struct cudnnPoolingStruct {
  uint64_t magic;
  int is_set;
  cudnnPoolingMode_t mode;
  cudnnNanPropagation_t nan_opt;
  int window_h;
  int window_w;
  int pad_h;
  int pad_w;
  int stride_h;
  int stride_w;
  struct cudnnPoolingStruct *next;
};

struct cudnnFilterStruct {
  uint64_t magic;
  int is_set;
  cudnnDataType_t data_type;
  cudnnTensorFormat_t format;
  int k;
  int c;
  int h;
  int w;
  size_t element_count;
  struct cudnnFilterStruct *next;
};

struct cudnnConvolutionStruct {
  uint64_t magic;
  int is_set;
  int pad_h;
  int pad_w;
  int stride_h;
  int stride_w;
  int dilation_h;
  int dilation_w;
  cudnnConvolutionMode_t mode;
  cudnnDataType_t compute_type;
  int group_count;
  struct cudnnConvolutionStruct *next;
};

static pthread_mutex_t psyche_cudnn_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct cudnnContext *psyche_cudnn_contexts = 0;
static struct cudnnTensorStruct *psyche_cudnn_tensors = 0;
static struct cudnnActivationStruct *psyche_cudnn_activations = 0;
static struct cudnnPoolingStruct *psyche_cudnn_poolings = 0;
static struct cudnnFilterStruct *psyche_cudnn_filters = 0;
static struct cudnnConvolutionStruct *psyche_cudnn_convolutions = 0;

#if defined(__APPLE__)
extern CUresult psyche_cuda_metal_launch_cudnn_activation_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_activation_backward_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_transform_tensor_f32(
    const float *x,
    const float *y_in,
    float *out,
    float alpha,
    float beta,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_add_tensor_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_batchnorm_inference_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_convolution_forward_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_convolution_bias_activation_forward_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_convolution_backward_data_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_convolution_backward_filter_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_pooling_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_pooling_backward_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_softmax_f32(
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
    unsigned int blockDimX);

extern CUresult psyche_cuda_metal_launch_cudnn_softmax_backward_f32(
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
    unsigned int blockDimX);
#endif

static int psyche_cudnn_env_truthy(const char *value) {
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

static int psyche_cudnn_env_required(const char *value) {
  return value != 0 && strcasecmp(value, "required") == 0;
}

static int psyche_cudnn_simulated_memory_enabled(void) {
  return psyche_cudnn_env_truthy(getenv("PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY"));
}

static int psyche_cudnn_metal_enabled(void) {
  const char *value = getenv("PSYCHE_CUDA_COMPAT_CUDNN_METAL");
  return psyche_cudnn_env_truthy(value) || psyche_cudnn_env_required(value);
}

static int psyche_cudnn_metal_required(void) {
  return psyche_cudnn_env_required(getenv("PSYCHE_CUDA_COMPAT_CUDNN_METAL"));
}

static cudnnStatus_t psyche_cudnn_status_from_cuda_result(CUresult result) {
  switch (result) {
  case CUDA_SUCCESS:
    return CUDNN_STATUS_SUCCESS;
  case CUDA_ERROR_INVALID_VALUE:
  case CUDA_ERROR_INVALID_HANDLE:
    return CUDNN_STATUS_BAD_PARAM;
  case CUDA_ERROR_OUT_OF_MEMORY:
    return CUDNN_STATUS_ALLOC_FAILED;
  case CUDA_ERROR_NOT_INITIALIZED:
  case CUDA_ERROR_NO_DEVICE:
  case CUDA_ERROR_INVALID_DEVICE:
  case CUDA_ERROR_NOT_SUPPORTED:
    return CUDNN_STATUS_NOT_SUPPORTED;
  case CUDA_ERROR_UNKNOWN:
    return CUDNN_STATUS_EXECUTION_FAILED;
  default:
    return CUDNN_STATUS_INTERNAL_ERROR;
  }
}

static int psyche_cudnn_metal_preferred_can_fallback(CUresult result) {
  return
      result == CUDA_ERROR_NOT_SUPPORTED ||
      result == CUDA_ERROR_NO_DEVICE ||
      result == CUDA_ERROR_NOT_INITIALIZED;
}

static struct cudnnContext *psyche_cudnn_find_context_locked(cudnnHandle_t handle) {
  struct cudnnContext *current = psyche_cudnn_contexts;
  while (current != 0) {
    if (current == handle && current->magic == PSYCHE_CUDNN_CONTEXT_MAGIC) {
      return current;
    }
    current = current->next;
  }
  return 0;
}

static struct cudnnTensorStruct *psyche_cudnn_find_tensor_locked(
    cudnnTensorDescriptor_t descriptor) {
  struct cudnnTensorStruct *current = psyche_cudnn_tensors;
  while (current != 0) {
    if (current == descriptor && current->magic == PSYCHE_CUDNN_TENSOR_MAGIC) {
      return current;
    }
    current = current->next;
  }
  return 0;
}

static struct cudnnActivationStruct *psyche_cudnn_find_activation_locked(
    cudnnActivationDescriptor_t descriptor) {
  struct cudnnActivationStruct *current = psyche_cudnn_activations;
  while (current != 0) {
    if (current == descriptor && current->magic == PSYCHE_CUDNN_ACTIVATION_MAGIC) {
      return current;
    }
    current = current->next;
  }
  return 0;
}

static struct cudnnPoolingStruct *psyche_cudnn_find_pooling_locked(
    cudnnPoolingDescriptor_t descriptor) {
  struct cudnnPoolingStruct *current = psyche_cudnn_poolings;
  while (current != 0) {
    if (current == descriptor && current->magic == PSYCHE_CUDNN_POOLING_MAGIC) {
      return current;
    }
    current = current->next;
  }
  return 0;
}

static struct cudnnFilterStruct *psyche_cudnn_find_filter_locked(
    cudnnFilterDescriptor_t descriptor) {
  struct cudnnFilterStruct *current = psyche_cudnn_filters;
  while (current != 0) {
    if (current == descriptor && current->magic == PSYCHE_CUDNN_FILTER_MAGIC) {
      return current;
    }
    current = current->next;
  }
  return 0;
}

static struct cudnnConvolutionStruct *psyche_cudnn_find_convolution_locked(
    cudnnConvolutionDescriptor_t descriptor) {
  struct cudnnConvolutionStruct *current = psyche_cudnn_convolutions;
  while (current != 0) {
    if (current == descriptor && current->magic == PSYCHE_CUDNN_CONVOLUTION_MAGIC) {
      return current;
    }
    current = current->next;
  }
  return 0;
}

static int psyche_cudnn_remove_context_locked(cudnnHandle_t handle) {
  struct cudnnContext **link = &psyche_cudnn_contexts;
  while (*link != 0) {
    struct cudnnContext *current = *link;
    if (current == handle && current->magic == PSYCHE_CUDNN_CONTEXT_MAGIC) {
      *link = current->next;
      current->magic = 0;
      current->next = 0;
      return 1;
    }
    link = &current->next;
  }
  return 0;
}

static int psyche_cudnn_remove_tensor_locked(cudnnTensorDescriptor_t descriptor) {
  struct cudnnTensorStruct **link = &psyche_cudnn_tensors;
  while (*link != 0) {
    struct cudnnTensorStruct *current = *link;
    if (current == descriptor && current->magic == PSYCHE_CUDNN_TENSOR_MAGIC) {
      *link = current->next;
      current->magic = 0;
      current->next = 0;
      return 1;
    }
    link = &current->next;
  }
  return 0;
}

static int psyche_cudnn_remove_activation_locked(cudnnActivationDescriptor_t descriptor) {
  struct cudnnActivationStruct **link = &psyche_cudnn_activations;
  while (*link != 0) {
    struct cudnnActivationStruct *current = *link;
    if (current == descriptor && current->magic == PSYCHE_CUDNN_ACTIVATION_MAGIC) {
      *link = current->next;
      current->magic = 0;
      current->next = 0;
      return 1;
    }
    link = &current->next;
  }
  return 0;
}

static int psyche_cudnn_remove_pooling_locked(cudnnPoolingDescriptor_t descriptor) {
  struct cudnnPoolingStruct **link = &psyche_cudnn_poolings;
  while (*link != 0) {
    struct cudnnPoolingStruct *current = *link;
    if (current == descriptor && current->magic == PSYCHE_CUDNN_POOLING_MAGIC) {
      *link = current->next;
      current->magic = 0;
      current->next = 0;
      return 1;
    }
    link = &current->next;
  }
  return 0;
}

static int psyche_cudnn_remove_filter_locked(cudnnFilterDescriptor_t descriptor) {
  struct cudnnFilterStruct **link = &psyche_cudnn_filters;
  while (*link != 0) {
    struct cudnnFilterStruct *current = *link;
    if (current == descriptor && current->magic == PSYCHE_CUDNN_FILTER_MAGIC) {
      *link = current->next;
      current->magic = 0;
      current->next = 0;
      return 1;
    }
    link = &current->next;
  }
  return 0;
}

static int psyche_cudnn_remove_convolution_locked(cudnnConvolutionDescriptor_t descriptor) {
  struct cudnnConvolutionStruct **link = &psyche_cudnn_convolutions;
  while (*link != 0) {
    struct cudnnConvolutionStruct *current = *link;
    if (current == descriptor && current->magic == PSYCHE_CUDNN_CONVOLUTION_MAGIC) {
      *link = current->next;
      current->magic = 0;
      current->next = 0;
      return 1;
    }
    link = &current->next;
  }
  return 0;
}

static int psyche_cudnn_tensor_values_equal(
    const struct cudnnTensorStruct *a,
    const struct cudnnTensorStruct *b) {
  int index;
  if (
      a->is_set != b->is_set ||
      a->data_type != b->data_type ||
      a->nb_dims != b->nb_dims ||
      a->element_count != b->element_count) {
    return 0;
  }
  for (index = 0; index < PSYCHE_CUDNN_MAX_TENSOR_DIMS; index++) {
    if (a->dim[index] != b->dim[index] || a->stride[index] != b->stride[index]) {
      return 0;
    }
  }
  return 1;
}

static int psyche_cudnn_tensor_dims_equal(
    const struct cudnnTensorStruct *a,
    const struct cudnnTensorStruct *b) {
  int index;
  if (a->nb_dims != b->nb_dims) {
    return 0;
  }
  for (index = 0; index < PSYCHE_CUDNN_MAX_TENSOR_DIMS; index++) {
    if (a->dim[index] != b->dim[index]) {
      return 0;
    }
  }
  return 1;
}

static int psyche_cudnn_tensor_is_contiguous_nchw(const struct cudnnTensorStruct *tensor) {
  size_t n;
  size_t c;
  size_t h;
  size_t w;
  size_t count;
  size_t stride_n;
  size_t stride_c;
  size_t stride_h;
  if (
      tensor == 0 ||
      !tensor->is_set ||
      tensor->data_type != CUDNN_DATA_FLOAT ||
      tensor->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      tensor->dim[0] <= 0 ||
      tensor->dim[1] <= 0 ||
      tensor->dim[2] <= 0 ||
      tensor->dim[3] <= 0) {
    return 0;
  }
  n = (size_t)tensor->dim[0];
  c = (size_t)tensor->dim[1];
  h = (size_t)tensor->dim[2];
  w = (size_t)tensor->dim[3];
  if (n > SIZE_MAX / c || n * c > SIZE_MAX / h || n * c * h > SIZE_MAX / w) {
    return 0;
  }
  count = n * c * h * w;
  stride_h = w;
  stride_c = h * w;
  stride_n = c * h * w;
  if (
      count != tensor->element_count ||
      stride_n > (size_t)INT_MAX ||
      stride_c > (size_t)INT_MAX ||
      stride_h > (size_t)INT_MAX) {
    return 0;
  }
  return
      tensor->stride[3] == 1 &&
      tensor->stride[2] == (int)stride_h &&
      tensor->stride[1] == (int)stride_c &&
      tensor->stride[0] == (int)stride_n;
}

static int psyche_cudnn_tensor_can_broadcast_to(
    const struct cudnnTensorStruct *source,
    const struct cudnnTensorStruct *destination) {
  int index;
  if (source == 0 || destination == 0 || source->nb_dims != destination->nb_dims) {
    return 0;
  }
  for (index = 0; index < PSYCHE_CUDNN_MAX_TENSOR_DIMS; index++) {
    if (source->dim[index] != 1 && source->dim[index] != destination->dim[index]) {
      return 0;
    }
  }
  return 1;
}

static int psyche_cudnn_filter_is_contiguous_kcrs(const struct cudnnFilterStruct *filter) {
  if (
      filter == 0 ||
      !filter->is_set ||
      filter->data_type != CUDNN_DATA_FLOAT ||
      filter->format != CUDNN_TENSOR_NCHW ||
      filter->k <= 0 ||
      filter->c <= 0 ||
      filter->h <= 0 ||
      filter->w <= 0) {
    return 0;
  }
  return filter->element_count ==
      (size_t)filter->k * (size_t)filter->c * (size_t)filter->h * (size_t)filter->w;
}

static int psyche_cudnn_convolution_output_dim(
    int input_dim,
    int padding,
    int filter_dim,
    int stride,
    int dilation,
    int *output_dim) {
  long long numerator;
  long long output;
  if (
      output_dim == 0 ||
      input_dim <= 0 ||
      padding < 0 ||
      filter_dim <= 0 ||
      stride <= 0 ||
      dilation <= 0) {
    return 0;
  }
  numerator =
      (long long)input_dim +
      2LL * (long long)padding -
      (long long)dilation * ((long long)filter_dim - 1LL) -
      1LL;
  if (numerator < 0) {
    return 0;
  }
  output = numerator / (long long)stride + 1LL;
  if (output <= 0 || output > (long long)INT_MAX) {
    return 0;
  }
  *output_dim = (int)output;
  return 1;
}

static int psyche_cudnn_convolution_output_dims(
    const struct cudnnTensorStruct *x_desc,
    const struct cudnnFilterStruct *w_desc,
    const struct cudnnConvolutionStruct *conv_desc,
    int *out_n,
    int *out_c,
    int *out_h,
    int *out_w) {
  int computed_h;
  int computed_w;
  int group_count;
  int in_channels_per_group;
  size_t expected_filter_elements;
  if (
      x_desc == 0 ||
      w_desc == 0 ||
      conv_desc == 0 ||
      out_n == 0 ||
      out_c == 0 ||
      out_h == 0 ||
      out_w == 0 ||
      !x_desc->is_set ||
      !w_desc->is_set ||
      !conv_desc->is_set ||
      x_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      conv_desc->group_count <= 0) {
    return 0;
  }
  group_count = conv_desc->group_count;
  if (
      x_desc->dim[1] % group_count != 0 ||
      w_desc->k % group_count != 0) {
    return 0;
  }
  in_channels_per_group = x_desc->dim[1] / group_count;
  if (w_desc->c != in_channels_per_group) {
    return 0;
  }
  if (
      (size_t)w_desc->k > SIZE_MAX / (size_t)w_desc->c ||
      (size_t)w_desc->k * (size_t)w_desc->c > SIZE_MAX / (size_t)w_desc->h ||
      (size_t)w_desc->k * (size_t)w_desc->c * (size_t)w_desc->h > SIZE_MAX / (size_t)w_desc->w) {
    return 0;
  }
  expected_filter_elements =
      (size_t)w_desc->k * (size_t)w_desc->c * (size_t)w_desc->h * (size_t)w_desc->w;
  if (w_desc->element_count != expected_filter_elements) {
    return 0;
  }
  if (
      !psyche_cudnn_convolution_output_dim(
          x_desc->dim[2],
          conv_desc->pad_h,
          w_desc->h,
          conv_desc->stride_h,
          conv_desc->dilation_h,
          &computed_h) ||
      !psyche_cudnn_convolution_output_dim(
          x_desc->dim[3],
          conv_desc->pad_w,
          w_desc->w,
          conv_desc->stride_w,
          conv_desc->dilation_w,
          &computed_w)) {
    return 0;
  }
  *out_n = x_desc->dim[0];
  *out_c = w_desc->k;
  *out_h = computed_h;
  *out_w = computed_w;
  return 1;
}

static cudnnStatus_t psyche_cudnn_validate_convolution_forward_config(
    const struct cudnnTensorStruct *x_desc,
    const struct cudnnFilterStruct *w_desc,
    const struct cudnnConvolutionStruct *conv_desc,
    const struct cudnnTensorStruct *y_desc,
    cudnnConvolutionFwdAlgo_t algo) {
  int expected_n;
  int expected_c;
  int expected_h;
  int expected_w;
  if (algo != CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      x_desc == 0 ||
      w_desc == 0 ||
      conv_desc == 0 ||
      y_desc == 0 ||
      !x_desc->is_set ||
      !w_desc->is_set ||
      !conv_desc->is_set ||
      !y_desc->is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (x_desc->data_type != y_desc->data_type) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      x_desc->data_type != CUDNN_DATA_FLOAT ||
      w_desc->data_type != CUDNN_DATA_FLOAT ||
      conv_desc->compute_type != CUDNN_DATA_FLOAT) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      x_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      y_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(x_desc) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(y_desc) ||
      !psyche_cudnn_filter_is_contiguous_kcrs(w_desc)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (conv_desc->group_count <= 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      !psyche_cudnn_convolution_output_dims(
          x_desc,
          w_desc,
          conv_desc,
          &expected_n,
          &expected_c,
          &expected_h,
          &expected_w)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      y_desc->dim[0] != expected_n ||
      y_desc->dim[1] != expected_c ||
      y_desc->dim[2] != expected_h ||
      y_desc->dim[3] != expected_w) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  return CUDNN_STATUS_SUCCESS;
}

static cudnnStatus_t psyche_cudnn_validate_convolution_bias_activation_forward_config(
    const struct cudnnTensorStruct *x_desc,
    const struct cudnnFilterStruct *w_desc,
    const struct cudnnConvolutionStruct *conv_desc,
    cudnnConvolutionFwdAlgo_t algo,
    const struct cudnnTensorStruct *z_desc,
    const struct cudnnTensorStruct *bias_desc,
    const struct cudnnActivationStruct *activation_desc,
    const struct cudnnTensorStruct *y_desc) {
  int expected_n;
  int expected_c;
  int expected_h;
  int expected_w;
  if (
      x_desc == 0 ||
      w_desc == 0 ||
      conv_desc == 0 ||
      z_desc == 0 ||
      bias_desc == 0 ||
      activation_desc == 0 ||
      y_desc == 0 ||
      !x_desc->is_set ||
      !w_desc->is_set ||
      !conv_desc->is_set ||
      !z_desc->is_set ||
      !bias_desc->is_set ||
      !activation_desc->is_set ||
      !y_desc->is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (activation_desc->mode == CUDNN_ACTIVATION_IDENTITY) {
    if (algo != CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
  } else if (activation_desc->mode == CUDNN_ACTIVATION_RELU) {
    if (algo != CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
  } else {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (activation_desc->nan_opt != CUDNN_NOT_PROPAGATE_NAN) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (x_desc->data_type != y_desc->data_type) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (z_desc->data_type != y_desc->data_type || bias_desc->data_type != y_desc->data_type) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      x_desc->data_type != CUDNN_DATA_FLOAT ||
      w_desc->data_type != CUDNN_DATA_FLOAT ||
      y_desc->data_type != CUDNN_DATA_FLOAT ||
      conv_desc->compute_type != CUDNN_DATA_FLOAT) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      x_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      y_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      z_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      bias_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(x_desc) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(y_desc) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(z_desc) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(bias_desc) ||
      !psyche_cudnn_filter_is_contiguous_kcrs(w_desc)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  /* The bounded fused path only supports contiguous descriptors, so value equality is the safe z/y match. */
  if (!psyche_cudnn_tensor_values_equal(z_desc, y_desc)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      bias_desc->dim[0] != 1 ||
      bias_desc->dim[1] != y_desc->dim[1] ||
      bias_desc->dim[2] != 1 ||
      bias_desc->dim[3] != 1 ||
      bias_desc->stride[1] != 1) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (conv_desc->group_count <= 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      !psyche_cudnn_convolution_output_dims(
          x_desc,
          w_desc,
          conv_desc,
          &expected_n,
          &expected_c,
          &expected_h,
          &expected_w)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      y_desc->dim[0] != expected_n ||
      y_desc->dim[1] != expected_c ||
      y_desc->dim[2] != expected_h ||
      y_desc->dim[3] != expected_w) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  return CUDNN_STATUS_SUCCESS;
}

static void psyche_cudnn_fill_convolution_forward_perf(
    cudnnConvolutionFwdAlgoPerf_t *perf) {
  memset(perf, 0, sizeof(*perf));
  perf->algo = CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM;
  perf->status = CUDNN_STATUS_SUCCESS;
  perf->time = 0.0f;
  perf->memory = 0;
  perf->determinism = CUDNN_DETERMINISTIC;
  perf->mathType = CUDNN_DEFAULT_MATH;
}

static cudnnStatus_t psyche_cudnn_validate_convolution_backward_data_config(
    const struct cudnnFilterStruct *w_desc,
    const struct cudnnTensorStruct *dy_desc,
    const struct cudnnConvolutionStruct *conv_desc,
    const struct cudnnTensorStruct *dx_desc,
    cudnnConvolutionBwdDataAlgo_t algo) {
  int expected_n;
  int expected_c;
  int expected_h;
  int expected_w;
  if (algo != CUDNN_CONVOLUTION_BWD_DATA_ALGO_1) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      w_desc == 0 ||
      dy_desc == 0 ||
      conv_desc == 0 ||
      dx_desc == 0 ||
      !w_desc->is_set ||
      !dy_desc->is_set ||
      !conv_desc->is_set ||
      !dx_desc->is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (dy_desc->data_type != dx_desc->data_type) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      dy_desc->data_type != CUDNN_DATA_FLOAT ||
      dx_desc->data_type != CUDNN_DATA_FLOAT ||
      w_desc->data_type != CUDNN_DATA_FLOAT ||
      conv_desc->compute_type != CUDNN_DATA_FLOAT) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      dy_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      dx_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(dy_desc) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(dx_desc) ||
      !psyche_cudnn_filter_is_contiguous_kcrs(w_desc)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (conv_desc->group_count <= 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  /*
   * Backward-data recovers the gradient of the forward input.  Treat dxDesc as
   * the forward x descriptor and require dyDesc to match the forward y shape.
   */
  if (
      !psyche_cudnn_convolution_output_dims(
          dx_desc,
          w_desc,
          conv_desc,
          &expected_n,
          &expected_c,
          &expected_h,
          &expected_w)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      dy_desc->dim[0] != expected_n ||
      dy_desc->dim[1] != expected_c ||
      dy_desc->dim[2] != expected_h ||
      dy_desc->dim[3] != expected_w) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  return CUDNN_STATUS_SUCCESS;
}

static void psyche_cudnn_fill_convolution_backward_data_perf(
    cudnnConvolutionBwdDataAlgoPerf_t *perf) {
  memset(perf, 0, sizeof(*perf));
  perf->algo = CUDNN_CONVOLUTION_BWD_DATA_ALGO_1;
  perf->status = CUDNN_STATUS_SUCCESS;
  perf->time = 0.0f;
  perf->memory = 0;
  perf->determinism = CUDNN_DETERMINISTIC;
  perf->mathType = CUDNN_DEFAULT_MATH;
}

static cudnnStatus_t psyche_cudnn_validate_convolution_backward_filter_config(
    const struct cudnnTensorStruct *x_desc,
    const struct cudnnTensorStruct *dy_desc,
    const struct cudnnConvolutionStruct *conv_desc,
    const struct cudnnFilterStruct *dw_desc,
    cudnnConvolutionBwdFilterAlgo_t algo) {
  int expected_n;
  int expected_c;
  int expected_h;
  int expected_w;
  if (algo != CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      x_desc == 0 ||
      dy_desc == 0 ||
      conv_desc == 0 ||
      dw_desc == 0 ||
      !x_desc->is_set ||
      !dy_desc->is_set ||
      !conv_desc->is_set ||
      !dw_desc->is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (x_desc->data_type != dy_desc->data_type) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      x_desc->data_type != CUDNN_DATA_FLOAT ||
      dy_desc->data_type != CUDNN_DATA_FLOAT ||
      dw_desc->data_type != CUDNN_DATA_FLOAT ||
      conv_desc->compute_type != CUDNN_DATA_FLOAT) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      x_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      dy_desc->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(x_desc) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(dy_desc) ||
      !psyche_cudnn_filter_is_contiguous_kcrs(dw_desc)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (conv_desc->group_count <= 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  /*
   * Backward-filter recovers the gradient of the forward filter.  Treat dwDesc
   * as the forward w descriptor and require dyDesc to match the forward y shape.
   */
  if (
      !psyche_cudnn_convolution_output_dims(
          x_desc,
          dw_desc,
          conv_desc,
          &expected_n,
          &expected_c,
          &expected_h,
          &expected_w)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      dy_desc->dim[0] != expected_n ||
      dy_desc->dim[1] != expected_c ||
      dy_desc->dim[2] != expected_h ||
      dy_desc->dim[3] != expected_w) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  return CUDNN_STATUS_SUCCESS;
}

static void psyche_cudnn_fill_convolution_backward_filter_perf(
    cudnnConvolutionBwdFilterAlgoPerf_t *perf) {
  memset(perf, 0, sizeof(*perf));
  perf->algo = CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1;
  perf->status = CUDNN_STATUS_SUCCESS;
  perf->time = 0.0f;
  perf->memory = 0;
  perf->determinism = CUDNN_DETERMINISTIC;
  perf->mathType = CUDNN_DEFAULT_MATH;
}

static cudnnStatus_t psyche_cudnn_snapshot_convolution_forward_config(
    cudnnHandle_t handle,
    const cudnnTensorDescriptor_t xDesc,
    const cudnnFilterDescriptor_t wDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t yDesc,
    struct cudnnTensorStruct *x_snapshot,
    struct cudnnFilterStruct *w_snapshot,
    struct cudnnConvolutionStruct *conv_snapshot,
    struct cudnnTensorStruct *y_snapshot) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *x_descriptor;
  struct cudnnTensorStruct *y_descriptor;
  struct cudnnFilterStruct *w_descriptor;
  struct cudnnConvolutionStruct *conv_descriptor;
  if (
      handle == 0 ||
      xDesc == 0 ||
      wDesc == 0 ||
      convDesc == 0 ||
      yDesc == 0 ||
      x_snapshot == 0 ||
      w_snapshot == 0 ||
      conv_snapshot == 0 ||
      y_snapshot == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  w_descriptor = psyche_cudnn_find_filter_locked((cudnnFilterDescriptor_t)wDesc);
  conv_descriptor = psyche_cudnn_find_convolution_locked((cudnnConvolutionDescriptor_t)convDesc);
  if (
      context != 0 &&
      x_descriptor != 0 &&
      y_descriptor != 0 &&
      w_descriptor != 0 &&
      conv_descriptor != 0) {
    *x_snapshot = *x_descriptor;
    *y_snapshot = *y_descriptor;
    *w_snapshot = *w_descriptor;
    *conv_snapshot = *conv_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (x_descriptor == 0 || y_descriptor == 0 || w_descriptor == 0 || conv_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  return CUDNN_STATUS_SUCCESS;
}

static cudnnStatus_t psyche_cudnn_snapshot_convolution_backward_data_config(
    cudnnHandle_t handle,
    const cudnnFilterDescriptor_t wDesc,
    const cudnnTensorDescriptor_t dyDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t dxDesc,
    struct cudnnFilterStruct *w_snapshot,
    struct cudnnTensorStruct *dy_snapshot,
    struct cudnnConvolutionStruct *conv_snapshot,
    struct cudnnTensorStruct *dx_snapshot) {
  return psyche_cudnn_snapshot_convolution_forward_config(
      handle,
      dxDesc,
      wDesc,
      convDesc,
      dyDesc,
      dx_snapshot,
      w_snapshot,
      conv_snapshot,
      dy_snapshot);
}

static cudnnStatus_t psyche_cudnn_snapshot_convolution_backward_filter_config(
    cudnnHandle_t handle,
    const cudnnTensorDescriptor_t xDesc,
    const cudnnTensorDescriptor_t dyDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnFilterDescriptor_t dwDesc,
    struct cudnnTensorStruct *x_snapshot,
    struct cudnnTensorStruct *dy_snapshot,
    struct cudnnConvolutionStruct *conv_snapshot,
    struct cudnnFilterStruct *dw_snapshot) {
  return psyche_cudnn_snapshot_convolution_forward_config(
      handle,
      xDesc,
      dwDesc,
      convDesc,
      dyDesc,
      x_snapshot,
      dw_snapshot,
      conv_snapshot,
      dy_snapshot);
}

static int psyche_cudnn_ranges_overlap(
    const void *a,
    const void *b,
    size_t bytes) {
  uintptr_t a_start;
  uintptr_t b_start;
  uintptr_t a_end;
  uintptr_t b_end;
  if (bytes == 0 || a == 0 || b == 0) {
    return 0;
  }
  a_start = (uintptr_t)a;
  b_start = (uintptr_t)b;
  if (a_start > UINTPTR_MAX - bytes || b_start > UINTPTR_MAX - bytes) {
    return 1;
  }
  a_end = a_start + bytes;
  b_end = b_start + bytes;
  return a_start < b_end && b_start < a_end;
}

static int psyche_cudnn_ranges_overlap_sized(
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

static float psyche_cudnn_cpu_activation_value(
    float value,
    cudnnActivationMode_t mode,
    cudnnNanPropagation_t nan_opt) {
  if (isnan(value) && nan_opt == CUDNN_NOT_PROPAGATE_NAN) {
    return 0.0f;
  }
  switch (mode) {
  case CUDNN_ACTIVATION_SIGMOID:
    return 1.0f / (1.0f + expf(-value));
  case CUDNN_ACTIVATION_RELU:
    if (isnan(value)) {
      return value;
    }
    return value > 0.0f ? value : 0.0f;
  case CUDNN_ACTIVATION_TANH:
    return tanhf(value);
  default:
    return value;
  }
}

static void psyche_cudnn_cpu_activation_forward(
    const float *x,
    const float *y_in,
    float *y_out,
    size_t n,
    float alpha,
    float beta,
    cudnnActivationMode_t mode,
    cudnnNanPropagation_t nan_opt) {
  size_t index;
  for (index = 0; index < n; index++) {
    float activated = psyche_cudnn_cpu_activation_value(x[index], mode, nan_opt);
    float prior = beta != 0.0f ? y_in[index] : 0.0f;
    y_out[index] = alpha * activated + beta * prior;
  }
}

static float psyche_cudnn_cpu_activation_derivative(
    float value,
    cudnnActivationMode_t mode,
    cudnnNanPropagation_t nan_opt) {
  if (isnan(value)) {
    if (nan_opt == CUDNN_NOT_PROPAGATE_NAN) {
      value = 0.0f;
    } else {
      return value;
    }
  }
  switch (mode) {
  case CUDNN_ACTIVATION_SIGMOID: {
    float sigmoid = 1.0f / (1.0f + expf(-value));
    return sigmoid * (1.0f - sigmoid);
  }
  case CUDNN_ACTIVATION_RELU:
    return value > 0.0f ? 1.0f : 0.0f;
  case CUDNN_ACTIVATION_TANH: {
    float tanh_value = tanhf(value);
    return 1.0f - tanh_value * tanh_value;
  }
  case CUDNN_ACTIVATION_IDENTITY:
    return 1.0f;
  default:
    return 0.0f;
  }
}

static void psyche_cudnn_cpu_activation_backward(
    const float *x,
    const float *dy,
    const float *dx_in,
    float *dx_out,
    size_t n,
    float alpha,
    float beta,
    cudnnActivationMode_t mode,
    cudnnNanPropagation_t nan_opt) {
  size_t index;
  for (index = 0; index < n; index++) {
    float derivative = psyche_cudnn_cpu_activation_derivative(x[index], mode, nan_opt);
    float result = dy[index] * derivative;
    float prior = beta != 0.0f ? dx_in[index] : 0.0f;
    dx_out[index] = alpha * result + beta * prior;
  }
}

static void psyche_cudnn_cpu_transform_tensor(
    const float *x,
    const float *y_in,
    float *y_out,
    size_t n,
    float alpha,
    float beta) {
  size_t index;
  for (index = 0; index < n; index++) {
    float prior = beta != 0.0f ? y_in[index] : 0.0f;
    y_out[index] = alpha * x[index] + beta * prior;
  }
}

static void psyche_cudnn_cpu_add_tensor(
    const float *a,
    const float *c_in,
    float *c_out,
    const struct cudnnTensorStruct *a_desc,
    const struct cudnnTensorStruct *c_desc,
    float alpha,
    float beta) {
  size_t batch;
  size_t channel;
  size_t row;
  size_t column;
  size_t c_index;
  size_t a_index;
  size_t a_batch;
  size_t a_channel;
  size_t a_row;
  size_t a_column;
  size_t c_n = (size_t)c_desc->dim[0];
  size_t c_c = (size_t)c_desc->dim[1];
  size_t c_h = (size_t)c_desc->dim[2];
  size_t c_w = (size_t)c_desc->dim[3];
  size_t a_c = (size_t)a_desc->dim[1];
  size_t a_h = (size_t)a_desc->dim[2];
  size_t a_w = (size_t)a_desc->dim[3];
  for (batch = 0; batch < c_n; batch++) {
    a_batch = a_desc->dim[0] == 1 ? 0 : batch;
    for (channel = 0; channel < c_c; channel++) {
      a_channel = a_desc->dim[1] == 1 ? 0 : channel;
      for (row = 0; row < c_h; row++) {
        a_row = a_desc->dim[2] == 1 ? 0 : row;
        for (column = 0; column < c_w; column++) {
          a_column = a_desc->dim[3] == 1 ? 0 : column;
          c_index = ((batch * c_c + channel) * c_h + row) * c_w + column;
          a_index = ((a_batch * a_c + a_channel) * a_h + a_row) * a_w + a_column;
          c_out[c_index] = alpha * a[a_index] + beta * (beta != 0.0f ? c_in[c_index] : 0.0f);
        }
      }
    }
  }
}

static void psyche_cudnn_cpu_convolution_forward(
    const float *x,
    const float *w,
    const float *y_in,
    float *y_out,
    const struct cudnnTensorStruct *x_desc,
    const struct cudnnFilterStruct *w_desc,
    const struct cudnnTensorStruct *y_desc,
    const struct cudnnConvolutionStruct *conv_desc,
    float alpha,
    float beta) {
  int n;
  int k;
  int oh;
  int ow;
  int c;
  int r;
  int s;
  int c_local;
  int group_count = conv_desc->group_count;
  int batches = y_desc->dim[0];
  int out_channels = y_desc->dim[1];
  int out_h = y_desc->dim[2];
  int out_w = y_desc->dim[3];
  int in_channels = x_desc->dim[1];
  int in_channels_per_group = w_desc->c;
  int out_channels_per_group = out_channels / group_count;
  int in_h = x_desc->dim[2];
  int in_w = x_desc->dim[3];
  int filter_h = w_desc->h;
  int filter_w = w_desc->w;
  for (n = 0; n < batches; n++) {
    for (k = 0; k < out_channels; k++) {
      int group = k / out_channels_per_group;
      int input_channel_base = group * in_channels_per_group;
      for (oh = 0; oh < out_h; oh++) {
        for (ow = 0; ow < out_w; ow++) {
          float sum = 0.0f;
          size_t y_index =
              (((size_t)n * (size_t)out_channels + (size_t)k) * (size_t)out_h + (size_t)oh) *
              (size_t)out_w + (size_t)ow;
          for (c_local = 0; c_local < in_channels_per_group; c_local++) {
            c = input_channel_base + c_local;
            for (r = 0; r < filter_h; r++) {
              int ih = oh * conv_desc->stride_h - conv_desc->pad_h + r * conv_desc->dilation_h;
              int filter_r = conv_desc->mode == CUDNN_CONVOLUTION ? filter_h - 1 - r : r;
              if (ih < 0 || ih >= in_h) {
                continue;
              }
              for (s = 0; s < filter_w; s++) {
                int iw = ow * conv_desc->stride_w - conv_desc->pad_w + s * conv_desc->dilation_w;
                int filter_s = conv_desc->mode == CUDNN_CONVOLUTION ? filter_w - 1 - s : s;
                size_t x_index;
                size_t w_index;
                if (iw < 0 || iw >= in_w) {
                  continue;
                }
                x_index =
                    (((size_t)n * (size_t)in_channels + (size_t)c) * (size_t)in_h + (size_t)ih) *
                    (size_t)in_w + (size_t)iw;
                w_index =
                    (((size_t)k * (size_t)in_channels_per_group + (size_t)c_local) * (size_t)filter_h +
                     (size_t)filter_r) *
                    (size_t)filter_w + (size_t)filter_s;
                sum += x[x_index] * w[w_index];
              }
            }
          }
          y_out[y_index] = alpha * sum + beta * (beta != 0.0f ? y_in[y_index] : 0.0f);
        }
      }
    }
  }
}

static void psyche_cudnn_cpu_convolution_bias_activation_forward(
    const float *x,
    const float *w,
    const float *z,
    const float *bias,
    float *y,
    const struct cudnnTensorStruct *x_desc,
    const struct cudnnFilterStruct *w_desc,
    const struct cudnnTensorStruct *y_desc,
    const struct cudnnConvolutionStruct *conv_desc,
    float alpha1,
    float alpha2,
    cudnnActivationMode_t activation_mode) {
  int n;
  int k;
  int oh;
  int ow;
  int c;
  int r;
  int s;
  int c_local;
  int group_count = conv_desc->group_count;
  int batches = y_desc->dim[0];
  int out_channels = y_desc->dim[1];
  int out_h = y_desc->dim[2];
  int out_w = y_desc->dim[3];
  int in_channels = x_desc->dim[1];
  int in_channels_per_group = w_desc->c;
  int out_channels_per_group = out_channels / group_count;
  int in_h = x_desc->dim[2];
  int in_w = x_desc->dim[3];
  int filter_h = w_desc->h;
  int filter_w = w_desc->w;
  for (n = 0; n < batches; n++) {
    for (k = 0; k < out_channels; k++) {
      int group = k / out_channels_per_group;
      int input_channel_base = group * in_channels_per_group;
      for (oh = 0; oh < out_h; oh++) {
        for (ow = 0; ow < out_w; ow++) {
          float sum = 0.0f;
          float fused;
          size_t y_index =
              (((size_t)n * (size_t)out_channels + (size_t)k) * (size_t)out_h + (size_t)oh) *
              (size_t)out_w + (size_t)ow;
          for (c_local = 0; c_local < in_channels_per_group; c_local++) {
            c = input_channel_base + c_local;
            for (r = 0; r < filter_h; r++) {
              int ih = oh * conv_desc->stride_h - conv_desc->pad_h + r * conv_desc->dilation_h;
              int filter_r = conv_desc->mode == CUDNN_CONVOLUTION ? filter_h - 1 - r : r;
              if (ih < 0 || ih >= in_h) {
                continue;
              }
              for (s = 0; s < filter_w; s++) {
                int iw = ow * conv_desc->stride_w - conv_desc->pad_w + s * conv_desc->dilation_w;
                int filter_s = conv_desc->mode == CUDNN_CONVOLUTION ? filter_w - 1 - s : s;
                size_t x_index;
                size_t w_index;
                if (iw < 0 || iw >= in_w) {
                  continue;
                }
                x_index =
                    (((size_t)n * (size_t)in_channels + (size_t)c) * (size_t)in_h + (size_t)ih) *
                    (size_t)in_w + (size_t)iw;
                w_index =
                    (((size_t)k * (size_t)in_channels_per_group + (size_t)c_local) * (size_t)filter_h +
                     (size_t)filter_r) *
                    (size_t)filter_w + (size_t)filter_s;
                sum += x[x_index] * w[w_index];
              }
            }
          }
          /* Exact z == y is safe here because each contiguous NCHW output index is read before its own write. */
          fused = alpha1 * sum + alpha2 * (alpha2 != 0.0f ? z[y_index] : 0.0f) + bias[k];
          y[y_index] = activation_mode == CUDNN_ACTIVATION_IDENTITY
              ? fused
              : psyche_cudnn_cpu_activation_value(fused, activation_mode, CUDNN_NOT_PROPAGATE_NAN);
        }
      }
    }
  }
}

static void psyche_cudnn_cpu_convolution_backward_data(
    const float *w,
    const float *dy,
    const float *dx_in,
    float *dx_out,
    const struct cudnnFilterStruct *w_desc,
    const struct cudnnTensorStruct *dy_desc,
    const struct cudnnTensorStruct *dx_desc,
    const struct cudnnConvolutionStruct *conv_desc,
    float alpha,
    float beta) {
  int n;
  int c;
  int ih;
  int iw;
  int k_local;
  int r;
  int s;
  int batches = dx_desc->dim[0];
  int in_channels = dx_desc->dim[1];
  int in_h = dx_desc->dim[2];
  int in_w = dx_desc->dim[3];
  int out_channels = dy_desc->dim[1];
  int out_h = dy_desc->dim[2];
  int out_w = dy_desc->dim[3];
  int group_count = conv_desc->group_count;
  int in_channels_per_group = w_desc->c;
  int out_channels_per_group = out_channels / group_count;
  int filter_h = w_desc->h;
  int filter_w = w_desc->w;
  /*
   * cuDNN grouped filters are full KCRS tensors: [K, C/groupCount, R, S].
   * The K index remains global while the C index is local to the selected group.
   */
  for (n = 0; n < batches; n++) {
    for (c = 0; c < in_channels; c++) {
      int group = c / in_channels_per_group;
      int c_local = c % in_channels_per_group;
      int output_channel_base = group * out_channels_per_group;
      for (ih = 0; ih < in_h; ih++) {
        for (iw = 0; iw < in_w; iw++) {
          float sum = 0.0f;
          size_t dx_index =
              (((size_t)n * (size_t)in_channels + (size_t)c) * (size_t)in_h + (size_t)ih) *
              (size_t)in_w + (size_t)iw;
          for (k_local = 0; k_local < out_channels_per_group; k_local++) {
            int k = output_channel_base + k_local;
            for (r = 0; r < filter_h; r++) {
              long long oh_numerator =
                  (long long)ih +
                  (long long)conv_desc->pad_h -
                  (long long)r * (long long)conv_desc->dilation_h;
              int oh;
              int filter_r = conv_desc->mode == CUDNN_CONVOLUTION ? filter_h - 1 - r : r;
              if (oh_numerator % (long long)conv_desc->stride_h != 0) {
                continue;
              }
              oh = (int)(oh_numerator / (long long)conv_desc->stride_h);
              if (oh < 0 || oh >= out_h) {
                continue;
              }
              for (s = 0; s < filter_w; s++) {
                long long ow_numerator =
                    (long long)iw +
                    (long long)conv_desc->pad_w -
                    (long long)s * (long long)conv_desc->dilation_w;
                int ow;
                int filter_s = conv_desc->mode == CUDNN_CONVOLUTION ? filter_w - 1 - s : s;
                size_t dy_index;
                size_t w_index;
                if (ow_numerator % (long long)conv_desc->stride_w != 0) {
                  continue;
                }
                ow = (int)(ow_numerator / (long long)conv_desc->stride_w);
                if (ow < 0 || ow >= out_w) {
                  continue;
                }
                dy_index =
                    (((size_t)n * (size_t)out_channels + (size_t)k) * (size_t)out_h + (size_t)oh) *
                    (size_t)out_w + (size_t)ow;
                w_index =
                    (((size_t)k * (size_t)in_channels_per_group + (size_t)c_local) * (size_t)filter_h +
                     (size_t)filter_r) *
                    (size_t)filter_w + (size_t)filter_s;
                sum += dy[dy_index] * w[w_index];
              }
            }
          }
          dx_out[dx_index] = alpha * sum + beta * (beta != 0.0f ? dx_in[dx_index] : 0.0f);
        }
      }
    }
  }
}

static void psyche_cudnn_cpu_convolution_backward_filter(
    const float *x,
    const float *dy,
    const float *dw_in,
    float *dw_out,
    const struct cudnnTensorStruct *x_desc,
    const struct cudnnTensorStruct *dy_desc,
    const struct cudnnFilterStruct *dw_desc,
    const struct cudnnConvolutionStruct *conv_desc,
    float alpha,
    float beta) {
  int k;
  int c_local;
  int r_phys;
  int s_phys;
  int n;
  int oh;
  int ow;
  int batches = x_desc->dim[0];
  int in_channels = x_desc->dim[1];
  int in_h = x_desc->dim[2];
  int in_w = x_desc->dim[3];
  int out_channels = dy_desc->dim[1];
  int out_h = dy_desc->dim[2];
  int out_w = dy_desc->dim[3];
  int group_count = conv_desc->group_count;
  int in_channels_per_group = dw_desc->c;
  int out_channels_per_group = out_channels / group_count;
  int filter_h = dw_desc->h;
  int filter_w = dw_desc->w;
  /*
   * dw is physically KCRS.  For true convolution, the physical dw slot stays
   * [k, c_local, r, s]; the forward tap that reaches x is flipped.
   */
  for (k = 0; k < out_channels; k++) {
    int group = k / out_channels_per_group;
    int input_channel_base = group * in_channels_per_group;
    for (c_local = 0; c_local < in_channels_per_group; c_local++) {
      int c = input_channel_base + c_local;
      for (r_phys = 0; r_phys < filter_h; r_phys++) {
        int r_tap = conv_desc->mode == CUDNN_CONVOLUTION ? filter_h - 1 - r_phys : r_phys;
        for (s_phys = 0; s_phys < filter_w; s_phys++) {
          int s_tap = conv_desc->mode == CUDNN_CONVOLUTION ? filter_w - 1 - s_phys : s_phys;
          float sum = 0.0f;
          size_t dw_index =
              (((size_t)k * (size_t)in_channels_per_group + (size_t)c_local) * (size_t)filter_h +
               (size_t)r_phys) *
              (size_t)filter_w + (size_t)s_phys;
          for (n = 0; n < batches; n++) {
            for (oh = 0; oh < out_h; oh++) {
              int ih = oh * conv_desc->stride_h - conv_desc->pad_h + r_tap * conv_desc->dilation_h;
              if (ih < 0 || ih >= in_h) {
                continue;
              }
              for (ow = 0; ow < out_w; ow++) {
                int iw = ow * conv_desc->stride_w - conv_desc->pad_w + s_tap * conv_desc->dilation_w;
                size_t x_index;
                size_t dy_index;
                if (iw < 0 || iw >= in_w) {
                  continue;
                }
                x_index =
                    (((size_t)n * (size_t)in_channels + (size_t)c) * (size_t)in_h + (size_t)ih) *
                    (size_t)in_w + (size_t)iw;
                dy_index =
                    (((size_t)n * (size_t)out_channels + (size_t)k) * (size_t)out_h + (size_t)oh) *
                    (size_t)out_w + (size_t)ow;
                sum += x[x_index] * dy[dy_index];
              }
            }
          }
          dw_out[dw_index] = alpha * sum + beta * (beta != 0.0f ? dw_in[dw_index] : 0.0f);
        }
      }
    }
  }
}

static int psyche_cudnn_bn_param_count(
    cudnnBatchNormMode_t mode,
    const struct cudnnTensorStruct *x_desc,
    const struct cudnnTensorStruct *bn_desc,
    size_t *param_count) {
  size_t c;
  size_t h;
  size_t w;
  if (x_desc == 0 || bn_desc == 0 || param_count == 0) {
    return 0;
  }
  c = (size_t)x_desc->dim[1];
  h = (size_t)x_desc->dim[2];
  w = (size_t)x_desc->dim[3];
  if (mode == CUDNN_BATCHNORM_SPATIAL) {
    if (
        bn_desc->dim[0] != 1 ||
        bn_desc->dim[1] != x_desc->dim[1] ||
        bn_desc->dim[2] != 1 ||
        bn_desc->dim[3] != 1) {
      return 0;
    }
    *param_count = c;
    return 1;
  }
  if (mode == CUDNN_BATCHNORM_PER_ACTIVATION) {
    if (
        bn_desc->dim[0] != 1 ||
        bn_desc->dim[1] != x_desc->dim[1] ||
        bn_desc->dim[2] != x_desc->dim[2] ||
        bn_desc->dim[3] != x_desc->dim[3]) {
      return 0;
    }
    if (c > SIZE_MAX / h || c * h > SIZE_MAX / w) {
      return 0;
    }
    *param_count = c * h * w;
    return 1;
  }
  return 0;
}

static void psyche_cudnn_cpu_batchnorm_inference(
    const float *x,
    const float *y_in,
    float *y_out,
    const float *scale,
    const float *bias,
    const float *mean,
    const float *variance,
    const struct cudnnTensorStruct *x_desc,
    cudnnBatchNormMode_t mode,
    float alpha,
    float beta,
    float epsilon) {
  size_t batch;
  size_t channel;
  size_t row;
  size_t column;
  size_t index;
  size_t param_index;
  size_t batches = (size_t)x_desc->dim[0];
  size_t channels = (size_t)x_desc->dim[1];
  size_t height = (size_t)x_desc->dim[2];
  size_t width = (size_t)x_desc->dim[3];
  /* Exact x == y is safe here because inference is element-local and reads x/y before writing y. */
  for (batch = 0; batch < batches; batch++) {
    for (channel = 0; channel < channels; channel++) {
      for (row = 0; row < height; row++) {
        for (column = 0; column < width; column++) {
          float normalized;
          float prior;
          index = ((batch * channels + channel) * height + row) * width + column;
          if (mode == CUDNN_BATCHNORM_PER_ACTIVATION) {
            param_index = (channel * height + row) * width + column;
          } else {
            param_index = channel;
          }
          normalized =
              bias[param_index] +
              scale[param_index] *
                  (x[index] - mean[param_index]) /
                  sqrtf(epsilon + variance[param_index]);
          prior = beta != 0.0f ? y_in[index] : 0.0f;
          y_out[index] = alpha * normalized + beta * prior;
        }
      }
    }
  }
}

static int psyche_cudnn_pooling_output_dim(
    int input_dim,
    int padding,
    int window_dim,
    int stride,
    int *output_dim) {
  long long numerator;
  long long output;
  if (
      output_dim == 0 ||
      input_dim <= 0 ||
      padding < 0 ||
      window_dim <= 0 ||
      stride <= 0) {
    return 0;
  }
  numerator = (long long)input_dim + 2LL * (long long)padding - (long long)window_dim;
  if (numerator < 0) {
    return 0;
  }
  output = 1LL + numerator / (long long)stride;
  if (output <= 0 || output > (long long)INT_MAX) {
    return 0;
  }
  *output_dim = (int)output;
  return 1;
}

static float psyche_cudnn_cpu_pooling_value(
    const float *x,
    int n,
    int c,
    int oh,
    int ow,
    int channels,
    int in_h,
    int in_w,
    int window_h,
    int window_w,
    int pad_h,
    int pad_w,
    int stride_h,
    int stride_w,
    cudnnPoolingMode_t mode,
    cudnnNanPropagation_t nan_opt) {
  int kh;
  int kw;
  int h_start = oh * stride_h - pad_h;
  int w_start = ow * stride_w - pad_w;
  float best = -FLT_MAX;
  float sum = 0.0f;
  int valid_count = 0;
  int denominator = window_h * window_w;
  int is_average =
      mode == CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING ||
      mode == CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING;
  for (kh = 0; kh < window_h; kh++) {
    int ih = h_start + kh;
    if (ih < 0 || ih >= in_h) {
      continue;
    }
    for (kw = 0; kw < window_w; kw++) {
      int iw = w_start + kw;
      float value;
      size_t index;
      if (iw < 0 || iw >= in_w) {
        continue;
      }
      valid_count++;
      index = (((size_t)n * (size_t)channels + (size_t)c) * (size_t)in_h + (size_t)ih) *
          (size_t)in_w + (size_t)iw;
      value = x[index];
      if (isnan(value)) {
        if (nan_opt == CUDNN_PROPAGATE_NAN) {
          return value;
        }
        /* NOT_PROPAGATE_NAN drops NaN from the sum; padding alone controls the denominator. */
        continue;
      }
      if (is_average) {
        sum += value;
        continue;
      }
      if (value > best) {
        best = value;
      }
    }
  }
  if (mode == CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING) {
    return denominator > 0 ? sum / (float)denominator : 0.0f;
  }
  if (mode == CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING) {
    return valid_count > 0 ? sum / (float)valid_count : 0.0f;
  }
  return best;
}

static void psyche_cudnn_cpu_pooling_forward(
    const float *x,
    const float *y_in,
    float *y_out,
    float alpha,
    float beta,
    const struct cudnnTensorStruct *x_desc,
    const struct cudnnTensorStruct *y_desc,
    const struct cudnnPoolingStruct *pool_desc) {
  int n;
  int c;
  int oh;
  int ow;
  int batches = y_desc->dim[0];
  int channels = y_desc->dim[1];
  int out_h = y_desc->dim[2];
  int out_w = y_desc->dim[3];
  int in_h = x_desc->dim[2];
  int in_w = x_desc->dim[3];
  for (n = 0; n < batches; n++) {
    for (c = 0; c < channels; c++) {
      for (oh = 0; oh < out_h; oh++) {
        for (ow = 0; ow < out_w; ow++) {
          size_t out_index =
              (((size_t)n * (size_t)channels + (size_t)c) * (size_t)out_h + (size_t)oh) *
              (size_t)out_w + (size_t)ow;
          float pooled = psyche_cudnn_cpu_pooling_value(
              x,
              n,
              c,
              oh,
              ow,
              channels,
              in_h,
              in_w,
              pool_desc->window_h,
              pool_desc->window_w,
              pool_desc->pad_h,
              pool_desc->pad_w,
              pool_desc->stride_h,
              pool_desc->stride_w,
              pool_desc->mode,
              pool_desc->nan_opt);
          float prior = beta != 0.0f ? y_in[out_index] : 0.0f;
          y_out[out_index] = alpha * pooled + beta * prior;
        }
      }
    }
  }
}

static int psyche_cudnn_pooling_mode_is_average(cudnnPoolingMode_t mode) {
  return
      mode == CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING ||
      mode == CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING;
}

static int psyche_cudnn_cpu_pooling_window_selects_input(
    const float *x,
    int n,
    int c,
    int target_h,
    int target_w,
    int oh,
    int ow,
    int channels,
    int in_h,
    int in_w,
    int window_h,
    int window_w,
    int pad_h,
    int pad_w,
    int stride_h,
    int stride_w,
    cudnnNanPropagation_t nan_opt) {
  int kh;
  int kw;
  int h_start = oh * stride_h - pad_h;
  int w_start = ow * stride_w - pad_w;
  int found = 0;
  int best_h = -1;
  int best_w = -1;
  float best = -FLT_MAX;
  for (kh = 0; kh < window_h; kh++) {
    int ih = h_start + kh;
    if (ih < 0 || ih >= in_h) {
      continue;
    }
    for (kw = 0; kw < window_w; kw++) {
      int iw = w_start + kw;
      size_t index;
      float value;
      if (iw < 0 || iw >= in_w) {
        continue;
      }
      index = (((size_t)n * (size_t)channels + (size_t)c) * (size_t)in_h + (size_t)ih) *
          (size_t)in_w + (size_t)iw;
      value = x[index];
      if (isnan(value)) {
        if (nan_opt == CUDNN_PROPAGATE_NAN) {
          return ih == target_h && iw == target_w;
        }
        continue;
      }
      if (!found || value > best) {
        found = 1;
        best = value;
        best_h = ih;
        best_w = iw;
      }
    }
  }
  return found && best_h == target_h && best_w == target_w;
}

static int psyche_cudnn_cpu_pooling_valid_count(
    int oh,
    int ow,
    int in_h,
    int in_w,
    int window_h,
    int window_w,
    int pad_h,
    int pad_w,
    int stride_h,
    int stride_w) {
  int kh;
  int kw;
  int count = 0;
  int h_start = oh * stride_h - pad_h;
  int w_start = ow * stride_w - pad_w;
  for (kh = 0; kh < window_h; kh++) {
    int ih = h_start + kh;
    if (ih < 0 || ih >= in_h) {
      continue;
    }
    for (kw = 0; kw < window_w; kw++) {
      int iw = w_start + kw;
      if (iw >= 0 && iw < in_w) {
        count++;
      }
    }
  }
  return count;
}

static void psyche_cudnn_cpu_pooling_backward(
    const float *x,
    const float *dy,
    const float *dx_in,
    float *dx_out,
    float alpha,
    float beta,
    const struct cudnnTensorStruct *dy_desc,
    const struct cudnnTensorStruct *dx_desc,
    const struct cudnnPoolingStruct *pool_desc) {
  int n;
  int c;
  int ih;
  int iw;
  int batches = dx_desc->dim[0];
  int channels = dx_desc->dim[1];
  int in_h = dx_desc->dim[2];
  int in_w = dx_desc->dim[3];
  int out_h = dy_desc->dim[2];
  int out_w = dy_desc->dim[3];
  int is_average = psyche_cudnn_pooling_mode_is_average(pool_desc->mode);
  for (n = 0; n < batches; n++) {
    for (c = 0; c < channels; c++) {
      for (ih = 0; ih < in_h; ih++) {
        for (iw = 0; iw < in_w; iw++) {
          int oh;
          int ow;
          size_t in_index =
              (((size_t)n * (size_t)channels + (size_t)c) * (size_t)in_h + (size_t)ih) *
              (size_t)in_w + (size_t)iw;
          float grad = 0.0f;
          for (oh = 0; oh < out_h; oh++) {
            int h_start = oh * pool_desc->stride_h - pool_desc->pad_h;
            if (ih < h_start || ih >= h_start + pool_desc->window_h) {
              continue;
            }
            for (ow = 0; ow < out_w; ow++) {
              int denominator;
              int w_start = ow * pool_desc->stride_w - pool_desc->pad_w;
              size_t out_index;
              if (iw < w_start || iw >= w_start + pool_desc->window_w) {
                continue;
              }
              out_index =
                  (((size_t)n * (size_t)channels + (size_t)c) * (size_t)out_h + (size_t)oh) *
                  (size_t)out_w + (size_t)ow;
              if (is_average) {
                denominator = pool_desc->window_h * pool_desc->window_w;
                if (pool_desc->mode == CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING) {
                  denominator = psyche_cudnn_cpu_pooling_valid_count(
                      oh,
                      ow,
                      in_h,
                      in_w,
                      pool_desc->window_h,
                      pool_desc->window_w,
                      pool_desc->pad_h,
                      pool_desc->pad_w,
                      pool_desc->stride_h,
                      pool_desc->stride_w);
                }
                if (denominator > 0) {
                  grad += dy[out_index] / (float)denominator;
                }
              } else if (psyche_cudnn_cpu_pooling_window_selects_input(
                  x,
                  n,
                  c,
                  ih,
                  iw,
                  oh,
                  ow,
                  channels,
                  in_h,
                  in_w,
                  pool_desc->window_h,
                  pool_desc->window_w,
                  pool_desc->pad_h,
                  pool_desc->pad_w,
                  pool_desc->stride_h,
                  pool_desc->stride_w,
                  pool_desc->nan_opt)) {
                grad += dy[out_index];
              }
            }
          }
          dx_out[in_index] = alpha * grad + beta * (beta != 0.0f ? dx_in[in_index] : 0.0f);
        }
      }
    }
  }
}

static int psyche_cudnn_softmax_shape(
    const struct cudnnTensorStruct *descriptor,
    cudnnSoftmaxMode_t mode,
    size_t *vector_count,
    size_t *vector_len) {
  size_t n;
  size_t c;
  size_t h;
  size_t w;
  size_t spatial;
  if (descriptor == 0 || vector_count == 0 || vector_len == 0) {
    return 0;
  }
  if (descriptor->nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return 0;
  }
  n = (size_t)descriptor->dim[0];
  c = (size_t)descriptor->dim[1];
  h = (size_t)descriptor->dim[2];
  w = (size_t)descriptor->dim[3];
  if (n == 0 || c == 0 || h == 0 || w == 0 || h > SIZE_MAX / w) {
    return 0;
  }
  spatial = h * w;
  switch (mode) {
  case CUDNN_SOFTMAX_MODE_INSTANCE:
    if (c > SIZE_MAX / spatial) {
      return 0;
    }
    *vector_count = n;
    *vector_len = c * spatial;
    return *vector_len != 0;
  case CUDNN_SOFTMAX_MODE_CHANNEL:
    if (n > SIZE_MAX / spatial) {
      return 0;
    }
    *vector_count = n * spatial;
    *vector_len = c;
    return *vector_count != 0 && *vector_len != 0;
  default:
    return 0;
  }
}

static size_t psyche_cudnn_softmax_offset(
    const struct cudnnTensorStruct *descriptor,
    cudnnSoftmaxMode_t mode,
    size_t vector,
    size_t lane) {
  size_t n = (size_t)descriptor->dim[0];
  size_t c = (size_t)descriptor->dim[1];
  size_t h = (size_t)descriptor->dim[2];
  size_t w = (size_t)descriptor->dim[3];
  (void)n;
  if (mode == CUDNN_SOFTMAX_MODE_INSTANCE) {
    return vector * c * h * w + lane;
  }
  {
    size_t ow = vector % w;
    size_t tmp = vector / w;
    size_t oh = tmp % h;
    size_t batch = tmp / h;
    return ((batch * c + lane) * h + oh) * w + ow;
  }
}

static void psyche_cudnn_cpu_softmax_forward(
    const float *x,
    const float *y_in,
    float *y_out,
    float alpha,
    float beta,
    const struct cudnnTensorStruct *x_desc,
    cudnnSoftmaxAlgorithm_t algorithm,
    cudnnSoftmaxMode_t mode,
    size_t vector_count,
    size_t vector_len) {
  size_t vector;
  for (vector = 0; vector < vector_count; vector++) {
    size_t lane;
    int has_nan = 0;
    size_t positive_inf_count = 0;
    float max_value = -INFINITY;
    float sum = 0.0f;
    for (lane = 0; lane < vector_len; lane++) {
      float value = x[psyche_cudnn_softmax_offset(x_desc, mode, vector, lane)];
      if (isnan(value)) {
        has_nan = 1;
        break;
      }
      if (isinf(value) && value > 0.0f) {
        positive_inf_count++;
      } else if (value > max_value) {
        max_value = value;
      }
    }
    if (!has_nan && !(algorithm != CUDNN_SOFTMAX_FAST && positive_inf_count > 0)) {
      for (lane = 0; lane < vector_len; lane++) {
        float value = x[psyche_cudnn_softmax_offset(x_desc, mode, vector, lane)];
        sum += algorithm == CUDNN_SOFTMAX_FAST ? expf(value) : expf(value - max_value);
      }
    }
    for (lane = 0; lane < vector_len; lane++) {
      size_t offset = psyche_cudnn_softmax_offset(x_desc, mode, vector, lane);
      float value = x[offset];
      float result;
      float prior = beta != 0.0f ? y_in[offset] : 0.0f;
      if (has_nan) {
        result = NAN;
      } else if (algorithm != CUDNN_SOFTMAX_FAST && positive_inf_count > 0) {
        if (isinf(value) && value > 0.0f) {
          result = algorithm == CUDNN_SOFTMAX_LOG
              ? -logf((float)positive_inf_count)
              : 1.0f / (float)positive_inf_count;
        } else {
          result = algorithm == CUDNN_SOFTMAX_LOG ? -INFINITY : 0.0f;
        }
      } else if (algorithm == CUDNN_SOFTMAX_LOG) {
        result = value - max_value - logf(sum);
      } else {
        float numerator = algorithm == CUDNN_SOFTMAX_FAST ? expf(value) : expf(value - max_value);
        result = numerator / sum;
      }
      y_out[offset] = alpha * result + beta * prior;
    }
  }
}

static void psyche_cudnn_cpu_softmax_backward(
    const float *y,
    const float *dy,
    const float *dx_in,
    float *dx_out,
    float alpha,
    float beta,
    const struct cudnnTensorStruct *descriptor,
    cudnnSoftmaxAlgorithm_t algorithm,
    cudnnSoftmaxMode_t mode,
    size_t vector_count,
    size_t vector_len) {
  size_t vector;
  for (vector = 0; vector < vector_count; vector++) {
    size_t lane;
    int has_nan = 0;
    float accum = 0.0f;
    for (lane = 0; lane < vector_len; lane++) {
      size_t offset = psyche_cudnn_softmax_offset(descriptor, mode, vector, lane);
      float y_value = y[offset];
      float dy_value = dy[offset];
      if (isnan(y_value) || isnan(dy_value)) {
        has_nan = 1;
        break;
      }
      accum += algorithm == CUDNN_SOFTMAX_LOG ? dy_value : y_value * dy_value;
    }
    if (isnan(accum)) {
      has_nan = 1;
    }
    for (lane = 0; lane < vector_len; lane++) {
      size_t offset = psyche_cudnn_softmax_offset(descriptor, mode, vector, lane);
      float result;
      float prior = beta != 0.0f ? dx_in[offset] : 0.0f;
      if (has_nan) {
        result = NAN;
      } else if (algorithm == CUDNN_SOFTMAX_LOG) {
        result = dy[offset] - expf(y[offset]) * accum;
      } else {
        result = y[offset] * (dy[offset] - accum);
      }
      dx_out[offset] = alpha * result + beta * prior;
    }
  }
}

PSYCHE_CUDA_STUB_API int psyche_cuda_compat_stub_is_stub(void) {
  return 1;
}

PSYCHE_CUDA_STUB_API const char *psyche_cuda_compat_stub_version(void) {
  return "psyche-cudnn-compat-stub/0.2";
}

PSYCHE_CUDA_STUB_API size_t cudnnGetVersion(void) {
  return 0;
}

PSYCHE_CUDA_STUB_API size_t cudnnGetCudartVersion(void) {
  return 0;
}

PSYCHE_CUDA_STUB_API size_t cudnnGetMaxDeviceVersion(void) {
  return 0;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetProperty(
    libraryPropertyType type,
    int *value) {
  if (value == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  switch (type) {
  case MAJOR_VERSION:
  case MINOR_VERSION:
  case PATCH_LEVEL:
    *value = 0;
    return CUDNN_STATUS_SUCCESS;
  default:
    return CUDNN_STATUS_INVALID_VALUE;
  }
}

PSYCHE_CUDA_STUB_API const char *cudnnGetErrorString(cudnnStatus_t status) {
  switch (status) {
  case CUDNN_STATUS_SUCCESS:
    return "CUDNN_STATUS_SUCCESS";
  case CUDNN_STATUS_NOT_INITIALIZED:
    return "CUDNN_STATUS_NOT_INITIALIZED";
  case CUDNN_STATUS_ALLOC_FAILED:
    return "CUDNN_STATUS_ALLOC_FAILED";
  case CUDNN_STATUS_BAD_PARAM:
    return "CUDNN_STATUS_BAD_PARAM";
  case CUDNN_STATUS_INTERNAL_ERROR:
    return "CUDNN_STATUS_INTERNAL_ERROR";
  case CUDNN_STATUS_INVALID_VALUE:
    return "CUDNN_STATUS_INVALID_VALUE";
  case CUDNN_STATUS_ARCH_MISMATCH:
    return "CUDNN_STATUS_ARCH_MISMATCH";
  case CUDNN_STATUS_MAPPING_ERROR:
    return "CUDNN_STATUS_MAPPING_ERROR";
  case CUDNN_STATUS_EXECUTION_FAILED:
    return "CUDNN_STATUS_EXECUTION_FAILED";
  case CUDNN_STATUS_NOT_SUPPORTED:
    return "CUDNN_STATUS_NOT_SUPPORTED";
  case CUDNN_STATUS_LICENSE_ERROR:
    return "CUDNN_STATUS_LICENSE_ERROR";
  case CUDNN_STATUS_RUNTIME_PREREQUISITE_MISSING:
    return "CUDNN_STATUS_RUNTIME_PREREQUISITE_MISSING";
  case CUDNN_STATUS_RUNTIME_IN_PROGRESS:
    return "CUDNN_STATUS_RUNTIME_IN_PROGRESS";
  case CUDNN_STATUS_RUNTIME_FP_OVERFLOW:
    return "CUDNN_STATUS_RUNTIME_FP_OVERFLOW";
  case CUDNN_STATUS_VERSION_MISMATCH:
    return "CUDNN_STATUS_VERSION_MISMATCH";
  default:
    return "CUDNN_UNKNOWN_STATUS";
  }
}

PSYCHE_CUDA_STUB_API void cudnnGetLastErrorString(
    char *message,
    size_t max_size) {
  size_t copy_len;
  if (message == 0 || max_size == 0) {
    return;
  }
  copy_len = strlen(PSYCHE_CUDNN_LAST_ERROR);
  if (copy_len >= max_size) {
    copy_len = max_size - 1;
  }
  memcpy(message, PSYCHE_CUDNN_LAST_ERROR, copy_len);
  message[copy_len] = '\0';
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnCreate(cudnnHandle_t *handle) {
  struct cudnnContext *context;
  if (handle == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *handle = 0;
  if (!psyche_cudnn_simulated_memory_enabled()) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  context = (struct cudnnContext *)calloc(1, sizeof(*context));
  if (context == 0) {
    return CUDNN_STATUS_ALLOC_FAILED;
  }
  context->magic = PSYCHE_CUDNN_CONTEXT_MAGIC;
  pthread_mutex_lock(&psyche_cudnn_mutex);
  context->next = psyche_cudnn_contexts;
  psyche_cudnn_contexts = context;
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  *handle = context;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnDestroy(cudnnHandle_t handle) {
  int removed;
  if (handle == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  removed = psyche_cudnn_remove_context_locked(handle);
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (!removed) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  free(handle);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetStream(
    cudnnHandle_t handle,
    cudaStream_t *streamId) {
  struct cudnnContext *context;
  if (handle == 0 || streamId == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  if (context != 0) {
    *streamId = context->stream;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  return context != 0 ? CUDNN_STATUS_SUCCESS : CUDNN_STATUS_BAD_PARAM;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnSetStream(
    cudnnHandle_t handle,
    cudaStream_t streamId) {
  struct cudnnContext *context;
  if (handle == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  if (context != 0) {
    context->stream = streamId;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  return context != 0 ? CUDNN_STATUS_SUCCESS : CUDNN_STATUS_BAD_PARAM;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnQueryRuntimeError(
    cudnnHandle_t handle,
    cudnnStatus_t *rstatus,
    int mode) {
  struct cudnnContext *context;
  (void)mode;
  if (handle == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (rstatus != 0) {
    *rstatus = CUDNN_STATUS_SUCCESS;
  }
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGraphVersionCheck(void) {
  return CUDNN_STATUS_NOT_SUPPORTED;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnCreateTensorDescriptor(
    cudnnTensorDescriptor_t *tensorDesc) {
  struct cudnnTensorStruct *descriptor;
  if (tensorDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *tensorDesc = 0;
  descriptor = (struct cudnnTensorStruct *)calloc(1, sizeof(*descriptor));
  if (descriptor == 0) {
    return CUDNN_STATUS_ALLOC_FAILED;
  }
  descriptor->magic = PSYCHE_CUDNN_TENSOR_MAGIC;
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor->next = psyche_cudnn_tensors;
  psyche_cudnn_tensors = descriptor;
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  *tensorDesc = descriptor;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnSetTensor4dDescriptor(
    cudnnTensorDescriptor_t tensorDesc,
    cudnnTensorFormat_t format,
    cudnnDataType_t dataType,
    int n,
    int c,
    int h,
    int w) {
  struct cudnnTensorStruct *descriptor;
  size_t count;
  size_t stride_n;
  size_t stride_c;
  size_t stride_h;
  if (tensorDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (format != CUDNN_TENSOR_NCHW) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (dataType != CUDNN_DATA_FLOAT) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (n <= 0 || c <= 0 || h <= 0 || w <= 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  count = (size_t)n;
  if (
      count > SIZE_MAX / (size_t)c ||
      count * (size_t)c > SIZE_MAX / (size_t)h ||
      count * (size_t)c * (size_t)h > SIZE_MAX / (size_t)w) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  count = count * (size_t)c * (size_t)h * (size_t)w;
  stride_h = (size_t)w;
  stride_c = (size_t)h * (size_t)w;
  stride_n = (size_t)c * (size_t)h * (size_t)w;
  if (stride_n > (size_t)INT_MAX || stride_c > (size_t)INT_MAX || stride_h > (size_t)INT_MAX) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor = psyche_cudnn_find_tensor_locked(tensorDesc);
  if (descriptor != 0) {
    descriptor->is_set = 1;
    descriptor->data_type = dataType;
    descriptor->nb_dims = PSYCHE_CUDNN_MAX_TENSOR_DIMS;
    descriptor->dim[0] = n;
    descriptor->dim[1] = c;
    descriptor->dim[2] = h;
    descriptor->dim[3] = w;
    descriptor->stride[3] = 1;
    descriptor->stride[2] = (int)stride_h;
    descriptor->stride[1] = (int)stride_c;
    descriptor->stride[0] = (int)stride_n;
    descriptor->element_count = count;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  return descriptor != 0 ? CUDNN_STATUS_SUCCESS : CUDNN_STATUS_BAD_PARAM;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnDestroyTensorDescriptor(
    cudnnTensorDescriptor_t tensorDesc) {
  int removed;
  if (tensorDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  removed = psyche_cudnn_remove_tensor_locked(tensorDesc);
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (!removed) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  free(tensorDesc);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnCreateFilterDescriptor(
    cudnnFilterDescriptor_t *filterDesc) {
  struct cudnnFilterStruct *descriptor;
  if (filterDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *filterDesc = 0;
  descriptor = (struct cudnnFilterStruct *)calloc(1, sizeof(*descriptor));
  if (descriptor == 0) {
    return CUDNN_STATUS_ALLOC_FAILED;
  }
  descriptor->magic = PSYCHE_CUDNN_FILTER_MAGIC;
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor->next = psyche_cudnn_filters;
  psyche_cudnn_filters = descriptor;
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  *filterDesc = descriptor;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnSetFilter4dDescriptor(
    cudnnFilterDescriptor_t filterDesc,
    cudnnDataType_t dataType,
    cudnnTensorFormat_t format,
    int k,
    int c,
    int h,
    int w) {
  struct cudnnFilterStruct *descriptor;
  size_t count;
  if (filterDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (format != CUDNN_TENSOR_NCHW) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (dataType != CUDNN_DATA_FLOAT) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (k <= 0 || c <= 0 || h <= 0 || w <= 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  count = (size_t)k;
  if (
      count > SIZE_MAX / (size_t)c ||
      count * (size_t)c > SIZE_MAX / (size_t)h ||
      count * (size_t)c * (size_t)h > SIZE_MAX / (size_t)w) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  count = count * (size_t)c * (size_t)h * (size_t)w;
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor = psyche_cudnn_find_filter_locked(filterDesc);
  if (descriptor != 0) {
    descriptor->is_set = 1;
    descriptor->data_type = dataType;
    descriptor->format = format;
    descriptor->k = k;
    descriptor->c = c;
    descriptor->h = h;
    descriptor->w = w;
    descriptor->element_count = count;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  return descriptor != 0 ? CUDNN_STATUS_SUCCESS : CUDNN_STATUS_BAD_PARAM;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetFilter4dDescriptor(
    const cudnnFilterDescriptor_t filterDesc,
    cudnnDataType_t *dataType,
    cudnnTensorFormat_t *format,
    int *k,
    int *c,
    int *h,
    int *w) {
  struct cudnnFilterStruct *descriptor;
  struct cudnnFilterStruct snapshot;
  if (
      filterDesc == 0 ||
      dataType == 0 ||
      format == 0 ||
      k == 0 ||
      c == 0 ||
      h == 0 ||
      w == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor = psyche_cudnn_find_filter_locked((cudnnFilterDescriptor_t)filterDesc);
  if (descriptor != 0) {
    snapshot = *descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (descriptor == 0 || !snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *dataType = snapshot.data_type;
  *format = snapshot.format;
  *k = snapshot.k;
  *c = snapshot.c;
  *h = snapshot.h;
  *w = snapshot.w;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnDestroyFilterDescriptor(
    cudnnFilterDescriptor_t filterDesc) {
  int removed;
  if (filterDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  removed = psyche_cudnn_remove_filter_locked(filterDesc);
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (!removed) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  free(filterDesc);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnCreateConvolutionDescriptor(
    cudnnConvolutionDescriptor_t *convDesc) {
  struct cudnnConvolutionStruct *descriptor;
  if (convDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *convDesc = 0;
  descriptor = (struct cudnnConvolutionStruct *)calloc(1, sizeof(*descriptor));
  if (descriptor == 0) {
    return CUDNN_STATUS_ALLOC_FAILED;
  }
  descriptor->magic = PSYCHE_CUDNN_CONVOLUTION_MAGIC;
  descriptor->group_count = 1;
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor->next = psyche_cudnn_convolutions;
  psyche_cudnn_convolutions = descriptor;
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  *convDesc = descriptor;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnSetConvolution2dDescriptor(
    cudnnConvolutionDescriptor_t convDesc,
    int pad_h,
    int pad_w,
    int u,
    int v,
    int dilation_h,
    int dilation_w,
    cudnnConvolutionMode_t mode,
    cudnnDataType_t computeType) {
  struct cudnnConvolutionStruct *descriptor;
  if (convDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      pad_h < 0 ||
      pad_w < 0 ||
      u <= 0 ||
      v <= 0 ||
      dilation_h <= 0 ||
      dilation_w <= 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (mode != CUDNN_CONVOLUTION && mode != CUDNN_CROSS_CORRELATION) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (computeType != CUDNN_DATA_FLOAT) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor = psyche_cudnn_find_convolution_locked(convDesc);
  if (descriptor != 0) {
    descriptor->is_set = 1;
    descriptor->pad_h = pad_h;
    descriptor->pad_w = pad_w;
    descriptor->stride_h = u;
    descriptor->stride_w = v;
    descriptor->dilation_h = dilation_h;
    descriptor->dilation_w = dilation_w;
    descriptor->mode = mode;
    descriptor->compute_type = computeType;
    if (descriptor->group_count <= 0) {
      descriptor->group_count = 1;
    }
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  return descriptor != 0 ? CUDNN_STATUS_SUCCESS : CUDNN_STATUS_BAD_PARAM;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolution2dDescriptor(
    const cudnnConvolutionDescriptor_t convDesc,
    int *pad_h,
    int *pad_w,
    int *u,
    int *v,
    int *dilation_h,
    int *dilation_w,
    cudnnConvolutionMode_t *mode,
    cudnnDataType_t *computeType) {
  struct cudnnConvolutionStruct *descriptor;
  struct cudnnConvolutionStruct snapshot;
  if (
      convDesc == 0 ||
      pad_h == 0 ||
      pad_w == 0 ||
      u == 0 ||
      v == 0 ||
      dilation_h == 0 ||
      dilation_w == 0 ||
      mode == 0 ||
      computeType == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor = psyche_cudnn_find_convolution_locked((cudnnConvolutionDescriptor_t)convDesc);
  if (descriptor != 0) {
    snapshot = *descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (descriptor == 0 || !snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *pad_h = snapshot.pad_h;
  *pad_w = snapshot.pad_w;
  *u = snapshot.stride_h;
  *v = snapshot.stride_w;
  *dilation_h = snapshot.dilation_h;
  *dilation_w = snapshot.dilation_w;
  *mode = snapshot.mode;
  *computeType = snapshot.compute_type;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnSetConvolutionGroupCount(
    cudnnConvolutionDescriptor_t convDesc,
    int groupCount) {
  struct cudnnConvolutionStruct *descriptor;
  if (convDesc == 0 || groupCount <= 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor = psyche_cudnn_find_convolution_locked(convDesc);
  if (descriptor != 0) {
    descriptor->group_count = groupCount;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  return descriptor != 0 ? CUDNN_STATUS_SUCCESS : CUDNN_STATUS_BAD_PARAM;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionGroupCount(
    const cudnnConvolutionDescriptor_t convDesc,
    int *groupCount) {
  struct cudnnConvolutionStruct *descriptor;
  int count = 0;
  if (convDesc == 0 || groupCount == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor = psyche_cudnn_find_convolution_locked((cudnnConvolutionDescriptor_t)convDesc);
  if (descriptor != 0) {
    count = descriptor->group_count;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *groupCount = count > 0 ? count : 1;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolution2dForwardOutputDim(
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t inputTensorDesc,
    const cudnnFilterDescriptor_t filterDesc,
    int *n,
    int *c,
    int *h,
    int *w) {
  struct cudnnConvolutionStruct *conv_descriptor;
  struct cudnnTensorStruct *input_descriptor;
  struct cudnnFilterStruct *filter_descriptor;
  struct cudnnConvolutionStruct conv_snapshot;
  struct cudnnTensorStruct input_snapshot;
  struct cudnnFilterStruct filter_snapshot;
  if (
      convDesc == 0 ||
      inputTensorDesc == 0 ||
      filterDesc == 0 ||
      n == 0 ||
      c == 0 ||
      h == 0 ||
      w == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  conv_descriptor = psyche_cudnn_find_convolution_locked((cudnnConvolutionDescriptor_t)convDesc);
  input_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)inputTensorDesc);
  filter_descriptor = psyche_cudnn_find_filter_locked((cudnnFilterDescriptor_t)filterDesc);
  if (conv_descriptor != 0 && input_descriptor != 0 && filter_descriptor != 0) {
    conv_snapshot = *conv_descriptor;
    input_snapshot = *input_descriptor;
    filter_snapshot = *filter_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (conv_descriptor == 0 || input_descriptor == 0 || filter_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!conv_snapshot.is_set || !input_snapshot.is_set || !filter_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      !psyche_cudnn_convolution_output_dims(
          &input_snapshot,
          &filter_snapshot,
          &conv_snapshot,
          n,
          c,
          h,
          w)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionForwardAlgorithmMaxCount(
    cudnnHandle_t handle,
    int *count) {
  struct cudnnContext *context;
  if (handle == 0 || count == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  *count = 1;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionForwardAlgorithm(
    cudnnHandle_t handle,
    const cudnnTensorDescriptor_t xDesc,
    const cudnnFilterDescriptor_t wDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t yDesc,
    cudnnConvolutionFwdPreference_t preference,
    size_t memoryLimitInBytes,
    cudnnConvolutionFwdAlgo_t *algo) {
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnFilterStruct w_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  cudnnStatus_t status;
  (void)memoryLimitInBytes;
  if (algo == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      preference != CUDNN_CONVOLUTION_FWD_NO_WORKSPACE &&
      preference != CUDNN_CONVOLUTION_FWD_PREFER_FASTEST &&
      preference != CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  status = psyche_cudnn_snapshot_convolution_forward_config(
      handle,
      xDesc,
      wDesc,
      convDesc,
      yDesc,
      &x_snapshot,
      &w_snapshot,
      &conv_snapshot,
      &y_snapshot);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cudnn_validate_convolution_forward_config(
      &x_snapshot,
      &w_snapshot,
      &conv_snapshot,
      &y_snapshot,
      CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  *algo = CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionForwardAlgorithm_v7(
    cudnnHandle_t handle,
    const cudnnTensorDescriptor_t xDesc,
    const cudnnFilterDescriptor_t wDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t yDesc,
    const int requestedAlgoCount,
    int *returnedAlgoCount,
    cudnnConvolutionFwdAlgoPerf_t *perfResults) {
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnFilterStruct w_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  cudnnStatus_t status;
  if (requestedAlgoCount <= 0 || returnedAlgoCount == 0 || perfResults == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  status = psyche_cudnn_snapshot_convolution_forward_config(
      handle,
      xDesc,
      wDesc,
      convDesc,
      yDesc,
      &x_snapshot,
      &w_snapshot,
      &conv_snapshot,
      &y_snapshot);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cudnn_validate_convolution_forward_config(
      &x_snapshot,
      &w_snapshot,
      &conv_snapshot,
      &y_snapshot,
      CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  *returnedAlgoCount = 1;
  psyche_cudnn_fill_convolution_forward_perf(&perfResults[0]);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnFindConvolutionForwardAlgorithm(
    cudnnHandle_t handle,
    const cudnnTensorDescriptor_t xDesc,
    const cudnnFilterDescriptor_t wDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t yDesc,
    const int requestedAlgoCount,
    int *returnedAlgoCount,
    cudnnConvolutionFwdAlgoPerf_t *perfResults) {
  return cudnnGetConvolutionForwardAlgorithm_v7(
      handle,
      xDesc,
      wDesc,
      convDesc,
      yDesc,
      requestedAlgoCount,
      returnedAlgoCount,
      perfResults);
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionForwardWorkspaceSize(
    cudnnHandle_t handle,
    const cudnnTensorDescriptor_t xDesc,
    const cudnnFilterDescriptor_t wDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t yDesc,
    cudnnConvolutionFwdAlgo_t algo,
    size_t *sizeInBytes) {
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnFilterStruct w_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  cudnnStatus_t status;
  if (sizeInBytes == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  status = psyche_cudnn_snapshot_convolution_forward_config(
      handle,
      xDesc,
      wDesc,
      convDesc,
      yDesc,
      &x_snapshot,
      &w_snapshot,
      &conv_snapshot,
      &y_snapshot);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cudnn_validate_convolution_forward_config(
      &x_snapshot,
      &w_snapshot,
      &conv_snapshot,
      &y_snapshot,
      algo);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  *sizeInBytes = 0;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionBackwardDataAlgorithmMaxCount(
    cudnnHandle_t handle,
    int *count) {
  struct cudnnContext *context;
  if (handle == 0 || count == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  *count = 1;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionBackwardDataAlgorithm(
    cudnnHandle_t handle,
    const cudnnFilterDescriptor_t wDesc,
    const cudnnTensorDescriptor_t dyDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t dxDesc,
    cudnnConvolutionBwdDataPreference_t preference,
    size_t memoryLimitInBytes,
    cudnnConvolutionBwdDataAlgo_t *algo) {
  struct cudnnFilterStruct w_snapshot;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  struct cudnnTensorStruct dx_snapshot;
  cudnnStatus_t status;
  (void)memoryLimitInBytes;
  if (algo == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      preference != CUDNN_CONVOLUTION_BWD_DATA_NO_WORKSPACE &&
      preference != CUDNN_CONVOLUTION_BWD_DATA_PREFER_FASTEST &&
      preference != CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  status = psyche_cudnn_snapshot_convolution_backward_data_config(
      handle,
      wDesc,
      dyDesc,
      convDesc,
      dxDesc,
      &w_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dx_snapshot);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cudnn_validate_convolution_backward_data_config(
      &w_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dx_snapshot,
      CUDNN_CONVOLUTION_BWD_DATA_ALGO_1);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  *algo = CUDNN_CONVOLUTION_BWD_DATA_ALGO_1;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionBackwardDataAlgorithm_v7(
    cudnnHandle_t handle,
    const cudnnFilterDescriptor_t wDesc,
    const cudnnTensorDescriptor_t dyDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t dxDesc,
    const int requestedAlgoCount,
    int *returnedAlgoCount,
    cudnnConvolutionBwdDataAlgoPerf_t *perfResults) {
  struct cudnnFilterStruct w_snapshot;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  struct cudnnTensorStruct dx_snapshot;
  cudnnStatus_t status;
  if (requestedAlgoCount <= 0 || returnedAlgoCount == 0 || perfResults == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  status = psyche_cudnn_snapshot_convolution_backward_data_config(
      handle,
      wDesc,
      dyDesc,
      convDesc,
      dxDesc,
      &w_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dx_snapshot);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cudnn_validate_convolution_backward_data_config(
      &w_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dx_snapshot,
      CUDNN_CONVOLUTION_BWD_DATA_ALGO_1);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  *returnedAlgoCount = 1;
  psyche_cudnn_fill_convolution_backward_data_perf(&perfResults[0]);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnFindConvolutionBackwardDataAlgorithm(
    cudnnHandle_t handle,
    const cudnnFilterDescriptor_t wDesc,
    const cudnnTensorDescriptor_t dyDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t dxDesc,
    const int requestedAlgoCount,
    int *returnedAlgoCount,
    cudnnConvolutionBwdDataAlgoPerf_t *perfResults) {
  return cudnnGetConvolutionBackwardDataAlgorithm_v7(
      handle,
      wDesc,
      dyDesc,
      convDesc,
      dxDesc,
      requestedAlgoCount,
      returnedAlgoCount,
      perfResults);
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionBackwardDataWorkspaceSize(
    cudnnHandle_t handle,
    const cudnnFilterDescriptor_t wDesc,
    const cudnnTensorDescriptor_t dyDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnTensorDescriptor_t dxDesc,
    cudnnConvolutionBwdDataAlgo_t algo,
    size_t *sizeInBytes) {
  struct cudnnFilterStruct w_snapshot;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  struct cudnnTensorStruct dx_snapshot;
  cudnnStatus_t status;
  if (sizeInBytes == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  status = psyche_cudnn_snapshot_convolution_backward_data_config(
      handle,
      wDesc,
      dyDesc,
      convDesc,
      dxDesc,
      &w_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dx_snapshot);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cudnn_validate_convolution_backward_data_config(
      &w_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dx_snapshot,
      algo);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  *sizeInBytes = 0;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionBackwardFilterAlgorithmMaxCount(
    cudnnHandle_t handle,
    int *count) {
  struct cudnnContext *context;
  if (handle == 0 || count == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  *count = 1;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionBackwardFilterAlgorithm(
    cudnnHandle_t handle,
    const cudnnTensorDescriptor_t xDesc,
    const cudnnTensorDescriptor_t dyDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnFilterDescriptor_t dwDesc,
    cudnnConvolutionBwdFilterPreference_t preference,
    size_t memoryLimitInBytes,
    cudnnConvolutionBwdFilterAlgo_t *algo) {
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  struct cudnnFilterStruct dw_snapshot;
  cudnnStatus_t status;
  (void)memoryLimitInBytes;
  if (algo == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      preference != CUDNN_CONVOLUTION_BWD_FILTER_NO_WORKSPACE &&
      preference != CUDNN_CONVOLUTION_BWD_FILTER_PREFER_FASTEST &&
      preference != CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  status = psyche_cudnn_snapshot_convolution_backward_filter_config(
      handle,
      xDesc,
      dyDesc,
      convDesc,
      dwDesc,
      &x_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dw_snapshot);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cudnn_validate_convolution_backward_filter_config(
      &x_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dw_snapshot,
      CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  *algo = CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionBackwardFilterAlgorithm_v7(
    cudnnHandle_t handle,
    const cudnnTensorDescriptor_t xDesc,
    const cudnnTensorDescriptor_t dyDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnFilterDescriptor_t dwDesc,
    const int requestedAlgoCount,
    int *returnedAlgoCount,
    cudnnConvolutionBwdFilterAlgoPerf_t *perfResults) {
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  struct cudnnFilterStruct dw_snapshot;
  cudnnStatus_t status;
  if (requestedAlgoCount <= 0 || returnedAlgoCount == 0 || perfResults == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  status = psyche_cudnn_snapshot_convolution_backward_filter_config(
      handle,
      xDesc,
      dyDesc,
      convDesc,
      dwDesc,
      &x_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dw_snapshot);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cudnn_validate_convolution_backward_filter_config(
      &x_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dw_snapshot,
      CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  *returnedAlgoCount = 1;
  psyche_cudnn_fill_convolution_backward_filter_perf(&perfResults[0]);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnFindConvolutionBackwardFilterAlgorithm(
    cudnnHandle_t handle,
    const cudnnTensorDescriptor_t xDesc,
    const cudnnTensorDescriptor_t dyDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnFilterDescriptor_t dwDesc,
    const int requestedAlgoCount,
    int *returnedAlgoCount,
    cudnnConvolutionBwdFilterAlgoPerf_t *perfResults) {
  return cudnnGetConvolutionBackwardFilterAlgorithm_v7(
      handle,
      xDesc,
      dyDesc,
      convDesc,
      dwDesc,
      requestedAlgoCount,
      returnedAlgoCount,
      perfResults);
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetConvolutionBackwardFilterWorkspaceSize(
    cudnnHandle_t handle,
    const cudnnTensorDescriptor_t xDesc,
    const cudnnTensorDescriptor_t dyDesc,
    const cudnnConvolutionDescriptor_t convDesc,
    const cudnnFilterDescriptor_t dwDesc,
    cudnnConvolutionBwdFilterAlgo_t algo,
    size_t *sizeInBytes) {
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  struct cudnnFilterStruct dw_snapshot;
  cudnnStatus_t status;
  if (sizeInBytes == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  status = psyche_cudnn_snapshot_convolution_backward_filter_config(
      handle,
      xDesc,
      dyDesc,
      convDesc,
      dwDesc,
      &x_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dw_snapshot);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  status = psyche_cudnn_validate_convolution_backward_filter_config(
      &x_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dw_snapshot,
      algo);
  if (status != CUDNN_STATUS_SUCCESS) {
    return status;
  }
  *sizeInBytes = 0;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnDestroyConvolutionDescriptor(
    cudnnConvolutionDescriptor_t convDesc) {
  int removed;
  if (convDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  removed = psyche_cudnn_remove_convolution_locked(convDesc);
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (!removed) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  free(convDesc);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnCreateActivationDescriptor(
    cudnnActivationDescriptor_t *activationDesc) {
  struct cudnnActivationStruct *descriptor;
  if (activationDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *activationDesc = 0;
  descriptor = (struct cudnnActivationStruct *)calloc(1, sizeof(*descriptor));
  if (descriptor == 0) {
    return CUDNN_STATUS_ALLOC_FAILED;
  }
  descriptor->magic = PSYCHE_CUDNN_ACTIVATION_MAGIC;
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor->next = psyche_cudnn_activations;
  psyche_cudnn_activations = descriptor;
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  *activationDesc = descriptor;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnSetActivationDescriptor(
    cudnnActivationDescriptor_t activationDesc,
    cudnnActivationMode_t mode,
    cudnnNanPropagation_t reluNanOpt,
    double coef) {
  struct cudnnActivationStruct *descriptor;
  if (activationDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (reluNanOpt != CUDNN_NOT_PROPAGATE_NAN && reluNanOpt != CUDNN_PROPAGATE_NAN) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  switch (mode) {
  case CUDNN_ACTIVATION_SIGMOID:
  case CUDNN_ACTIVATION_RELU:
  case CUDNN_ACTIVATION_TANH:
  case CUDNN_ACTIVATION_IDENTITY:
    break;
  case CUDNN_ACTIVATION_CLIPPED_RELU:
  case CUDNN_ACTIVATION_ELU:
  case CUDNN_ACTIVATION_SWISH:
    return CUDNN_STATUS_NOT_SUPPORTED;
  default:
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor = psyche_cudnn_find_activation_locked(activationDesc);
  if (descriptor != 0) {
    descriptor->is_set = 1;
    descriptor->mode = mode;
    descriptor->nan_opt = reluNanOpt;
    descriptor->coef = coef;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  return descriptor != 0 ? CUDNN_STATUS_SUCCESS : CUDNN_STATUS_BAD_PARAM;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnDestroyActivationDescriptor(
    cudnnActivationDescriptor_t activationDesc) {
  int removed;
  if (activationDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  removed = psyche_cudnn_remove_activation_locked(activationDesc);
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (!removed) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  free(activationDesc);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnCreatePoolingDescriptor(
    cudnnPoolingDescriptor_t *poolingDesc) {
  struct cudnnPoolingStruct *descriptor;
  if (poolingDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *poolingDesc = 0;
  descriptor = (struct cudnnPoolingStruct *)calloc(1, sizeof(*descriptor));
  if (descriptor == 0) {
    return CUDNN_STATUS_ALLOC_FAILED;
  }
  descriptor->magic = PSYCHE_CUDNN_POOLING_MAGIC;
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor->next = psyche_cudnn_poolings;
  psyche_cudnn_poolings = descriptor;
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  *poolingDesc = descriptor;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnSetPooling2dDescriptor(
    cudnnPoolingDescriptor_t poolingDesc,
    cudnnPoolingMode_t mode,
    cudnnNanPropagation_t maxpoolingNanOpt,
    int windowHeight,
    int windowWidth,
    int verticalPadding,
    int horizontalPadding,
    int verticalStride,
    int horizontalStride) {
  struct cudnnPoolingStruct *descriptor;
  if (poolingDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (maxpoolingNanOpt != CUDNN_NOT_PROPAGATE_NAN && maxpoolingNanOpt != CUDNN_PROPAGATE_NAN) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  switch (mode) {
  case CUDNN_POOLING_MAX:
  case CUDNN_POOLING_MAX_DETERMINISTIC:
  case CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING:
  case CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING:
    break;
  default:
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      windowHeight <= 0 ||
      windowWidth <= 0 ||
      verticalPadding < 0 ||
      horizontalPadding < 0 ||
      verticalStride <= 0 ||
      horizontalStride <= 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor = psyche_cudnn_find_pooling_locked(poolingDesc);
  if (descriptor != 0) {
    descriptor->is_set = 1;
    descriptor->mode = mode;
    descriptor->nan_opt = maxpoolingNanOpt;
    descriptor->window_h = windowHeight;
    descriptor->window_w = windowWidth;
    descriptor->pad_h = verticalPadding;
    descriptor->pad_w = horizontalPadding;
    descriptor->stride_h = verticalStride;
    descriptor->stride_w = horizontalStride;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  return descriptor != 0 ? CUDNN_STATUS_SUCCESS : CUDNN_STATUS_BAD_PARAM;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetPooling2dDescriptor(
    const cudnnPoolingDescriptor_t poolingDesc,
    cudnnPoolingMode_t *mode,
    cudnnNanPropagation_t *maxpoolingNanOpt,
    int *windowHeight,
    int *windowWidth,
    int *verticalPadding,
    int *horizontalPadding,
    int *verticalStride,
    int *horizontalStride) {
  struct cudnnPoolingStruct *descriptor;
  struct cudnnPoolingStruct snapshot;
  if (
      poolingDesc == 0 ||
      mode == 0 ||
      maxpoolingNanOpt == 0 ||
      windowHeight == 0 ||
      windowWidth == 0 ||
      verticalPadding == 0 ||
      horizontalPadding == 0 ||
      verticalStride == 0 ||
      horizontalStride == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  descriptor = psyche_cudnn_find_pooling_locked((cudnnPoolingDescriptor_t)poolingDesc);
  if (descriptor != 0) {
    snapshot = *descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (descriptor == 0 || !snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *mode = snapshot.mode;
  *maxpoolingNanOpt = snapshot.nan_opt;
  *windowHeight = snapshot.window_h;
  *windowWidth = snapshot.window_w;
  *verticalPadding = snapshot.pad_h;
  *horizontalPadding = snapshot.pad_w;
  *verticalStride = snapshot.stride_h;
  *horizontalStride = snapshot.stride_w;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnGetPooling2dForwardOutputDim(
    const cudnnPoolingDescriptor_t poolingDesc,
    const cudnnTensorDescriptor_t inputTensorDesc,
    int *outN,
    int *outC,
    int *outH,
    int *outW) {
  struct cudnnPoolingStruct *pool_descriptor;
  struct cudnnTensorStruct *input_descriptor;
  struct cudnnPoolingStruct pool_snapshot;
  struct cudnnTensorStruct input_snapshot;
  int output_h;
  int output_w;
  if (
      poolingDesc == 0 ||
      inputTensorDesc == 0 ||
      outN == 0 ||
      outC == 0 ||
      outH == 0 ||
      outW == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  pool_descriptor = psyche_cudnn_find_pooling_locked((cudnnPoolingDescriptor_t)poolingDesc);
  input_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)inputTensorDesc);
  if (pool_descriptor != 0 && input_descriptor != 0) {
    pool_snapshot = *pool_descriptor;
    input_snapshot = *input_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (pool_descriptor == 0 || input_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!pool_snapshot.is_set || !input_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (input_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      !psyche_cudnn_pooling_output_dim(
          input_snapshot.dim[2],
          pool_snapshot.pad_h,
          pool_snapshot.window_h,
          pool_snapshot.stride_h,
          &output_h) ||
      !psyche_cudnn_pooling_output_dim(
          input_snapshot.dim[3],
          pool_snapshot.pad_w,
          pool_snapshot.window_w,
          pool_snapshot.stride_w,
          &output_w)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  *outN = input_snapshot.dim[0];
  *outC = input_snapshot.dim[1];
  *outH = output_h;
  *outW = output_w;
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnDestroyPoolingDescriptor(
    cudnnPoolingDescriptor_t poolingDesc) {
  int removed;
  if (poolingDesc == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  pthread_mutex_lock(&psyche_cudnn_mutex);
  removed = psyche_cudnn_remove_pooling_locked(poolingDesc);
  pthread_mutex_unlock(&psyche_cudnn_mutex);
  if (!removed) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  free(poolingDesc);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnActivationForward(
    cudnnHandle_t handle,
    cudnnActivationDescriptor_t activationDesc,
    const void *alpha,
    const cudnnTensorDescriptor_t xDesc,
    const void *x,
    const void *beta,
    const cudnnTensorDescriptor_t yDesc,
    void *y) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *x_descriptor;
  struct cudnnTensorStruct *y_descriptor;
  struct cudnnActivationStruct *activation_descriptor;
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnActivationStruct activation_snapshot;
  size_t count;
  size_t bytes;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;
  int descriptors_equal;

  if (
      handle == 0 ||
      activationDesc == 0 ||
      alpha == 0 ||
      xDesc == 0 ||
      x == 0 ||
      beta == 0 ||
      yDesc == 0 ||
      y == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  activation_descriptor = psyche_cudnn_find_activation_locked(activationDesc);
  x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  if (
      context != 0 &&
      activation_descriptor != 0 &&
      x_descriptor != 0 &&
      y_descriptor != 0) {
    x_snapshot = *x_descriptor;
    y_snapshot = *y_descriptor;
    activation_snapshot = *activation_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (activation_descriptor == 0 || x_descriptor == 0 || y_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!activation_snapshot.is_set || !x_snapshot.is_set || !y_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      activation_snapshot.mode != CUDNN_ACTIVATION_SIGMOID &&
      activation_snapshot.mode != CUDNN_ACTIVATION_RELU &&
      activation_snapshot.mode != CUDNN_ACTIVATION_TANH &&
      activation_snapshot.mode != CUDNN_ACTIVATION_IDENTITY) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      x_snapshot.data_type != CUDNN_DATA_FLOAT ||
      y_snapshot.data_type != CUDNN_DATA_FLOAT ||
      x_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      y_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(&x_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&y_snapshot)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  descriptors_equal = psyche_cudnn_tensor_values_equal(&x_snapshot, &y_snapshot);
  if (!descriptors_equal) {
    if (!psyche_cudnn_tensor_dims_equal(&x_snapshot, &y_snapshot)) {
      return CUDNN_STATUS_BAD_PARAM;
    }
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  count = x_snapshot.element_count;
  if (count == 0 || count > UINT_MAX || count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  bytes = count * sizeof(float);
  if (psyche_cudnn_ranges_overlap(x, y, bytes) && x != y) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  /* This FP32-only subset follows cuDNN host-scalar convention: alpha/beta are float pointers. */
  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  grid_dim = (unsigned int)((count + PSYCHE_CUDNN_BLOCK_THREADS - 1U) / PSYCHE_CUDNN_BLOCK_THREADS);

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    CUresult metal_result = psyche_cuda_metal_launch_cudnn_activation_f32(
        (const float *)x,
        (const float *)y,
        (float *)y,
        alpha_value,
        beta_value,
        (unsigned int)activation_snapshot.mode,
        (unsigned int)activation_snapshot.nan_opt,
        (unsigned int)count,
        bytes,
        grid_dim,
        PSYCHE_CUDNN_BLOCK_THREADS);
    if (metal_result == CUDA_SUCCESS) {
      return CUDNN_STATUS_SUCCESS;
    }
    if (
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  psyche_cudnn_cpu_activation_forward(
      (const float *)x,
      (const float *)y,
      (float *)y,
      count,
      alpha_value,
      beta_value,
      activation_snapshot.mode,
      activation_snapshot.nan_opt);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnActivationBackward(
    cudnnHandle_t handle,
    cudnnActivationDescriptor_t activationDesc,
    const void *alpha,
    const cudnnTensorDescriptor_t yDesc,
    const void *y,
    const cudnnTensorDescriptor_t dyDesc,
    const void *dy,
    const cudnnTensorDescriptor_t xDesc,
    const void *x,
    const void *beta,
    const cudnnTensorDescriptor_t dxDesc,
    void *dx) {
  struct cudnnContext *context;
  struct cudnnActivationStruct *activation_descriptor;
  struct cudnnTensorStruct *y_descriptor;
  struct cudnnTensorStruct *dy_descriptor;
  struct cudnnTensorStruct *x_descriptor;
  struct cudnnTensorStruct *dx_descriptor;
  struct cudnnActivationStruct activation_snapshot;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct dx_snapshot;
  size_t count;
  size_t bytes;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;
  int descriptors_equal;
  int exact_dy_dx_alias;
  float *dy_dx_snapshot = 0;
  const float *dy_source;
  const float *dx_source;

  if (
      handle == 0 ||
      activationDesc == 0 ||
      alpha == 0 ||
      yDesc == 0 ||
      y == 0 ||
      dyDesc == 0 ||
      dy == 0 ||
      xDesc == 0 ||
      x == 0 ||
      beta == 0 ||
      dxDesc == 0 ||
      dx == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  activation_descriptor = psyche_cudnn_find_activation_locked(activationDesc);
  y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  dy_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)dyDesc);
  x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  dx_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)dxDesc);
  if (
      context != 0 &&
      activation_descriptor != 0 &&
      y_descriptor != 0 &&
      dy_descriptor != 0 &&
      x_descriptor != 0 &&
      dx_descriptor != 0) {
    activation_snapshot = *activation_descriptor;
    y_snapshot = *y_descriptor;
    dy_snapshot = *dy_descriptor;
    x_snapshot = *x_descriptor;
    dx_snapshot = *dx_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (
      activation_descriptor == 0 ||
      y_descriptor == 0 ||
      dy_descriptor == 0 ||
      x_descriptor == 0 ||
      dx_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      !activation_snapshot.is_set ||
      !y_snapshot.is_set ||
      !dy_snapshot.is_set ||
      !x_snapshot.is_set ||
      !dx_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      activation_snapshot.mode != CUDNN_ACTIVATION_SIGMOID &&
      activation_snapshot.mode != CUDNN_ACTIVATION_RELU &&
      activation_snapshot.mode != CUDNN_ACTIVATION_TANH &&
      activation_snapshot.mode != CUDNN_ACTIVATION_IDENTITY) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      y_snapshot.data_type != CUDNN_DATA_FLOAT ||
      dy_snapshot.data_type != CUDNN_DATA_FLOAT ||
      x_snapshot.data_type != CUDNN_DATA_FLOAT ||
      dx_snapshot.data_type != CUDNN_DATA_FLOAT ||
      y_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      dy_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      x_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      dx_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(&y_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&dy_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&x_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&dx_snapshot)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_dims_equal(&x_snapshot, &y_snapshot) ||
      !psyche_cudnn_tensor_dims_equal(&x_snapshot, &dy_snapshot) ||
      !psyche_cudnn_tensor_dims_equal(&x_snapshot, &dx_snapshot)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  descriptors_equal =
      psyche_cudnn_tensor_values_equal(&x_snapshot, &y_snapshot) &&
      psyche_cudnn_tensor_values_equal(&x_snapshot, &dy_snapshot) &&
      psyche_cudnn_tensor_values_equal(&x_snapshot, &dx_snapshot);
  if (!descriptors_equal) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  count = x_snapshot.element_count;
  if (count == 0 || count > UINT_MAX || count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  bytes = count * sizeof(float);
  exact_dy_dx_alias = dy == dx;
  if (psyche_cudnn_ranges_overlap(dy, dx, bytes)) {
    if (!exact_dy_dx_alias) {
      return CUDNN_STATUS_BAD_PARAM;
    }
  }
  if (
      psyche_cudnn_ranges_overlap(x, dx, bytes) ||
      psyche_cudnn_ranges_overlap(y, dx, bytes)) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  grid_dim = (unsigned int)((count + PSYCHE_CUDNN_BLOCK_THREADS - 1U) / PSYCHE_CUDNN_BLOCK_THREADS);

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    CUresult metal_result = psyche_cuda_metal_launch_cudnn_activation_backward_f32(
        (const float *)x,
        (const float *)dy,
        (const float *)dx,
        (float *)dx,
        alpha_value,
        beta_value,
        (unsigned int)activation_snapshot.mode,
        (unsigned int)activation_snapshot.nan_opt,
        (unsigned int)count,
        bytes,
        grid_dim,
        PSYCHE_CUDNN_BLOCK_THREADS);
    if (metal_result == CUDA_SUCCESS) {
      return CUDNN_STATUS_SUCCESS;
    }
    if (
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  dy_source = (const float *)dy;
  dx_source = (const float *)dx;
  if (exact_dy_dx_alias) {
    dy_dx_snapshot = (float *)malloc(bytes);
    if (dy_dx_snapshot == 0) {
      return CUDNN_STATUS_ALLOC_FAILED;
    }
    memcpy(dy_dx_snapshot, dx, bytes);
    dy_source = dy_dx_snapshot;
    dx_source = dy_dx_snapshot;
  }
  psyche_cudnn_cpu_activation_backward(
      (const float *)x,
      dy_source,
      dx_source,
      (float *)dx,
      count,
      alpha_value,
      beta_value,
      activation_snapshot.mode,
      activation_snapshot.nan_opt);
  free(dy_dx_snapshot);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnAddTensor(
    cudnnHandle_t handle,
    const void *alpha,
    const cudnnTensorDescriptor_t aDesc,
    const void *A,
    const void *beta,
    const cudnnTensorDescriptor_t cDesc,
    void *C) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *a_descriptor;
  struct cudnnTensorStruct *c_descriptor;
  struct cudnnTensorStruct a_snapshot;
  struct cudnnTensorStruct c_snapshot;
  size_t a_count;
  size_t c_count;
  size_t a_bytes;
  size_t c_bytes;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;

  if (
      handle == 0 ||
      alpha == 0 ||
      aDesc == 0 ||
      A == 0 ||
      beta == 0 ||
      cDesc == 0 ||
      C == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  a_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)aDesc);
  c_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)cDesc);
  if (context != 0 && a_descriptor != 0 && c_descriptor != 0) {
    a_snapshot = *a_descriptor;
    c_snapshot = *c_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (a_descriptor == 0 || c_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!a_snapshot.is_set || !c_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (a_snapshot.data_type != c_snapshot.data_type) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (a_snapshot.data_type != CUDNN_DATA_FLOAT) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      a_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      c_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(&a_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&c_snapshot)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cudnn_tensor_can_broadcast_to(&a_snapshot, &c_snapshot)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  a_count = a_snapshot.element_count;
  c_count = c_snapshot.element_count;
  if (
      a_count == 0 ||
      c_count == 0 ||
      a_count > UINT_MAX ||
      c_count > UINT_MAX ||
      a_count > SIZE_MAX / sizeof(float) ||
      c_count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  a_bytes = a_count * sizeof(float);
  c_bytes = c_count * sizeof(float);
  if (psyche_cudnn_ranges_overlap_sized(A, a_bytes, C, c_bytes)) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  grid_dim = (unsigned int)((c_count + PSYCHE_CUDNN_BLOCK_THREADS - 1U) / PSYCHE_CUDNN_BLOCK_THREADS);

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    CUresult metal_result = psyche_cuda_metal_launch_cudnn_add_tensor_f32(
        (const float *)A,
        (const float *)C,
        (float *)C,
        alpha_value,
        beta_value,
        (unsigned int)a_snapshot.dim[0],
        (unsigned int)a_snapshot.dim[1],
        (unsigned int)a_snapshot.dim[2],
        (unsigned int)a_snapshot.dim[3],
        (unsigned int)c_snapshot.dim[0],
        (unsigned int)c_snapshot.dim[1],
        (unsigned int)c_snapshot.dim[2],
        (unsigned int)c_snapshot.dim[3],
        (unsigned int)c_count,
        a_bytes,
        c_bytes,
        grid_dim,
        PSYCHE_CUDNN_BLOCK_THREADS);
    if (metal_result == CUDA_SUCCESS) {
      return CUDNN_STATUS_SUCCESS;
    }
    if (
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  psyche_cudnn_cpu_add_tensor(
      (const float *)A,
      (const float *)C,
      (float *)C,
      &a_snapshot,
      &c_snapshot,
      alpha_value,
      beta_value);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnBatchNormalizationForwardInference(
    cudnnHandle_t handle,
    cudnnBatchNormMode_t mode,
    const void *alpha,
    const void *beta,
    const cudnnTensorDescriptor_t xDesc,
    const void *x,
    const cudnnTensorDescriptor_t yDesc,
    void *y,
    const cudnnTensorDescriptor_t bnScaleBiasMeanVarDesc,
    const void *bnScale,
    const void *bnBias,
    const void *estimatedMean,
    const void *estimatedVariance,
    double epsilon) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *x_descriptor;
  struct cudnnTensorStruct *y_descriptor;
  struct cudnnTensorStruct *bn_descriptor;
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnTensorStruct bn_snapshot;
  size_t tensor_count;
  size_t param_count;
  size_t tensor_bytes;
  size_t param_bytes;
  int exact_xy_alias;
  float alpha_value;
  float beta_value;
  float epsilon_value;
  unsigned int grid_dim;

  if (
      handle == 0 ||
      alpha == 0 ||
      beta == 0 ||
      xDesc == 0 ||
      x == 0 ||
      yDesc == 0 ||
      y == 0 ||
      bnScaleBiasMeanVarDesc == 0 ||
      bnScale == 0 ||
      bnBias == 0 ||
      estimatedMean == 0 ||
      estimatedVariance == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (epsilon < CUDNN_BN_MIN_EPSILON) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  bn_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)bnScaleBiasMeanVarDesc);
  if (context != 0 && x_descriptor != 0 && y_descriptor != 0 && bn_descriptor != 0) {
    x_snapshot = *x_descriptor;
    y_snapshot = *y_descriptor;
    bn_snapshot = *bn_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (x_descriptor == 0 || y_descriptor == 0 || bn_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!x_snapshot.is_set || !y_snapshot.is_set || !bn_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (mode == CUDNN_BATCHNORM_SPATIAL_PERSISTENT) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (mode != CUDNN_BATCHNORM_SPATIAL && mode != CUDNN_BATCHNORM_PER_ACTIVATION) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (x_snapshot.data_type != y_snapshot.data_type) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (x_snapshot.data_type != CUDNN_DATA_FLOAT || bn_snapshot.data_type != CUDNN_DATA_FLOAT) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      x_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      y_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      bn_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cudnn_tensor_dims_equal(&x_snapshot, &y_snapshot)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(&x_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&y_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&bn_snapshot)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cudnn_bn_param_count(mode, &x_snapshot, &bn_snapshot, &param_count)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  tensor_count = x_snapshot.element_count;
  if (
      tensor_count == 0 ||
      param_count == 0 ||
      tensor_count > UINT_MAX ||
      param_count > UINT_MAX ||
      tensor_count > SIZE_MAX / sizeof(float) ||
      param_count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  tensor_bytes = tensor_count * sizeof(float);
  param_bytes = param_count * sizeof(float);
  exact_xy_alias = x == y;
  if (psyche_cudnn_ranges_overlap_sized(x, tensor_bytes, y, tensor_bytes) && !exact_xy_alias) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      psyche_cudnn_ranges_overlap_sized(x, tensor_bytes, bnScale, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(x, tensor_bytes, bnBias, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(x, tensor_bytes, estimatedMean, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(x, tensor_bytes, estimatedVariance, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(y, tensor_bytes, bnScale, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(y, tensor_bytes, bnBias, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(y, tensor_bytes, estimatedMean, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(y, tensor_bytes, estimatedVariance, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(bnScale, param_bytes, bnBias, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(bnScale, param_bytes, estimatedMean, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(bnScale, param_bytes, estimatedVariance, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(bnBias, param_bytes, estimatedMean, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(bnBias, param_bytes, estimatedVariance, param_bytes) ||
      psyche_cudnn_ranges_overlap_sized(estimatedMean, param_bytes, estimatedVariance, param_bytes)) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  /* This FP32-only subset follows cuDNN host-scalar convention: alpha/beta are float pointers. */
  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  epsilon_value = (float)epsilon;
  grid_dim = (unsigned int)((tensor_count + PSYCHE_CUDNN_BLOCK_THREADS - 1U) / PSYCHE_CUDNN_BLOCK_THREADS);

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    CUresult metal_result = psyche_cuda_metal_launch_cudnn_batchnorm_inference_f32(
        (const float *)x,
        (const float *)y,
        (float *)y,
        (const float *)bnScale,
        (const float *)bnBias,
        (const float *)estimatedMean,
        (const float *)estimatedVariance,
        alpha_value,
        beta_value,
        epsilon_value,
        (unsigned int)mode,
        (unsigned int)x_snapshot.dim[1],
        (unsigned int)x_snapshot.dim[2],
        (unsigned int)x_snapshot.dim[3],
        (unsigned int)tensor_count,
        tensor_bytes,
        param_bytes,
        grid_dim,
        PSYCHE_CUDNN_BLOCK_THREADS);
    if (metal_result == CUDA_SUCCESS) {
      return CUDNN_STATUS_SUCCESS;
    }
    if (
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  psyche_cudnn_cpu_batchnorm_inference(
      (const float *)x,
      (const float *)y,
      (float *)y,
      (const float *)bnScale,
      (const float *)bnBias,
      (const float *)estimatedMean,
      (const float *)estimatedVariance,
      &x_snapshot,
      mode,
      alpha_value,
      beta_value,
      epsilon_value);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnConvolutionForward(
    cudnnHandle_t handle,
    const void *alpha,
    const cudnnTensorDescriptor_t xDesc,
    const void *x,
    const cudnnFilterDescriptor_t wDesc,
    const void *w,
    const cudnnConvolutionDescriptor_t convDesc,
    cudnnConvolutionFwdAlgo_t algo,
    void *workSpace,
    size_t workSpaceSizeInBytes,
    const void *beta,
    const cudnnTensorDescriptor_t yDesc,
    void *y) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *x_descriptor;
  struct cudnnTensorStruct *y_descriptor;
  struct cudnnFilterStruct *w_descriptor;
  struct cudnnConvolutionStruct *conv_descriptor;
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnFilterStruct w_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  cudnnStatus_t validation_status;
  size_t x_count;
  size_t w_count;
  size_t y_count;
  size_t x_bytes;
  size_t w_bytes;
  size_t y_bytes;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;

  (void)workSpace;
  (void)workSpaceSizeInBytes;

  if (
      handle == 0 ||
      alpha == 0 ||
      xDesc == 0 ||
      x == 0 ||
      wDesc == 0 ||
      w == 0 ||
      convDesc == 0 ||
      beta == 0 ||
      yDesc == 0 ||
      y == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (algo != CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  w_descriptor = psyche_cudnn_find_filter_locked((cudnnFilterDescriptor_t)wDesc);
  conv_descriptor = psyche_cudnn_find_convolution_locked((cudnnConvolutionDescriptor_t)convDesc);
  if (
      context != 0 &&
      x_descriptor != 0 &&
      y_descriptor != 0 &&
      w_descriptor != 0 &&
      conv_descriptor != 0) {
    x_snapshot = *x_descriptor;
    y_snapshot = *y_descriptor;
    w_snapshot = *w_descriptor;
    conv_snapshot = *conv_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (x_descriptor == 0 || y_descriptor == 0 || w_descriptor == 0 || conv_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!x_snapshot.is_set || !y_snapshot.is_set || !w_snapshot.is_set || !conv_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  validation_status = psyche_cudnn_validate_convolution_forward_config(
      &x_snapshot,
      &w_snapshot,
      &conv_snapshot,
      &y_snapshot,
      algo);
  if (validation_status != CUDNN_STATUS_SUCCESS) {
    return validation_status;
  }

  x_count = x_snapshot.element_count;
  w_count = w_snapshot.element_count;
  y_count = y_snapshot.element_count;
  if (
      x_count == 0 ||
      w_count == 0 ||
      y_count == 0 ||
      x_count > UINT_MAX ||
      w_count > UINT_MAX ||
      y_count > UINT_MAX ||
      x_count > SIZE_MAX / sizeof(float) ||
      w_count > SIZE_MAX / sizeof(float) ||
      y_count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  x_bytes = x_count * sizeof(float);
  w_bytes = w_count * sizeof(float);
  y_bytes = y_count * sizeof(float);
  if (
      psyche_cudnn_ranges_overlap_sized(x, x_bytes, y, y_bytes) ||
      psyche_cudnn_ranges_overlap_sized(x, x_bytes, w, w_bytes) ||
      psyche_cudnn_ranges_overlap_sized(w, w_bytes, y, y_bytes)) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  grid_dim = (unsigned int)((y_count + PSYCHE_CUDNN_BLOCK_THREADS - 1U) / PSYCHE_CUDNN_BLOCK_THREADS);

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    CUresult metal_result = psyche_cuda_metal_launch_cudnn_convolution_forward_f32(
        (const float *)x,
        (const float *)w,
        (const float *)y,
        (float *)y,
        alpha_value,
        beta_value,
        (unsigned int)conv_snapshot.mode,
        (unsigned int)conv_snapshot.group_count,
        (unsigned int)x_snapshot.dim[0],
        (unsigned int)x_snapshot.dim[1],
        (unsigned int)x_snapshot.dim[2],
        (unsigned int)x_snapshot.dim[3],
        (unsigned int)y_snapshot.dim[1],
        (unsigned int)y_snapshot.dim[2],
        (unsigned int)y_snapshot.dim[3],
        (unsigned int)w_snapshot.h,
        (unsigned int)w_snapshot.w,
        (unsigned int)conv_snapshot.pad_h,
        (unsigned int)conv_snapshot.pad_w,
        (unsigned int)conv_snapshot.stride_h,
        (unsigned int)conv_snapshot.stride_w,
        (unsigned int)conv_snapshot.dilation_h,
        (unsigned int)conv_snapshot.dilation_w,
        (unsigned int)y_count,
        x_bytes,
        w_bytes,
        y_bytes,
        grid_dim,
        PSYCHE_CUDNN_BLOCK_THREADS);
    if (metal_result == CUDA_SUCCESS) {
      return CUDNN_STATUS_SUCCESS;
    }
    if (
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  psyche_cudnn_cpu_convolution_forward(
      (const float *)x,
      (const float *)w,
      (const float *)y,
      (float *)y,
      &x_snapshot,
      &w_snapshot,
      &y_snapshot,
      &conv_snapshot,
      alpha_value,
      beta_value);
  return CUDNN_STATUS_SUCCESS;
}

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
    void *y) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *x_descriptor;
  struct cudnnTensorStruct *z_descriptor;
  struct cudnnTensorStruct *bias_descriptor;
  struct cudnnTensorStruct *y_descriptor;
  struct cudnnFilterStruct *w_descriptor;
  struct cudnnConvolutionStruct *conv_descriptor;
  struct cudnnActivationStruct *activation_descriptor;
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct z_snapshot;
  struct cudnnTensorStruct bias_snapshot;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnFilterStruct w_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  struct cudnnActivationStruct activation_snapshot;
  cudnnStatus_t validation_status;
  size_t x_count;
  size_t w_count;
  size_t z_count;
  size_t bias_count;
  size_t y_count;
  size_t x_bytes;
  size_t w_bytes;
  size_t z_bytes;
  size_t bias_bytes;
  size_t y_bytes;
  float alpha1_value;
  float alpha2_value;
  int z_y_overlap;
  unsigned int blockDimX;
  unsigned int gridDimX;
  CUresult metal_result;

  (void)workSpace;
  (void)workSpaceSizeInBytes;

  if (
      handle == 0 ||
      alpha1 == 0 ||
      xDesc == 0 ||
      x == 0 ||
      wDesc == 0 ||
      w == 0 ||
      convDesc == 0 ||
      alpha2 == 0 ||
      zDesc == 0 ||
      z == 0 ||
      biasDesc == 0 ||
      bias == 0 ||
      activationDesc == 0 ||
      yDesc == 0 ||
      y == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  z_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)zDesc);
  bias_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)biasDesc);
  y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  w_descriptor = psyche_cudnn_find_filter_locked((cudnnFilterDescriptor_t)wDesc);
  conv_descriptor = psyche_cudnn_find_convolution_locked((cudnnConvolutionDescriptor_t)convDesc);
  activation_descriptor = psyche_cudnn_find_activation_locked((cudnnActivationDescriptor_t)activationDesc);
  if (
      context != 0 &&
      x_descriptor != 0 &&
      z_descriptor != 0 &&
      bias_descriptor != 0 &&
      y_descriptor != 0 &&
      w_descriptor != 0 &&
      conv_descriptor != 0 &&
      activation_descriptor != 0) {
    x_snapshot = *x_descriptor;
    z_snapshot = *z_descriptor;
    bias_snapshot = *bias_descriptor;
    y_snapshot = *y_descriptor;
    w_snapshot = *w_descriptor;
    conv_snapshot = *conv_descriptor;
    activation_snapshot = *activation_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (
      x_descriptor == 0 ||
      z_descriptor == 0 ||
      bias_descriptor == 0 ||
      y_descriptor == 0 ||
      w_descriptor == 0 ||
      conv_descriptor == 0 ||
      activation_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  validation_status = psyche_cudnn_validate_convolution_bias_activation_forward_config(
      &x_snapshot,
      &w_snapshot,
      &conv_snapshot,
      algo,
      &z_snapshot,
      &bias_snapshot,
      &activation_snapshot,
      &y_snapshot);
  if (validation_status != CUDNN_STATUS_SUCCESS) {
    return validation_status;
  }

  x_count = x_snapshot.element_count;
  w_count = w_snapshot.element_count;
  z_count = z_snapshot.element_count;
  bias_count = bias_snapshot.element_count;
  y_count = y_snapshot.element_count;
  if (
      x_count == 0 ||
      w_count == 0 ||
      z_count == 0 ||
      bias_count == 0 ||
      y_count == 0 ||
      x_count > UINT_MAX ||
      w_count > UINT_MAX ||
      z_count > UINT_MAX ||
      y_count > UINT_MAX ||
      x_count > SIZE_MAX / sizeof(float) ||
      w_count > SIZE_MAX / sizeof(float) ||
      z_count > SIZE_MAX / sizeof(float) ||
      bias_count > SIZE_MAX / sizeof(float) ||
      y_count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  x_bytes = x_count * sizeof(float);
  w_bytes = w_count * sizeof(float);
  z_bytes = z_count * sizeof(float);
  bias_bytes = bias_count * sizeof(float);
  y_bytes = y_count * sizeof(float);
  z_y_overlap = psyche_cudnn_ranges_overlap_sized(z, z_bytes, y, y_bytes);
  if (
      psyche_cudnn_ranges_overlap_sized(x, x_bytes, w, w_bytes) ||
      psyche_cudnn_ranges_overlap_sized(x, x_bytes, z, z_bytes) ||
      psyche_cudnn_ranges_overlap_sized(x, x_bytes, bias, bias_bytes) ||
      psyche_cudnn_ranges_overlap_sized(x, x_bytes, y, y_bytes) ||
      psyche_cudnn_ranges_overlap_sized(w, w_bytes, z, z_bytes) ||
      psyche_cudnn_ranges_overlap_sized(w, w_bytes, bias, bias_bytes) ||
      psyche_cudnn_ranges_overlap_sized(w, w_bytes, y, y_bytes) ||
      psyche_cudnn_ranges_overlap_sized(bias, bias_bytes, z, z_bytes) ||
      psyche_cudnn_ranges_overlap_sized(bias, bias_bytes, y, y_bytes) ||
      (z_y_overlap && z != y)) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  alpha1_value = *(const float *)alpha1;
  alpha2_value = *(const float *)alpha2;
  if (psyche_cudnn_metal_enabled()) {
    blockDimX = 256U;
    gridDimX = (unsigned int)((y_count + (size_t)blockDimX - 1U) / (size_t)blockDimX);
    metal_result = psyche_cuda_metal_launch_cudnn_convolution_bias_activation_forward_f32(
        (const float *)x,
        (const float *)w,
        (const float *)z,
        (const float *)bias,
        (float *)y,
        alpha1_value,
        alpha2_value,
        (unsigned int)activation_snapshot.mode,
        (unsigned int)conv_snapshot.mode,
        (unsigned int)conv_snapshot.group_count,
        (unsigned int)x_snapshot.dim[0],
        (unsigned int)x_snapshot.dim[1],
        (unsigned int)x_snapshot.dim[2],
        (unsigned int)x_snapshot.dim[3],
        (unsigned int)y_snapshot.dim[1],
        (unsigned int)y_snapshot.dim[2],
        (unsigned int)y_snapshot.dim[3],
        (unsigned int)w_snapshot.h,
        (unsigned int)w_snapshot.w,
        (unsigned int)conv_snapshot.pad_h,
        (unsigned int)conv_snapshot.pad_w,
        (unsigned int)conv_snapshot.stride_h,
        (unsigned int)conv_snapshot.stride_w,
        (unsigned int)conv_snapshot.dilation_h,
        (unsigned int)conv_snapshot.dilation_w,
        (unsigned int)y_count,
        x_bytes,
        w_bytes,
        z_bytes,
        bias_bytes,
        y_bytes,
        gridDimX,
        blockDimX);
    if (
        metal_result == CUDA_SUCCESS ||
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  psyche_cudnn_cpu_convolution_bias_activation_forward(
      (const float *)x,
      (const float *)w,
      (const float *)z,
      (const float *)bias,
      (float *)y,
      &x_snapshot,
      &w_snapshot,
      &y_snapshot,
      &conv_snapshot,
      alpha1_value,
      alpha2_value,
      activation_snapshot.mode);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnConvolutionBackwardData(
    cudnnHandle_t handle,
    const void *alpha,
    const cudnnFilterDescriptor_t wDesc,
    const void *w,
    const cudnnTensorDescriptor_t dyDesc,
    const void *dy,
    const cudnnConvolutionDescriptor_t convDesc,
    cudnnConvolutionBwdDataAlgo_t algo,
    void *workSpace,
    size_t workSpaceSizeInBytes,
    const void *beta,
    const cudnnTensorDescriptor_t dxDesc,
    void *dx) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *dy_descriptor;
  struct cudnnTensorStruct *dx_descriptor;
  struct cudnnFilterStruct *w_descriptor;
  struct cudnnConvolutionStruct *conv_descriptor;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnTensorStruct dx_snapshot;
  struct cudnnFilterStruct w_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  cudnnStatus_t validation_status;
  size_t w_count;
  size_t dy_count;
  size_t dx_count;
  size_t w_bytes;
  size_t dy_bytes;
  size_t dx_bytes;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;

  (void)workSpace;
  (void)workSpaceSizeInBytes;

  if (
      handle == 0 ||
      alpha == 0 ||
      wDesc == 0 ||
      w == 0 ||
      dyDesc == 0 ||
      dy == 0 ||
      convDesc == 0 ||
      beta == 0 ||
      dxDesc == 0 ||
      dx == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (algo != CUDNN_CONVOLUTION_BWD_DATA_ALGO_1) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  dy_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)dyDesc);
  dx_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)dxDesc);
  w_descriptor = psyche_cudnn_find_filter_locked((cudnnFilterDescriptor_t)wDesc);
  conv_descriptor = psyche_cudnn_find_convolution_locked((cudnnConvolutionDescriptor_t)convDesc);
  if (
      context != 0 &&
      dy_descriptor != 0 &&
      dx_descriptor != 0 &&
      w_descriptor != 0 &&
      conv_descriptor != 0) {
    dy_snapshot = *dy_descriptor;
    dx_snapshot = *dx_descriptor;
    w_snapshot = *w_descriptor;
    conv_snapshot = *conv_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (dy_descriptor == 0 || dx_descriptor == 0 || w_descriptor == 0 || conv_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!dy_snapshot.is_set || !dx_snapshot.is_set || !w_snapshot.is_set || !conv_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  validation_status = psyche_cudnn_validate_convolution_backward_data_config(
      &w_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dx_snapshot,
      algo);
  if (validation_status != CUDNN_STATUS_SUCCESS) {
    return validation_status;
  }

  w_count = w_snapshot.element_count;
  dy_count = dy_snapshot.element_count;
  dx_count = dx_snapshot.element_count;
  if (
      w_count == 0 ||
      dy_count == 0 ||
      dx_count == 0 ||
      w_count > UINT_MAX ||
      dy_count > UINT_MAX ||
      dx_count > UINT_MAX ||
      w_count > SIZE_MAX / sizeof(float) ||
      dy_count > SIZE_MAX / sizeof(float) ||
      dx_count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  w_bytes = w_count * sizeof(float);
  dy_bytes = dy_count * sizeof(float);
  dx_bytes = dx_count * sizeof(float);
  if (
      psyche_cudnn_ranges_overlap_sized(w, w_bytes, dy, dy_bytes) ||
      psyche_cudnn_ranges_overlap_sized(w, w_bytes, dx, dx_bytes) ||
      psyche_cudnn_ranges_overlap_sized(dy, dy_bytes, dx, dx_bytes)) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  grid_dim = (unsigned int)((dx_count + PSYCHE_CUDNN_BLOCK_THREADS - 1U) / PSYCHE_CUDNN_BLOCK_THREADS);

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    CUresult metal_result = psyche_cuda_metal_launch_cudnn_convolution_backward_data_f32(
        (const float *)w,
        (const float *)dy,
        (const float *)dx,
        (float *)dx,
        alpha_value,
        beta_value,
        (unsigned int)conv_snapshot.mode,
        (unsigned int)conv_snapshot.group_count,
        (unsigned int)dx_snapshot.dim[0],
        (unsigned int)dx_snapshot.dim[1],
        (unsigned int)dx_snapshot.dim[2],
        (unsigned int)dx_snapshot.dim[3],
        (unsigned int)dy_snapshot.dim[1],
        (unsigned int)dy_snapshot.dim[2],
        (unsigned int)dy_snapshot.dim[3],
        (unsigned int)w_snapshot.h,
        (unsigned int)w_snapshot.w,
        (unsigned int)conv_snapshot.pad_h,
        (unsigned int)conv_snapshot.pad_w,
        (unsigned int)conv_snapshot.stride_h,
        (unsigned int)conv_snapshot.stride_w,
        (unsigned int)conv_snapshot.dilation_h,
        (unsigned int)conv_snapshot.dilation_w,
        (unsigned int)dx_count,
        w_bytes,
        dy_bytes,
        dx_bytes,
        grid_dim,
        PSYCHE_CUDNN_BLOCK_THREADS);
    if (metal_result == CUDA_SUCCESS) {
      return CUDNN_STATUS_SUCCESS;
    }
    if (
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  psyche_cudnn_cpu_convolution_backward_data(
      (const float *)w,
      (const float *)dy,
      (const float *)dx,
      (float *)dx,
      &w_snapshot,
      &dy_snapshot,
      &dx_snapshot,
      &conv_snapshot,
      alpha_value,
      beta_value);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnConvolutionBackwardFilter(
    cudnnHandle_t handle,
    const void *alpha,
    const cudnnTensorDescriptor_t xDesc,
    const void *x,
    const cudnnTensorDescriptor_t dyDesc,
    const void *dy,
    const cudnnConvolutionDescriptor_t convDesc,
    cudnnConvolutionBwdFilterAlgo_t algo,
    void *workSpace,
    size_t workSpaceSizeInBytes,
    const void *beta,
    const cudnnFilterDescriptor_t dwDesc,
    void *dw) {
  struct cudnnContext *context;
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnFilterStruct dw_snapshot;
  struct cudnnConvolutionStruct conv_snapshot;
  struct cudnnTensorStruct *x_descriptor;
  struct cudnnTensorStruct *dy_descriptor;
  struct cudnnFilterStruct *dw_descriptor;
  struct cudnnConvolutionStruct *conv_descriptor;
  cudnnStatus_t validation_status;
  size_t x_count;
  size_t dy_count;
  size_t dw_count;
  size_t x_bytes;
  size_t dy_bytes;
  size_t dw_bytes;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;

  (void)workSpace;
  (void)workSpaceSizeInBytes;

  if (
      handle == 0 ||
      alpha == 0 ||
      xDesc == 0 ||
      x == 0 ||
      dyDesc == 0 ||
      dy == 0 ||
      convDesc == 0 ||
      beta == 0 ||
      dwDesc == 0 ||
      dw == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (algo != CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  dy_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)dyDesc);
  dw_descriptor = psyche_cudnn_find_filter_locked((cudnnFilterDescriptor_t)dwDesc);
  conv_descriptor = psyche_cudnn_find_convolution_locked((cudnnConvolutionDescriptor_t)convDesc);
  if (
      context != 0 &&
      x_descriptor != 0 &&
      dy_descriptor != 0 &&
      dw_descriptor != 0 &&
      conv_descriptor != 0) {
    x_snapshot = *x_descriptor;
    dy_snapshot = *dy_descriptor;
    dw_snapshot = *dw_descriptor;
    conv_snapshot = *conv_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (x_descriptor == 0 || dy_descriptor == 0 || dw_descriptor == 0 || conv_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!x_snapshot.is_set || !dy_snapshot.is_set || !dw_snapshot.is_set || !conv_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  validation_status = psyche_cudnn_validate_convolution_backward_filter_config(
      &x_snapshot,
      &dy_snapshot,
      &conv_snapshot,
      &dw_snapshot,
      algo);
  if (validation_status != CUDNN_STATUS_SUCCESS) {
    return validation_status;
  }

  x_count = x_snapshot.element_count;
  dy_count = dy_snapshot.element_count;
  dw_count = dw_snapshot.element_count;
  if (
      x_count == 0 ||
      dy_count == 0 ||
      dw_count == 0 ||
      x_count > UINT_MAX ||
      dy_count > UINT_MAX ||
      dw_count > UINT_MAX ||
      x_count > SIZE_MAX / sizeof(float) ||
      dy_count > SIZE_MAX / sizeof(float) ||
      dw_count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  x_bytes = x_count * sizeof(float);
  dy_bytes = dy_count * sizeof(float);
  dw_bytes = dw_count * sizeof(float);
  if (
      psyche_cudnn_ranges_overlap_sized(x, x_bytes, dy, dy_bytes) ||
      psyche_cudnn_ranges_overlap_sized(x, x_bytes, dw, dw_bytes) ||
      psyche_cudnn_ranges_overlap_sized(dy, dy_bytes, dw, dw_bytes)) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  grid_dim = (unsigned int)((dw_count + PSYCHE_CUDNN_BLOCK_THREADS - 1U) / PSYCHE_CUDNN_BLOCK_THREADS);

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    CUresult metal_result = psyche_cuda_metal_launch_cudnn_convolution_backward_filter_f32(
        (const float *)x,
        (const float *)dy,
        (const float *)dw,
        (float *)dw,
        alpha_value,
        beta_value,
        (unsigned int)conv_snapshot.mode,
        (unsigned int)conv_snapshot.group_count,
        (unsigned int)x_snapshot.dim[0],
        (unsigned int)x_snapshot.dim[1],
        (unsigned int)x_snapshot.dim[2],
        (unsigned int)x_snapshot.dim[3],
        (unsigned int)dy_snapshot.dim[1],
        (unsigned int)dy_snapshot.dim[2],
        (unsigned int)dy_snapshot.dim[3],
        (unsigned int)dw_snapshot.h,
        (unsigned int)dw_snapshot.w,
        (unsigned int)conv_snapshot.pad_h,
        (unsigned int)conv_snapshot.pad_w,
        (unsigned int)conv_snapshot.stride_h,
        (unsigned int)conv_snapshot.stride_w,
        (unsigned int)conv_snapshot.dilation_h,
        (unsigned int)conv_snapshot.dilation_w,
        (unsigned int)dw_count,
        x_bytes,
        dy_bytes,
        dw_bytes,
        grid_dim,
        PSYCHE_CUDNN_BLOCK_THREADS);
    if (metal_result == CUDA_SUCCESS) {
      return CUDNN_STATUS_SUCCESS;
    }
    if (
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  psyche_cudnn_cpu_convolution_backward_filter(
      (const float *)x,
      (const float *)dy,
      (const float *)dw,
      (float *)dw,
      &x_snapshot,
      &dy_snapshot,
      &dw_snapshot,
      &conv_snapshot,
      alpha_value,
      beta_value);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnTransformTensor(
    cudnnHandle_t handle,
    const void *alpha,
    const cudnnTensorDescriptor_t xDesc,
    const void *x,
    const void *beta,
    const cudnnTensorDescriptor_t yDesc,
    void *y) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *x_descriptor;
  struct cudnnTensorStruct *y_descriptor;
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct y_snapshot;
  size_t count;
  size_t bytes;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;
  int descriptors_equal;

  if (
      handle == 0 ||
      alpha == 0 ||
      xDesc == 0 ||
      x == 0 ||
      beta == 0 ||
      yDesc == 0 ||
      y == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  if (context != 0 && x_descriptor != 0 && y_descriptor != 0) {
    x_snapshot = *x_descriptor;
    y_snapshot = *y_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (x_descriptor == 0 || y_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!x_snapshot.is_set || !y_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      x_snapshot.data_type != CUDNN_DATA_FLOAT ||
      y_snapshot.data_type != CUDNN_DATA_FLOAT ||
      x_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      y_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(&x_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&y_snapshot)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cudnn_tensor_dims_equal(&x_snapshot, &y_snapshot)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  descriptors_equal = psyche_cudnn_tensor_values_equal(&x_snapshot, &y_snapshot);
  if (!descriptors_equal) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  count = x_snapshot.element_count;
  if (count == 0 || count > UINT_MAX || count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  bytes = count * sizeof(float);
  if (psyche_cudnn_ranges_overlap(x, y, bytes)) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  grid_dim = (unsigned int)((count + PSYCHE_CUDNN_BLOCK_THREADS - 1U) / PSYCHE_CUDNN_BLOCK_THREADS);

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    CUresult metal_result = psyche_cuda_metal_launch_cudnn_transform_tensor_f32(
        (const float *)x,
        (const float *)y,
        (float *)y,
        alpha_value,
        beta_value,
        (unsigned int)count,
        bytes,
        grid_dim,
        PSYCHE_CUDNN_BLOCK_THREADS);
    if (metal_result == CUDA_SUCCESS) {
      return CUDNN_STATUS_SUCCESS;
    }
    if (
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  psyche_cudnn_cpu_transform_tensor(
      (const float *)x,
      (const float *)y,
      (float *)y,
      count,
      alpha_value,
      beta_value);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnSoftmaxForward(
    cudnnHandle_t handle,
    cudnnSoftmaxAlgorithm_t algorithm,
    cudnnSoftmaxMode_t mode,
    const void *alpha,
    const cudnnTensorDescriptor_t xDesc,
    const void *x,
    const void *beta,
    const cudnnTensorDescriptor_t yDesc,
    void *y) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *x_descriptor;
  struct cudnnTensorStruct *y_descriptor;
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct y_snapshot;
  size_t count;
  size_t bytes;
  size_t vector_count;
  size_t vector_len;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;
  int descriptors_equal;
  int exact_alias;
  float *inplace_snapshot = 0;
  const float *x_source;
  const float *y_source;

  if (
      handle == 0 ||
      alpha == 0 ||
      xDesc == 0 ||
      x == 0 ||
      beta == 0 ||
      yDesc == 0 ||
      y == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  switch (algorithm) {
  case CUDNN_SOFTMAX_FAST:
  case CUDNN_SOFTMAX_ACCURATE:
  case CUDNN_SOFTMAX_LOG:
    break;
  default:
    return CUDNN_STATUS_BAD_PARAM;
  }
  switch (mode) {
  case CUDNN_SOFTMAX_MODE_INSTANCE:
  case CUDNN_SOFTMAX_MODE_CHANNEL:
    break;
  default:
    return CUDNN_STATUS_BAD_PARAM;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  if (context != 0 && x_descriptor != 0 && y_descriptor != 0) {
    x_snapshot = *x_descriptor;
    y_snapshot = *y_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (x_descriptor == 0 || y_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!x_snapshot.is_set || !y_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      x_snapshot.data_type != CUDNN_DATA_FLOAT ||
      y_snapshot.data_type != CUDNN_DATA_FLOAT ||
      x_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      y_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(&x_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&y_snapshot)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  descriptors_equal = psyche_cudnn_tensor_values_equal(&x_snapshot, &y_snapshot);
  if (!descriptors_equal) {
    if (!psyche_cudnn_tensor_dims_equal(&x_snapshot, &y_snapshot)) {
      return CUDNN_STATUS_BAD_PARAM;
    }
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cudnn_softmax_shape(&x_snapshot, mode, &vector_count, &vector_len)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  count = x_snapshot.element_count;
  if (
      count == 0 ||
      count > UINT_MAX ||
      count > SIZE_MAX / sizeof(float) ||
      vector_count == 0 ||
      vector_len == 0) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  bytes = count * sizeof(float);
  exact_alias = x == y;
  if (psyche_cudnn_ranges_overlap(x, y, bytes)) {
    if (!exact_alias || !descriptors_equal) {
      return CUDNN_STATUS_BAD_PARAM;
    }
  }

  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    if (vector_count <= UINT_MAX && vector_len <= UINT_MAX) {
      CUresult metal_result;
      grid_dim = (unsigned int)vector_count;
      metal_result = psyche_cuda_metal_launch_cudnn_softmax_f32(
          (const float *)x,
          (const float *)y,
          (float *)y,
          alpha_value,
          beta_value,
          (unsigned int)algorithm,
          (unsigned int)mode,
          (unsigned int)x_snapshot.dim[0],
          (unsigned int)x_snapshot.dim[1],
          (unsigned int)x_snapshot.dim[2],
          (unsigned int)x_snapshot.dim[3],
          (unsigned int)vector_count,
          (unsigned int)vector_len,
          bytes,
          grid_dim,
          PSYCHE_CUDNN_BLOCK_THREADS);
      if (metal_result == CUDA_SUCCESS) {
        return CUDNN_STATUS_SUCCESS;
      }
      if (
          psyche_cudnn_metal_required() ||
          !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
        return psyche_cudnn_status_from_cuda_result(metal_result);
      }
    } else if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  x_source = (const float *)x;
  y_source = (const float *)y;
  if (exact_alias) {
    inplace_snapshot = (float *)malloc(bytes);
    if (inplace_snapshot == 0) {
      return CUDNN_STATUS_ALLOC_FAILED;
    }
    memcpy(inplace_snapshot, x, bytes);
    x_source = inplace_snapshot;
    y_source = inplace_snapshot;
  }
  psyche_cudnn_cpu_softmax_forward(
      x_source,
      y_source,
      (float *)y,
      alpha_value,
      beta_value,
      &x_snapshot,
      algorithm,
      mode,
      vector_count,
      vector_len);
  free(inplace_snapshot);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnSoftmaxBackward(
    cudnnHandle_t handle,
    cudnnSoftmaxAlgorithm_t algorithm,
    cudnnSoftmaxMode_t mode,
    const void *alpha,
    const cudnnTensorDescriptor_t yDesc,
    const void *y,
    const cudnnTensorDescriptor_t dyDesc,
    const void *dy,
    const void *beta,
    const cudnnTensorDescriptor_t dxDesc,
    void *dx) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *y_descriptor;
  struct cudnnTensorStruct *dy_descriptor;
  struct cudnnTensorStruct *dx_descriptor;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnTensorStruct dx_snapshot;
  size_t count;
  size_t bytes;
  size_t vector_count;
  size_t vector_len;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;
  int descriptors_equal;
  int exact_dy_dx_alias;
  float *dy_dx_snapshot = 0;
  const float *dy_source;
  const float *dx_source;

  if (
      handle == 0 ||
      alpha == 0 ||
      yDesc == 0 ||
      y == 0 ||
      dyDesc == 0 ||
      dy == 0 ||
      beta == 0 ||
      dxDesc == 0 ||
      dx == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  switch (algorithm) {
  case CUDNN_SOFTMAX_FAST:
  case CUDNN_SOFTMAX_ACCURATE:
  case CUDNN_SOFTMAX_LOG:
    break;
  default:
    return CUDNN_STATUS_BAD_PARAM;
  }
  switch (mode) {
  case CUDNN_SOFTMAX_MODE_INSTANCE:
  case CUDNN_SOFTMAX_MODE_CHANNEL:
    break;
  default:
    return CUDNN_STATUS_BAD_PARAM;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  dy_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)dyDesc);
  dx_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)dxDesc);
  if (context != 0 && y_descriptor != 0 && dy_descriptor != 0 && dx_descriptor != 0) {
    y_snapshot = *y_descriptor;
    dy_snapshot = *dy_descriptor;
    dx_snapshot = *dx_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (y_descriptor == 0 || dy_descriptor == 0 || dx_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!y_snapshot.is_set || !dy_snapshot.is_set || !dx_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      y_snapshot.data_type != CUDNN_DATA_FLOAT ||
      dy_snapshot.data_type != CUDNN_DATA_FLOAT ||
      dx_snapshot.data_type != CUDNN_DATA_FLOAT ||
      y_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      dy_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      dx_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(&y_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&dy_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&dx_snapshot)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_dims_equal(&y_snapshot, &dy_snapshot) ||
      !psyche_cudnn_tensor_dims_equal(&y_snapshot, &dx_snapshot)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  descriptors_equal =
      psyche_cudnn_tensor_values_equal(&y_snapshot, &dy_snapshot) &&
      psyche_cudnn_tensor_values_equal(&y_snapshot, &dx_snapshot);
  if (!descriptors_equal) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (!psyche_cudnn_softmax_shape(&y_snapshot, mode, &vector_count, &vector_len)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  count = y_snapshot.element_count;
  if (
      count == 0 ||
      count > UINT_MAX ||
      count > SIZE_MAX / sizeof(float) ||
      vector_count == 0 ||
      vector_len == 0) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  bytes = count * sizeof(float);
  exact_dy_dx_alias = dy == dx;
  if (psyche_cudnn_ranges_overlap(dy, dx, bytes) && !exact_dy_dx_alias) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (psyche_cudnn_ranges_overlap(y, dx, bytes)) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    if (vector_count <= UINT_MAX && vector_len <= UINT_MAX) {
      CUresult metal_result;
      grid_dim = (unsigned int)vector_count;
      metal_result = psyche_cuda_metal_launch_cudnn_softmax_backward_f32(
          (const float *)y,
          (const float *)dy,
          (const float *)dx,
          (float *)dx,
          alpha_value,
          beta_value,
          (unsigned int)algorithm,
          (unsigned int)mode,
          (unsigned int)y_snapshot.dim[0],
          (unsigned int)y_snapshot.dim[1],
          (unsigned int)y_snapshot.dim[2],
          (unsigned int)y_snapshot.dim[3],
          (unsigned int)vector_count,
          (unsigned int)vector_len,
          bytes,
          grid_dim,
          PSYCHE_CUDNN_BLOCK_THREADS);
      if (metal_result == CUDA_SUCCESS) {
        return CUDNN_STATUS_SUCCESS;
      }
      if (
          psyche_cudnn_metal_required() ||
          !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
        return psyche_cudnn_status_from_cuda_result(metal_result);
      }
    } else if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  dy_source = (const float *)dy;
  dx_source = (const float *)dx;
  if (exact_dy_dx_alias) {
    dy_dx_snapshot = (float *)malloc(bytes);
    if (dy_dx_snapshot == 0) {
      return CUDNN_STATUS_ALLOC_FAILED;
    }
    memcpy(dy_dx_snapshot, dy, bytes);
    dy_source = dy_dx_snapshot;
    dx_source = dy_dx_snapshot;
  }
  psyche_cudnn_cpu_softmax_backward(
      (const float *)y,
      dy_source,
      dx_source,
      (float *)dx,
      alpha_value,
      beta_value,
      &y_snapshot,
      algorithm,
      mode,
      vector_count,
      vector_len);
  free(dy_dx_snapshot);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnPoolingForward(
    cudnnHandle_t handle,
    const cudnnPoolingDescriptor_t poolingDesc,
    const void *alpha,
    const cudnnTensorDescriptor_t xDesc,
    const void *x,
    const void *beta,
    const cudnnTensorDescriptor_t yDesc,
    void *y) {
  struct cudnnContext *context;
  struct cudnnTensorStruct *x_descriptor;
  struct cudnnTensorStruct *y_descriptor;
  struct cudnnPoolingStruct *pooling_descriptor;
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnPoolingStruct pooling_snapshot;
  size_t x_count;
  size_t y_count;
  size_t x_bytes;
  size_t y_bytes;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;
  int output_h;
  int output_w;
  int descriptors_equal;
  int exact_alias;
  float *inplace_snapshot = 0;
  const float *x_source;
  const float *y_source;

  if (
      handle == 0 ||
      poolingDesc == 0 ||
      alpha == 0 ||
      xDesc == 0 ||
      x == 0 ||
      beta == 0 ||
      yDesc == 0 ||
      y == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  pooling_descriptor = psyche_cudnn_find_pooling_locked((cudnnPoolingDescriptor_t)poolingDesc);
  x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  if (
      context != 0 &&
      pooling_descriptor != 0 &&
      x_descriptor != 0 &&
      y_descriptor != 0) {
    x_snapshot = *x_descriptor;
    y_snapshot = *y_descriptor;
    pooling_snapshot = *pooling_descriptor;
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (pooling_descriptor == 0 || x_descriptor == 0 || y_descriptor == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!pooling_snapshot.is_set || !x_snapshot.is_set || !y_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      x_snapshot.data_type != CUDNN_DATA_FLOAT ||
      y_snapshot.data_type != CUDNN_DATA_FLOAT ||
      x_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      y_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(&x_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&y_snapshot)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      pooling_snapshot.mode != CUDNN_POOLING_MAX &&
      pooling_snapshot.mode != CUDNN_POOLING_MAX_DETERMINISTIC &&
      pooling_snapshot.mode != CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING &&
      pooling_snapshot.mode != CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_pooling_output_dim(
          x_snapshot.dim[2],
          pooling_snapshot.pad_h,
          pooling_snapshot.window_h,
          pooling_snapshot.stride_h,
          &output_h) ||
      !psyche_cudnn_pooling_output_dim(
          x_snapshot.dim[3],
          pooling_snapshot.pad_w,
          pooling_snapshot.window_w,
          pooling_snapshot.stride_w,
          &output_w)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      y_snapshot.dim[0] != x_snapshot.dim[0] ||
      y_snapshot.dim[1] != x_snapshot.dim[1] ||
      y_snapshot.dim[2] != output_h ||
      y_snapshot.dim[3] != output_w) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  x_count = x_snapshot.element_count;
  y_count = y_snapshot.element_count;
  if (
      x_count == 0 ||
      y_count == 0 ||
      x_count > UINT_MAX ||
      y_count > UINT_MAX ||
      x_count > SIZE_MAX / sizeof(float) ||
      y_count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  x_bytes = x_count * sizeof(float);
  y_bytes = y_count * sizeof(float);
  descriptors_equal = psyche_cudnn_tensor_values_equal(&x_snapshot, &y_snapshot);
  exact_alias = x == y;
  if (psyche_cudnn_ranges_overlap_sized(x, x_bytes, y, y_bytes)) {
    if (!exact_alias || !descriptors_equal) {
      return CUDNN_STATUS_BAD_PARAM;
    }
  }

  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  grid_dim = (unsigned int)((y_count + PSYCHE_CUDNN_BLOCK_THREADS - 1U) / PSYCHE_CUDNN_BLOCK_THREADS);

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    CUresult metal_result = psyche_cuda_metal_launch_cudnn_pooling_f32(
        (const float *)x,
        (const float *)y,
        (float *)y,
        alpha_value,
        beta_value,
        (unsigned int)pooling_snapshot.mode,
        (unsigned int)pooling_snapshot.nan_opt,
        (unsigned int)x_snapshot.dim[0],
        (unsigned int)x_snapshot.dim[1],
        (unsigned int)x_snapshot.dim[2],
        (unsigned int)x_snapshot.dim[3],
        (unsigned int)y_snapshot.dim[2],
        (unsigned int)y_snapshot.dim[3],
        (unsigned int)pooling_snapshot.window_h,
        (unsigned int)pooling_snapshot.window_w,
        (unsigned int)pooling_snapshot.pad_h,
        (unsigned int)pooling_snapshot.pad_w,
        (unsigned int)pooling_snapshot.stride_h,
        (unsigned int)pooling_snapshot.stride_w,
        (unsigned int)y_count,
        x_bytes,
        y_bytes,
        grid_dim,
        PSYCHE_CUDNN_BLOCK_THREADS);
    if (metal_result == CUDA_SUCCESS) {
      return CUDNN_STATUS_SUCCESS;
    }
    if (
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  x_source = (const float *)x;
  y_source = (const float *)y;
  if (exact_alias) {
    inplace_snapshot = (float *)malloc(x_bytes);
    if (inplace_snapshot == 0) {
      return CUDNN_STATUS_ALLOC_FAILED;
    }
    memcpy(inplace_snapshot, x, x_bytes);
    x_source = inplace_snapshot;
    y_source = inplace_snapshot;
  }
  psyche_cudnn_cpu_pooling_forward(
      x_source,
      y_source,
      (float *)y,
      alpha_value,
      beta_value,
      &x_snapshot,
      &y_snapshot,
      &pooling_snapshot);
  free(inplace_snapshot);
  return CUDNN_STATUS_SUCCESS;
}

PSYCHE_CUDA_STUB_API cudnnStatus_t cudnnPoolingBackward(
    cudnnHandle_t handle,
    const cudnnPoolingDescriptor_t poolingDesc,
    const void *alpha,
    const cudnnTensorDescriptor_t yDesc,
    const void *y,
    const cudnnTensorDescriptor_t dyDesc,
    const void *dy,
    const cudnnTensorDescriptor_t xDesc,
    const void *x,
    const void *beta,
    const cudnnTensorDescriptor_t dxDesc,
    void *dx) {
  struct cudnnContext *context;
  struct cudnnPoolingStruct *pooling_descriptor;
  struct cudnnTensorStruct *y_descriptor = 0;
  struct cudnnTensorStruct *dy_descriptor;
  struct cudnnTensorStruct *x_descriptor = 0;
  struct cudnnTensorStruct *dx_descriptor;
  struct cudnnPoolingStruct pooling_snapshot;
  struct cudnnTensorStruct y_snapshot;
  struct cudnnTensorStruct dy_snapshot;
  struct cudnnTensorStruct x_snapshot;
  struct cudnnTensorStruct dx_snapshot;
  size_t dy_count;
  size_t dx_count;
  size_t dy_bytes;
  size_t dx_bytes;
  float alpha_value;
  float beta_value;
  unsigned int grid_dim;
  int output_h;
  int output_w;
  int is_average;
  int exact_dy_dx_alias;
  float *dy_dx_snapshot = 0;
  const float *dy_source;
  const float *dx_source;

  if (
      handle == 0 ||
      poolingDesc == 0 ||
      alpha == 0 ||
      dyDesc == 0 ||
      dy == 0 ||
      beta == 0 ||
      dxDesc == 0 ||
      dx == 0) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  pthread_mutex_lock(&psyche_cudnn_mutex);
  context = psyche_cudnn_find_context_locked(handle);
  pooling_descriptor = psyche_cudnn_find_pooling_locked((cudnnPoolingDescriptor_t)poolingDesc);
  dy_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)dyDesc);
  dx_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)dxDesc);
  if (yDesc != 0) {
    y_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)yDesc);
  }
  if (xDesc != 0) {
    x_descriptor = psyche_cudnn_find_tensor_locked((cudnnTensorDescriptor_t)xDesc);
  }
  if (
      context != 0 &&
      pooling_descriptor != 0 &&
      dy_descriptor != 0 &&
      dx_descriptor != 0 &&
      (yDesc == 0 || y_descriptor != 0) &&
      (xDesc == 0 || x_descriptor != 0)) {
    pooling_snapshot = *pooling_descriptor;
    dy_snapshot = *dy_descriptor;
    dx_snapshot = *dx_descriptor;
    if (y_descriptor != 0) {
      y_snapshot = *y_descriptor;
    }
    if (x_descriptor != 0) {
      x_snapshot = *x_descriptor;
    }
  }
  pthread_mutex_unlock(&psyche_cudnn_mutex);

  if (context == 0) {
    return CUDNN_STATUS_NOT_INITIALIZED;
  }
  if (
      pooling_descriptor == 0 ||
      dy_descriptor == 0 ||
      dx_descriptor == 0 ||
      (yDesc != 0 && y_descriptor == 0) ||
      (xDesc != 0 && x_descriptor == 0)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (!pooling_snapshot.is_set || !dy_snapshot.is_set || !dx_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      pooling_snapshot.mode != CUDNN_POOLING_MAX &&
      pooling_snapshot.mode != CUDNN_POOLING_MAX_DETERMINISTIC &&
      pooling_snapshot.mode != CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING &&
      pooling_snapshot.mode != CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  is_average = psyche_cudnn_pooling_mode_is_average(pooling_snapshot.mode);
  if (!is_average && (yDesc == 0 || y == 0 || xDesc == 0 || x == 0)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (is_average) {
    if (yDesc != 0 && !y_snapshot.is_set) {
      return CUDNN_STATUS_BAD_PARAM;
    }
    if (xDesc != 0 && !x_snapshot.is_set) {
      return CUDNN_STATUS_BAD_PARAM;
    }
  } else if (!y_snapshot.is_set || !x_snapshot.is_set) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      dy_snapshot.data_type != CUDNN_DATA_FLOAT ||
      dx_snapshot.data_type != CUDNN_DATA_FLOAT ||
      dy_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
      dx_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_tensor_is_contiguous_nchw(&dy_snapshot) ||
      !psyche_cudnn_tensor_is_contiguous_nchw(&dx_snapshot)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !is_average &&
      (y_snapshot.data_type != CUDNN_DATA_FLOAT ||
       x_snapshot.data_type != CUDNN_DATA_FLOAT ||
       y_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS ||
       x_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !is_average &&
      (!psyche_cudnn_tensor_is_contiguous_nchw(&y_snapshot) ||
       !psyche_cudnn_tensor_is_contiguous_nchw(&x_snapshot))) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      is_average &&
      ((yDesc != 0 &&
        (y_snapshot.data_type != CUDNN_DATA_FLOAT ||
         y_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS)) ||
       (xDesc != 0 &&
        (x_snapshot.data_type != CUDNN_DATA_FLOAT ||
         x_snapshot.nb_dims != PSYCHE_CUDNN_MAX_TENSOR_DIMS)))) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      is_average &&
      ((yDesc != 0 && !psyche_cudnn_tensor_is_contiguous_nchw(&y_snapshot)) ||
       (xDesc != 0 && !psyche_cudnn_tensor_is_contiguous_nchw(&x_snapshot)))) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  if (
      !psyche_cudnn_pooling_output_dim(
          dx_snapshot.dim[2],
          pooling_snapshot.pad_h,
          pooling_snapshot.window_h,
          pooling_snapshot.stride_h,
          &output_h) ||
      !psyche_cudnn_pooling_output_dim(
          dx_snapshot.dim[3],
          pooling_snapshot.pad_w,
          pooling_snapshot.window_w,
          pooling_snapshot.stride_w,
          &output_w)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (
      dy_snapshot.dim[0] != dx_snapshot.dim[0] ||
      dy_snapshot.dim[1] != dx_snapshot.dim[1] ||
      dy_snapshot.dim[2] != output_h ||
      dy_snapshot.dim[3] != output_w) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (yDesc != 0 && !psyche_cudnn_tensor_values_equal(&y_snapshot, &dy_snapshot)) {
    return CUDNN_STATUS_BAD_PARAM;
  }
  if (xDesc != 0 && !psyche_cudnn_tensor_values_equal(&x_snapshot, &dx_snapshot)) {
    return CUDNN_STATUS_BAD_PARAM;
  }

  dy_count = dy_snapshot.element_count;
  dx_count = dx_snapshot.element_count;
  if (
      dy_count == 0 ||
      dx_count == 0 ||
      dy_count > UINT_MAX ||
      dx_count > UINT_MAX ||
      dy_count > SIZE_MAX / sizeof(float) ||
      dx_count > SIZE_MAX / sizeof(float)) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }
  dy_bytes = dy_count * sizeof(float);
  dx_bytes = dx_count * sizeof(float);
  exact_dy_dx_alias = dy == dx;
  if (psyche_cudnn_ranges_overlap_sized(dy, dy_bytes, dx, dx_bytes)) {
    if (!exact_dy_dx_alias || !psyche_cudnn_tensor_values_equal(&dy_snapshot, &dx_snapshot)) {
      return CUDNN_STATUS_BAD_PARAM;
    }
  }
  if (!is_average) {
    if (
        psyche_cudnn_ranges_overlap_sized(x, dx_bytes, dx, dx_bytes) ||
        psyche_cudnn_ranges_overlap_sized(y, dy_bytes, dx, dx_bytes)) {
      return CUDNN_STATUS_BAD_PARAM;
    }
  }

  alpha_value = *(const float *)alpha;
  beta_value = *(const float *)beta;
  grid_dim = (unsigned int)((dx_count + PSYCHE_CUDNN_BLOCK_THREADS - 1U) / PSYCHE_CUDNN_BLOCK_THREADS);

  if (psyche_cudnn_metal_enabled()) {
#if defined(__APPLE__)
    CUresult metal_result = psyche_cuda_metal_launch_cudnn_pooling_backward_f32(
        (const float *)x,
        (const float *)dy,
        (const float *)dx,
        (float *)dx,
        alpha_value,
        beta_value,
        (unsigned int)pooling_snapshot.mode,
        (unsigned int)pooling_snapshot.nan_opt,
        (unsigned int)dx_snapshot.dim[0],
        (unsigned int)dx_snapshot.dim[1],
        (unsigned int)dx_snapshot.dim[2],
        (unsigned int)dx_snapshot.dim[3],
        (unsigned int)dy_snapshot.dim[2],
        (unsigned int)dy_snapshot.dim[3],
        (unsigned int)pooling_snapshot.window_h,
        (unsigned int)pooling_snapshot.window_w,
        (unsigned int)pooling_snapshot.pad_h,
        (unsigned int)pooling_snapshot.pad_w,
        (unsigned int)pooling_snapshot.stride_h,
        (unsigned int)pooling_snapshot.stride_w,
        (unsigned int)dx_count,
        dy_bytes,
        dx_bytes,
        grid_dim,
        PSYCHE_CUDNN_BLOCK_THREADS);
    if (metal_result == CUDA_SUCCESS) {
      return CUDNN_STATUS_SUCCESS;
    }
    if (
        psyche_cudnn_metal_required() ||
        !psyche_cudnn_metal_preferred_can_fallback(metal_result)) {
      return psyche_cudnn_status_from_cuda_result(metal_result);
    }
#else
    if (psyche_cudnn_metal_required()) {
      return CUDNN_STATUS_NOT_SUPPORTED;
    }
#endif
  } else if (psyche_cudnn_metal_required()) {
    return CUDNN_STATUS_NOT_SUPPORTED;
  }

  dy_source = (const float *)dy;
  dx_source = (const float *)dx;
  if (exact_dy_dx_alias) {
    dy_dx_snapshot = (float *)malloc(dx_bytes);
    if (dy_dx_snapshot == 0) {
      return CUDNN_STATUS_ALLOC_FAILED;
    }
    memcpy(dy_dx_snapshot, dx, dx_bytes);
    dy_source = dy_dx_snapshot;
    dx_source = dy_dx_snapshot;
  }
  psyche_cudnn_cpu_pooling_backward(
      (const float *)x,
      dy_source,
      dx_source,
      (float *)dx,
      alpha_value,
      beta_value,
      &dy_snapshot,
      &dx_snapshot,
      &pooling_snapshot);
  free(dy_dx_snapshot);
  return CUDNN_STATUS_SUCCESS;
}
