#define _POSIX_C_SOURCE 200809L

#include "cuda_compat_stub.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static _Atomic int psyche_cuda_runtime_last_error = cudaSuccess;

typedef struct PsycheCudaRuntimeAllocation {
  void *ptr;
  size_t size;
  unsigned long long buffer_id;
  cudaMemPool_t pool;
  int managed;
  int async_alloc;
  int read_mostly;
  int preferred_location;
  int accessed_by;
  int last_prefetch_location;
  int sync_memops;
  struct PsycheCudaRuntimeAllocation *next;
} PsycheCudaRuntimeAllocation;

typedef struct PsycheCudaRuntimeHostAllocation {
  void *ptr;
  size_t size;
  unsigned int flags;
  int owns_memory;
  int registered;
  int device_mapped;
  struct PsycheCudaRuntimeHostAllocation *next;
} PsycheCudaRuntimeHostAllocation;

typedef struct PsycheCudaRuntimeStream {
  cudaStream_t handle;
  unsigned int flags;
  int priority;
  struct PsycheCudaRuntimeStream *next;
} PsycheCudaRuntimeStream;

typedef struct PsycheCudaRuntimeEvent {
  cudaEvent_t handle;
  unsigned int flags;
  int recorded;
  struct timespec recorded_at;
  struct PsycheCudaRuntimeEvent *next;
} PsycheCudaRuntimeEvent;

typedef struct PsycheCudaRuntimeMemoryPool {
  cudaMemPool_t handle;
  int is_default;
  cudaMemAllocationType alloc_type;
  cudaMemAllocationHandleType handle_types;
  cudaMemLocation location;
  size_t max_size;
  cuuint64_t release_threshold;
  cuuint64_t reserved_current;
  cuuint64_t reserved_high;
  cuuint64_t used_current;
  cuuint64_t used_high;
  int reuse_follow_event_dependencies;
  int reuse_allow_opportunistic;
  int reuse_allow_internal_dependencies;
  struct PsycheCudaRuntimeMemoryPool *next;
} PsycheCudaRuntimeMemoryPool;

static pthread_mutex_t psyche_cuda_runtime_allocation_mutex = PTHREAD_MUTEX_INITIALIZER;
static PsycheCudaRuntimeAllocation *psyche_cuda_runtime_allocations = 0;
static PsycheCudaRuntimeHostAllocation *psyche_cuda_runtime_host_allocations = 0;
static PsycheCudaRuntimeStream *psyche_cuda_runtime_streams = 0;
static PsycheCudaRuntimeEvent *psyche_cuda_runtime_events = 0;
static PsycheCudaRuntimeMemoryPool *psyche_cuda_runtime_memory_pools = 0;
static PsycheCudaRuntimeMemoryPool psyche_cuda_runtime_default_host_pool;
static PsycheCudaRuntimeMemoryPool psyche_cuda_runtime_default_managed_pool;
static cudaMemPool_t psyche_cuda_runtime_current_host_pool = 0;
static cudaMemPool_t psyche_cuda_runtime_current_managed_pool = 0;
static uintptr_t psyche_cuda_runtime_next_stream_handle = 0xC50000000001ULL;
static uintptr_t psyche_cuda_runtime_next_event_handle = 0xC60000000001ULL;
static unsigned long long psyche_cuda_runtime_next_buffer_id = 1;

typedef cudaError_t (*PsycheCudaRuntimeKernelLaunchFn)(
    dim3 gridDim,
    dim3 blockDim,
    void **args);

typedef struct PsycheCudaRuntimeKernelDescriptor {
  const char *name;
  unsigned int param_count;
  const void *token;
  PsycheCudaRuntimeKernelLaunchFn launch;
} PsycheCudaRuntimeKernelDescriptor;

/*
 * Keep simulated memory validation and the actual read/write/fill/free under
 * the allocation mutex. Moving operations outside the lock would let a
 * concurrent free invalidate CPU-backed simulated memory after validation.
 */

PSYCHE_CUDA_STUB_API const char *psyche_cuda_compat_stub_version(void) {
  return "psyche-cuda-compat-stub/0.1";
}

PSYCHE_CUDA_STUB_API int psyche_cuda_compat_stub_is_stub(void) {
  return 1;
}

PSYCHE_CUDA_STUB_API cudaError_t cudaStreamCreateWithFlags(
    cudaStream_t *pStream,
    unsigned int flags);
PSYCHE_CUDA_STUB_API cudaError_t cudaEventCreateWithFlags(
    cudaEvent_t *event,
    unsigned int flags);
PSYCHE_CUDA_STUB_API cudaError_t cudaEventRecordWithFlags(
    cudaEvent_t event,
    cudaStream_t stream,
    unsigned int flags);
PSYCHE_CUDA_STUB_API cudaError_t cudaEventQuery(cudaEvent_t event);

static cudaError_t psyche_cuda_runtime_launch_vector_add_f32(
    dim3 gridDim,
    dim3 blockDim,
    void **args);
static cudaError_t psyche_cuda_runtime_launch_saxpy_f32(
    dim3 gridDim,
    dim3 blockDim,
    void **args);
static cudaError_t psyche_cuda_runtime_launch_scale_f32(
    dim3 gridDim,
    dim3 blockDim,
    void **args);
static cudaError_t psyche_cuda_runtime_launch_axpby_f32(
    dim3 gridDim,
    dim3 blockDim,
    void **args);

PSYCHE_CUDA_STUB_API void psyche_cuda_runtime_kernel_vector_add_f32(void) {
}

PSYCHE_CUDA_STUB_API void psyche_cuda_runtime_kernel_saxpy_f32(void) {
}

PSYCHE_CUDA_STUB_API void psyche_cuda_runtime_kernel_scale_f32(void) {
}

PSYCHE_CUDA_STUB_API void psyche_cuda_runtime_kernel_axpby_f32(void) {
}

static const PsycheCudaRuntimeKernelDescriptor
    psyche_cuda_runtime_kernel_descriptors[] = {
        {
            "vector_add_f32",
            4,
            (const void *)&psyche_cuda_runtime_kernel_vector_add_f32,
            psyche_cuda_runtime_launch_vector_add_f32
        },
        {
            "saxpy_f32",
            4,
            (const void *)&psyche_cuda_runtime_kernel_saxpy_f32,
            psyche_cuda_runtime_launch_saxpy_f32
        },
        {
            "scale_f32",
            3,
            (const void *)&psyche_cuda_runtime_kernel_scale_f32,
            psyche_cuda_runtime_launch_scale_f32
        },
        {
            "axpby_f32",
            5,
            (const void *)&psyche_cuda_runtime_kernel_axpby_f32,
            psyche_cuda_runtime_launch_axpby_f32
        },
    };

static cudaError_t psyche_cuda_runtime_record(cudaError_t error) {
  atomic_store(&psyche_cuda_runtime_last_error, (int)error);
  return error;
}

static int psyche_cuda_runtime_simulated_memory_enabled(void) {
  const char *value = getenv("PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY");
  return
      value != 0 &&
      (
          strcmp(value, "1") == 0 ||
          strcasecmp(value, "true") == 0 ||
          strcasecmp(value, "yes") == 0 ||
          strcasecmp(value, "on") == 0);
}

static int psyche_cuda_runtime_simulated_sync_enabled(void) {
  return psyche_cuda_runtime_simulated_memory_enabled();
}

static void psyche_cuda_runtime_now(struct timespec *ts) {
  (void)clock_gettime(CLOCK_MONOTONIC, ts);
}

static float psyche_cuda_runtime_elapsed_ms(
    const struct timespec *start,
    const struct timespec *end) {
  double seconds = (double)(end->tv_sec - start->tv_sec);
  double nanoseconds = (double)(end->tv_nsec - start->tv_nsec);
  return (float)(seconds * 1000.0 + nanoseconds / 1000000.0);
}

static PsycheCudaRuntimeStream *psyche_cuda_runtime_find_stream_locked(
    cudaStream_t stream) {
  PsycheCudaRuntimeStream *record = psyche_cuda_runtime_streams;
  while (record != 0) {
    if (record->handle == stream) {
      return record;
    }
    record = record->next;
  }
  return 0;
}

static PsycheCudaRuntimeEvent *psyche_cuda_runtime_find_event_locked(cudaEvent_t event) {
  PsycheCudaRuntimeEvent *record = psyche_cuda_runtime_events;
  while (record != 0) {
    if (record->handle == event) {
      return record;
    }
    record = record->next;
  }
  return 0;
}

static cudaStream_t psyche_cuda_runtime_next_stream_handle_locked(void) {
  cudaStream_t handle = (cudaStream_t)psyche_cuda_runtime_next_stream_handle;
  psyche_cuda_runtime_next_stream_handle += 2;
  return handle;
}

static cudaEvent_t psyche_cuda_runtime_next_event_handle_locked(void) {
  cudaEvent_t handle = (cudaEvent_t)psyche_cuda_runtime_next_event_handle;
  psyche_cuda_runtime_next_event_handle += 2;
  return handle;
}

static cudaError_t psyche_cuda_runtime_validate_stream_simulated(cudaStream_t stream) {
  cudaError_t result = cudaSuccess;
  if (stream == 0) {
    return cudaSuccess;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  if (psyche_cuda_runtime_find_stream_locked(stream) == 0) {
    result = cudaErrorInvalidResourceHandle;
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return result;
}

static cudaError_t psyche_cuda_runtime_validate_async_stream(cudaStream_t stream) {
  if (stream == 0) {
    return cudaSuccess;
  }
  if (!psyche_cuda_runtime_simulated_sync_enabled()) {
    return cudaErrorNoDevice;
  }
  return psyche_cuda_runtime_validate_stream_simulated(stream);
}

static cudaError_t psyche_cuda_runtime_create_stream_simulated(
    cudaStream_t *pStream,
    unsigned int flags,
    int priority) {
  PsycheCudaRuntimeStream *record = 0;
  const unsigned int allowed_flags = cudaStreamNonBlocking;
  if ((flags & ~allowed_flags) != 0) {
    *pStream = 0;
    return cudaErrorInvalidValue;
  }
  record = (PsycheCudaRuntimeStream *)malloc(sizeof(*record));
  if (record == 0) {
    *pStream = 0;
    return cudaErrorMemoryAllocation;
  }
  record->flags = flags;
  (void)priority;
  record->priority = 0;
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  record->handle = psyche_cuda_runtime_next_stream_handle_locked();
  record->next = psyche_cuda_runtime_streams;
  psyche_cuda_runtime_streams = record;
  *pStream = record->handle;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_destroy_stream_simulated(cudaStream_t stream) {
  PsycheCudaRuntimeStream **link = &psyche_cuda_runtime_streams;
  PsycheCudaRuntimeStream *record = 0;
  if (stream == 0) {
    return cudaErrorInvalidResourceHandle;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  while (*link != 0 && (*link)->handle != stream) {
    link = &(*link)->next;
  }
  if (*link == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorInvalidResourceHandle;
  }
  record = *link;
  *link = record->next;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  free(record);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_create_event_simulated(
    cudaEvent_t *event,
    unsigned int flags) {
  PsycheCudaRuntimeEvent *record = 0;
  const unsigned int allowed_flags = cudaEventBlockingSync | cudaEventDisableTiming;
  if ((flags & cudaEventInterprocess) != 0) {
    *event = 0;
    return cudaErrorNotSupported;
  }
  if ((flags & ~allowed_flags) != 0) {
    *event = 0;
    return cudaErrorInvalidValue;
  }
  record = (PsycheCudaRuntimeEvent *)malloc(sizeof(*record));
  if (record == 0) {
    *event = 0;
    return cudaErrorMemoryAllocation;
  }
  record->flags = flags;
  record->recorded = 0;
  record->recorded_at.tv_sec = 0;
  record->recorded_at.tv_nsec = 0;
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  record->handle = psyche_cuda_runtime_next_event_handle_locked();
  record->next = psyche_cuda_runtime_events;
  psyche_cuda_runtime_events = record;
  *event = record->handle;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_destroy_event_simulated(cudaEvent_t event) {
  PsycheCudaRuntimeEvent **link = &psyche_cuda_runtime_events;
  PsycheCudaRuntimeEvent *record = 0;
  if (event == 0) {
    return cudaErrorInvalidResourceHandle;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  while (*link != 0 && (*link)->handle != event) {
    link = &(*link)->next;
  }
  if (*link == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorInvalidResourceHandle;
  }
  record = *link;
  *link = record->next;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  free(record);
  return cudaSuccess;
}

static int psyche_cuda_runtime_add_size_checked(
    size_t left,
    size_t right,
    size_t *sum) {
  if (right > SIZE_MAX - left) {
    return 0;
  }
  *sum = left + right;
  return 1;
}

static int psyche_cuda_runtime_mul_size_checked(
    size_t left,
    size_t right,
    size_t *product) {
  if (right != 0 && left > SIZE_MAX / right) {
    return 0;
  }
  *product = left * right;
  return 1;
}

static int psyche_cuda_runtime_align_up_size_checked(
    size_t value,
    size_t alignment,
    size_t *aligned) {
  size_t remainder = 0;
  size_t increment = 0;
  if (alignment == 0) {
    return 0;
  }
  remainder = value % alignment;
  if (remainder == 0) {
    *aligned = value;
    return 1;
  }
  increment = alignment - remainder;
  return psyche_cuda_runtime_add_size_checked(value, increment, aligned);
}

static int psyche_cuda_runtime_range_contains(
    PsycheCudaRuntimeAllocation *allocation,
    const void *ptr,
    size_t count) {
  uintptr_t base = (uintptr_t)allocation->ptr;
  uintptr_t address = (uintptr_t)ptr;
  uintptr_t allocation_end = 0;
  uintptr_t copy_end = 0;
  if (
      allocation->size > UINTPTR_MAX - base ||
      count > UINTPTR_MAX - address) {
    return 0;
  }
  allocation_end = base + allocation->size;
  copy_end = address + count;
  return address >= base && copy_end <= allocation_end;
}

static int psyche_cuda_runtime_host_range_contains(
    PsycheCudaRuntimeHostAllocation *allocation,
    const void *ptr,
    size_t count) {
  uintptr_t base = (uintptr_t)allocation->ptr;
  uintptr_t address = (uintptr_t)ptr;
  uintptr_t allocation_end = 0;
  uintptr_t copy_end = 0;
  if (
      allocation->size > UINTPTR_MAX - base ||
      count > UINTPTR_MAX - address) {
    return 0;
  }
  allocation_end = base + allocation->size;
  copy_end = address + count;
  return address >= base && copy_end <= allocation_end;
}

static int psyche_cuda_runtime_ranges_overlap(
    const void *first,
    size_t first_size,
    const void *second,
    size_t second_size) {
  uintptr_t first_base = (uintptr_t)first;
  uintptr_t second_base = (uintptr_t)second;
  uintptr_t first_end = 0;
  uintptr_t second_end = 0;
  if (
      first_size == 0 ||
      second_size == 0 ||
      first_size > UINTPTR_MAX - first_base ||
      second_size > UINTPTR_MAX - second_base) {
    return 0;
  }
  first_end = first_base + first_size;
  second_end = second_base + second_size;
  return first_base < second_end && second_base < first_end;
}

static int psyche_cuda_runtime_range_is_valid(const void *ptr, size_t size) {
  uintptr_t base = (uintptr_t)ptr;
  return ptr != 0 && size != 0 && size <= UINTPTR_MAX - base;
}

static PsycheCudaRuntimeAllocation *psyche_cuda_runtime_find_allocation_locked(
    const void *ptr,
    size_t count) {
  PsycheCudaRuntimeAllocation *allocation = psyche_cuda_runtime_allocations;
  while (allocation != 0) {
    if (psyche_cuda_runtime_range_contains(allocation, ptr, count)) {
      return allocation;
    }
    allocation = allocation->next;
  }
  return 0;
}

static const PsycheCudaRuntimeKernelDescriptor *
psyche_cuda_runtime_find_kernel_descriptor(const void *func) {
  size_t index = 0;
  for (
      index = 0;
      index < sizeof(psyche_cuda_runtime_kernel_descriptors) /
          sizeof(psyche_cuda_runtime_kernel_descriptors[0]);
      index++) {
    if (func == psyche_cuda_runtime_kernel_descriptors[index].token) {
      return &psyche_cuda_runtime_kernel_descriptors[index];
    }
  }
  return 0;
}

static int psyche_cuda_runtime_kernel_params_present(
    void **args,
    unsigned int count) {
  unsigned int index = 0;
  /*
   * CUDA's launch ABI requires args to point at an array with one storage
   * pointer per kernel parameter. The shim can reject null slots, but a too
   * short args array is still caller-undefined ABI misuse.
   */
  if (args == 0) {
    return 0;
  }
  for (index = 0; index < count; index++) {
    if (args[index] == 0) {
      return 0;
    }
  }
  return 1;
}

static cudaError_t psyche_cuda_runtime_validate_kernel_shape(
    dim3 gridDim,
    dim3 blockDim,
    unsigned int n,
    size_t *bytes_out) {
  size_t thread_count = 0;
  if (bytes_out == 0) {
    return cudaErrorInvalidValue;
  }
  *bytes_out = 0;
  if (gridDim.x == 0 || blockDim.x == 0) {
    return cudaErrorInvalidConfiguration;
  }
  if (
      gridDim.y != 1 ||
      gridDim.z != 1 ||
      blockDim.y != 1 ||
      blockDim.z != 1) {
    return cudaErrorNotSupported;
  }
  if (n == 0) {
    return cudaSuccess;
  }
  if ((size_t)n > SIZE_MAX / sizeof(float)) {
    return cudaErrorInvalidValue;
  }
  /*
   * This is unreachable on LP64 with CUDA's uint32 dim3.x/blockDim.x fields,
   * but it keeps the helper correct for 32-bit hosts or future ABI widening.
   */
  if ((size_t)gridDim.x > SIZE_MAX / (size_t)blockDim.x) {
    return cudaErrorInvalidConfiguration;
  }
  thread_count = (size_t)gridDim.x * (size_t)blockDim.x;
  if (thread_count < (size_t)n) {
    return cudaErrorInvalidConfiguration;
  }
  *bytes_out = (size_t)n * sizeof(float);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_launch_vector_add_f32(
    dim3 gridDim,
    dim3 blockDim,
    void **args) {
  void *a_ptr = 0;
  void *b_ptr = 0;
  void *out_ptr = 0;
  unsigned int n = 0;
  size_t bytes = 0;
  cudaError_t result = cudaSuccess;
  PsycheCudaRuntimeAllocation *a_allocation = 0;
  PsycheCudaRuntimeAllocation *b_allocation = 0;
  PsycheCudaRuntimeAllocation *out_allocation = 0;
  const float *a = 0;
  const float *b = 0;
  float *out = 0;
  float *staged_out = 0;
  unsigned int i = 0;
  if (!psyche_cuda_runtime_kernel_params_present(args, 4)) {
    return cudaErrorInvalidValue;
  }
  a_ptr = *(void **)args[0];
  b_ptr = *(void **)args[1];
  out_ptr = *(void **)args[2];
  n = *(const unsigned int *)args[3];
  result = psyche_cuda_runtime_validate_kernel_shape(
      gridDim,
      blockDim,
      n,
      &bytes);
  if (result != cudaSuccess || n == 0) {
    return result;
  }
  staged_out = (float *)malloc(bytes);
  if (staged_out == 0) {
    return cudaErrorMemoryAllocation;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  a_allocation = psyche_cuda_runtime_find_allocation_locked(a_ptr, bytes);
  b_allocation = psyche_cuda_runtime_find_allocation_locked(b_ptr, bytes);
  out_allocation = psyche_cuda_runtime_find_allocation_locked(out_ptr, bytes);
  if (a_allocation == 0 || b_allocation == 0 || out_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    free(staged_out);
    return cudaErrorInvalidValue;
  }
  a = (const float *)a_ptr;
  b = (const float *)b_ptr;
  out = (float *)out_ptr;
  for (i = 0; i < n; i++) {
    staged_out[i] = a[i] + b[i];
  }
  (void)memcpy(out, staged_out, bytes);
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  free(staged_out);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_launch_saxpy_f32(
    dim3 gridDim,
    dim3 blockDim,
    void **args) {
  void *x_ptr = 0;
  void *y_ptr = 0;
  float alpha = 0.0f;
  unsigned int n = 0;
  size_t bytes = 0;
  cudaError_t result = cudaSuccess;
  PsycheCudaRuntimeAllocation *x_allocation = 0;
  PsycheCudaRuntimeAllocation *y_allocation = 0;
  const float *x = 0;
  float *y = 0;
  float *staged_y = 0;
  unsigned int i = 0;
  if (!psyche_cuda_runtime_kernel_params_present(args, 4)) {
    return cudaErrorInvalidValue;
  }
  x_ptr = *(void **)args[0];
  y_ptr = *(void **)args[1];
  alpha = *(const float *)args[2];
  n = *(const unsigned int *)args[3];
  result = psyche_cuda_runtime_validate_kernel_shape(
      gridDim,
      blockDim,
      n,
      &bytes);
  if (result != cudaSuccess || n == 0) {
    return result;
  }
  staged_y = (float *)malloc(bytes);
  if (staged_y == 0) {
    return cudaErrorMemoryAllocation;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  x_allocation = psyche_cuda_runtime_find_allocation_locked(x_ptr, bytes);
  y_allocation = psyche_cuda_runtime_find_allocation_locked(y_ptr, bytes);
  if (x_allocation == 0 || y_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    free(staged_y);
    return cudaErrorInvalidValue;
  }
  x = (const float *)x_ptr;
  y = (float *)y_ptr;
  for (i = 0; i < n; i++) {
    staged_y[i] = alpha * x[i] + y[i];
  }
  (void)memcpy(y, staged_y, bytes);
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  free(staged_y);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_launch_scale_f32(
    dim3 gridDim,
    dim3 blockDim,
    void **args) {
  void *x_ptr = 0;
  float alpha = 0.0f;
  unsigned int n = 0;
  size_t bytes = 0;
  cudaError_t result = cudaSuccess;
  PsycheCudaRuntimeAllocation *x_allocation = 0;
  float *x = 0;
  unsigned int i = 0;
  if (!psyche_cuda_runtime_kernel_params_present(args, 3)) {
    return cudaErrorInvalidValue;
  }
  x_ptr = *(void **)args[0];
  alpha = *(const float *)args[1];
  n = *(const unsigned int *)args[2];
  result = psyche_cuda_runtime_validate_kernel_shape(
      gridDim,
      blockDim,
      n,
      &bytes);
  if (result != cudaSuccess || n == 0) {
    return result;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  x_allocation = psyche_cuda_runtime_find_allocation_locked(x_ptr, bytes);
  if (x_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorInvalidValue;
  }
  x = (float *)x_ptr;
  /*
   * scale_f32 is a single-buffer element-local update. The multi-input
   * mutating kernels stage their outputs; scale intentionally stays in-place.
   */
  for (i = 0; i < n; i++) {
    x[i] = alpha * x[i];
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_launch_axpby_f32(
    dim3 gridDim,
    dim3 blockDim,
    void **args) {
  void *x_ptr = 0;
  void *y_ptr = 0;
  float alpha = 0.0f;
  float beta = 0.0f;
  unsigned int n = 0;
  size_t bytes = 0;
  cudaError_t result = cudaSuccess;
  PsycheCudaRuntimeAllocation *x_allocation = 0;
  PsycheCudaRuntimeAllocation *y_allocation = 0;
  float *x = 0;
  const float *y = 0;
  float *staged_x = 0;
  unsigned int i = 0;
  if (!psyche_cuda_runtime_kernel_params_present(args, 5)) {
    return cudaErrorInvalidValue;
  }
  x_ptr = *(void **)args[0];
  y_ptr = *(void **)args[1];
  alpha = *(const float *)args[2];
  beta = *(const float *)args[3];
  n = *(const unsigned int *)args[4];
  result = psyche_cuda_runtime_validate_kernel_shape(
      gridDim,
      blockDim,
      n,
      &bytes);
  if (result != cudaSuccess || n == 0) {
    return result;
  }
  staged_x = (float *)malloc(bytes);
  if (staged_x == 0) {
    return cudaErrorMemoryAllocation;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  x_allocation = psyche_cuda_runtime_find_allocation_locked(x_ptr, bytes);
  y_allocation = psyche_cuda_runtime_find_allocation_locked(y_ptr, bytes);
  if (x_allocation == 0 || y_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    free(staged_x);
    return cudaErrorInvalidValue;
  }
  x = (float *)x_ptr;
  y = (const float *)y_ptr;
  for (i = 0; i < n; i++) {
    staged_x[i] = alpha * x[i] + beta * y[i];
  }
  (void)memcpy(x, staged_x, bytes);
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  free(staged_x);
  return cudaSuccess;
}

static PsycheCudaRuntimeHostAllocation *psyche_cuda_runtime_find_host_allocation_locked(
    const void *ptr,
    size_t count) {
  PsycheCudaRuntimeHostAllocation *allocation = psyche_cuda_runtime_host_allocations;
  while (allocation != 0) {
    if (psyche_cuda_runtime_host_range_contains(allocation, ptr, count)) {
      return allocation;
    }
    allocation = allocation->next;
  }
  return 0;
}

static PsycheCudaRuntimeHostAllocation *psyche_cuda_runtime_find_mapped_host_allocation_locked(
    const void *ptr,
    size_t count) {
  PsycheCudaRuntimeHostAllocation *allocation = psyche_cuda_runtime_host_allocations;
  while (allocation != 0) {
    if (
        allocation->device_mapped &&
        psyche_cuda_runtime_host_range_contains(allocation, ptr, count)) {
      return allocation;
    }
    allocation = allocation->next;
  }
  return 0;
}

static int psyche_cuda_runtime_host_range_registered_locked(
    const void *ptr,
    size_t size) {
  PsycheCudaRuntimeHostAllocation *allocation = psyche_cuda_runtime_host_allocations;
  while (allocation != 0) {
    if (psyche_cuda_runtime_ranges_overlap(ptr, size, allocation->ptr, allocation->size)) {
      return 1;
    }
    allocation = allocation->next;
  }
  return 0;
}

static PsycheCudaRuntimeHostAllocation *
psyche_cuda_runtime_find_touched_mapped_host_allocation_locked(
    const void *ptr,
    size_t count) {
  PsycheCudaRuntimeHostAllocation *allocation = psyche_cuda_runtime_host_allocations;
  uintptr_t address = (uintptr_t)ptr;
  uintptr_t copy_end = 0;
  int copy_end_overflow = count > UINTPTR_MAX - address;
  copy_end = copy_end_overflow ? UINTPTR_MAX : address + count;
  while (allocation != 0) {
    uintptr_t base = (uintptr_t)allocation->ptr;
    uintptr_t allocation_end = 0;
    if (allocation->device_mapped && allocation->size <= UINTPTR_MAX - base) {
      allocation_end = base + allocation->size;
      if (address < allocation_end && copy_end > base) {
        return allocation;
      }
    }
    allocation = allocation->next;
  }
  return 0;
}

static PsycheCudaRuntimeAllocation *psyche_cuda_runtime_find_touched_allocation_locked(
    const void *ptr,
    size_t count) {
  PsycheCudaRuntimeAllocation *allocation = psyche_cuda_runtime_allocations;
  uintptr_t address = (uintptr_t)ptr;
  uintptr_t copy_end = 0;
  int copy_end_overflow = count > UINTPTR_MAX - address;
  copy_end = copy_end_overflow ? UINTPTR_MAX : address + count;
  while (allocation != 0) {
    uintptr_t base = (uintptr_t)allocation->ptr;
    uintptr_t allocation_end = 0;
    if (allocation->size <= UINTPTR_MAX - base) {
      allocation_end = base + allocation->size;
      if (address < allocation_end && copy_end > base) {
        return allocation;
      }
    }
    allocation = allocation->next;
  }
  return 0;
}

static PsycheCudaRuntimeHostAllocation *
psyche_cuda_runtime_find_end_boundary_mapped_host_allocation_locked(
    const void *ptr,
    size_t count) {
  PsycheCudaRuntimeHostAllocation *allocation = psyche_cuda_runtime_host_allocations;
  uintptr_t address = (uintptr_t)ptr;
  if (count == 0) {
    return 0;
  }
  while (allocation != 0) {
    uintptr_t base = (uintptr_t)allocation->ptr;
    uintptr_t allocation_end = 0;
    if (allocation->device_mapped && allocation->size <= UINTPTR_MAX - base) {
      allocation_end = base + allocation->size;
      if (address == allocation_end) {
        return allocation;
      }
    }
    allocation = allocation->next;
  }
  return 0;
}

static PsycheCudaRuntimeAllocation *
psyche_cuda_runtime_find_end_boundary_allocation_locked(
    const void *ptr,
    size_t count) {
  PsycheCudaRuntimeAllocation *allocation = psyche_cuda_runtime_allocations;
  uintptr_t address = (uintptr_t)ptr;
  if (count == 0) {
    return 0;
  }
  while (allocation != 0) {
    uintptr_t base = (uintptr_t)allocation->ptr;
    uintptr_t allocation_end = 0;
    if (allocation->size <= UINTPTR_MAX - base) {
      allocation_end = base + allocation->size;
      if (address == allocation_end) {
        return allocation;
      }
    }
    allocation = allocation->next;
  }
  return 0;
}

static unsigned long long psyche_cuda_runtime_next_buffer_id_locked(void) {
  unsigned long long id = psyche_cuda_runtime_next_buffer_id;
  psyche_cuda_runtime_next_buffer_id += 1;
  if (psyche_cuda_runtime_next_buffer_id == 0) {
    psyche_cuda_runtime_next_buffer_id = 1;
  }
  return id;
}

static int psyche_cuda_runtime_pool_reserved_zeroed(const unsigned char *reserved) {
  for (size_t index = 0; index < 54; index++) {
    if (reserved[index] != 0) {
      return 0;
    }
  }
  return 1;
}

static void psyche_cuda_runtime_init_pool_defaults_locked(
    PsycheCudaRuntimeMemoryPool *pool,
    cudaMemPool_t handle,
    int is_default,
    cudaMemAllocationType alloc_type,
    cudaMemLocation location,
    size_t max_size) {
  memset(pool, 0, sizeof(*pool));
  pool->handle = handle;
  pool->is_default = is_default;
  pool->alloc_type = alloc_type;
  pool->handle_types = cudaMemHandleTypeNone;
  pool->location = location;
  pool->max_size = max_size;
  pool->reuse_follow_event_dependencies = 1;
  pool->reuse_allow_opportunistic = 1;
  pool->reuse_allow_internal_dependencies = 1;
}

static PsycheCudaRuntimeMemoryPool *psyche_cuda_runtime_default_host_pool_locked(void) {
  cudaMemLocation location;
  if (psyche_cuda_runtime_default_host_pool.handle == 0) {
    location.id = cudaCpuDeviceId;
    location.type = cudaMemLocationTypeHost;
    psyche_cuda_runtime_init_pool_defaults_locked(
        &psyche_cuda_runtime_default_host_pool,
        (cudaMemPool_t)&psyche_cuda_runtime_default_host_pool,
        1,
        cudaMemAllocationTypePinned,
        location,
        0);
  }
  return &psyche_cuda_runtime_default_host_pool;
}

static PsycheCudaRuntimeMemoryPool *psyche_cuda_runtime_default_managed_pool_locked(void) {
  cudaMemLocation location;
  if (psyche_cuda_runtime_default_managed_pool.handle == 0) {
    location.id = cudaInvalidDeviceId;
    location.type = cudaMemLocationTypeNone;
    psyche_cuda_runtime_init_pool_defaults_locked(
        &psyche_cuda_runtime_default_managed_pool,
        (cudaMemPool_t)&psyche_cuda_runtime_default_managed_pool,
        1,
        cudaMemAllocationTypeManaged,
        location,
        0);
  }
  return &psyche_cuda_runtime_default_managed_pool;
}

static PsycheCudaRuntimeMemoryPool *psyche_cuda_runtime_default_pool_locked(
    cudaMemAllocationType type) {
  if (type == cudaMemAllocationTypeManaged) {
    return psyche_cuda_runtime_default_managed_pool_locked();
  }
  return psyche_cuda_runtime_default_host_pool_locked();
}

static PsycheCudaRuntimeMemoryPool *psyche_cuda_runtime_current_pool_locked(
    cudaMemAllocationType type) {
  if (type == cudaMemAllocationTypeManaged) {
    if (psyche_cuda_runtime_current_managed_pool == 0) {
      return psyche_cuda_runtime_default_managed_pool_locked();
    }
    return (PsycheCudaRuntimeMemoryPool *)psyche_cuda_runtime_current_managed_pool;
  }
  if (psyche_cuda_runtime_current_host_pool == 0) {
    return psyche_cuda_runtime_default_host_pool_locked();
  }
  return (PsycheCudaRuntimeMemoryPool *)psyche_cuda_runtime_current_host_pool;
}

static PsycheCudaRuntimeMemoryPool *psyche_cuda_runtime_find_pool_locked(
    cudaMemPool_t pool) {
  PsycheCudaRuntimeMemoryPool *record = 0;
  if (pool == 0) {
    return 0;
  }
  record = psyche_cuda_runtime_default_host_pool_locked();
  if (record->handle == pool) {
    return record;
  }
  record = psyche_cuda_runtime_default_managed_pool_locked();
  if (record->handle == pool) {
    return record;
  }
  record = psyche_cuda_runtime_memory_pools;
  while (record != 0) {
    if (record->handle == pool) {
      return record;
    }
    record = record->next;
  }
  return 0;
}

static int psyche_cuda_runtime_pool_location_supported(
    const cudaMemLocation *location,
    cudaMemAllocationType type) {
  if (location == 0) {
    return 0;
  }
  if (type == cudaMemAllocationTypePinned) {
    return location->type == cudaMemLocationTypeHost;
  }
  if (type == cudaMemAllocationTypeManaged) {
    return location->type == cudaMemLocationTypeNone;
  }
  return 0;
}

static cudaError_t psyche_cuda_runtime_validate_pool_props(
    const cudaMemPoolProps *poolProps,
    cudaMemAllocationType *alloc_type,
    cudaMemLocation *location,
    size_t *max_size) {
  if (poolProps == 0 || alloc_type == 0 || location == 0 || max_size == 0) {
    return cudaErrorInvalidValue;
  }
  if (
      poolProps->usage != 0 ||
      poolProps->win32SecurityAttributes != 0 ||
      !psyche_cuda_runtime_pool_reserved_zeroed(poolProps->reserved)) {
    return cudaErrorInvalidValue;
  }
  if (poolProps->handleTypes != cudaMemHandleTypeNone) {
    return cudaErrorNotSupported;
  }
  if (poolProps->allocType != cudaMemAllocationTypePinned) {
    return cudaErrorInvalidValue;
  }
  if (poolProps->location.type == cudaMemLocationTypeDevice) {
    return cudaErrorInvalidDevice;
  }
  if (!psyche_cuda_runtime_pool_location_supported(&poolProps->location, poolProps->allocType)) {
    return cudaErrorInvalidValue;
  }
  *alloc_type = poolProps->allocType;
  *location = poolProps->location;
  *max_size = poolProps->maxSize;
  return cudaSuccess;
}

static void psyche_cuda_runtime_pool_account_alloc_locked(
    PsycheCudaRuntimeMemoryPool *pool,
    size_t size) {
  if (pool == 0) {
    return;
  }
  pool->reserved_current += (cuuint64_t)size;
  pool->used_current += (cuuint64_t)size;
  if (pool->reserved_current > pool->reserved_high) {
    pool->reserved_high = pool->reserved_current;
  }
  if (pool->used_current > pool->used_high) {
    pool->used_high = pool->used_current;
  }
}

static int psyche_cuda_runtime_pool_can_account_alloc_locked(
    PsycheCudaRuntimeMemoryPool *pool,
    size_t size) {
  cuuint64_t amount = (cuuint64_t)size;
  if (pool == 0) {
    return 1;
  }
  if (
      pool->reserved_current > UINT64_MAX - amount ||
      pool->used_current > UINT64_MAX - amount) {
    return 0;
  }
  if (
      pool->max_size != 0 &&
      (
          pool->reserved_current > (cuuint64_t)pool->max_size ||
          amount > (cuuint64_t)pool->max_size - pool->reserved_current)) {
    return 0;
  }
  return 1;
}

static void psyche_cuda_runtime_pool_account_free_locked(
    PsycheCudaRuntimeMemoryPool *pool,
    size_t size) {
  if (pool == 0) {
    return;
  }
  if ((cuuint64_t)size > pool->used_current) {
    pool->used_current = 0;
  } else {
    pool->used_current -= (cuuint64_t)size;
  }
  if ((cuuint64_t)size > pool->reserved_current) {
    pool->reserved_current = 0;
  } else {
    pool->reserved_current -= (cuuint64_t)size;
  }
}

static cudaError_t psyche_cuda_runtime_malloc_kind_simulated(
    void **devPtr,
    size_t size,
    int managed,
    cudaMemPool_t pool_handle,
    int use_current_pool,
    int async_alloc) {
  PsycheCudaRuntimeAllocation *allocation = 0;
  PsycheCudaRuntimeMemoryPool *pool = 0;
  void *ptr = 0;
  if (size == 0) {
    *devPtr = 0;
    return cudaErrorInvalidValue;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  if (use_current_pool) {
    pool = psyche_cuda_runtime_current_pool_locked(cudaMemAllocationTypePinned);
  } else if (pool_handle != 0) {
    pool = psyche_cuda_runtime_find_pool_locked(pool_handle);
    if (pool == 0) {
      pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
      *devPtr = 0;
      return cudaErrorInvalidResourceHandle;
    }
  }
  if (pool != 0 && pool->alloc_type == cudaMemAllocationTypeManaged) {
    managed = 1;
  }
  if (!psyche_cuda_runtime_pool_can_account_alloc_locked(pool, size)) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    *devPtr = 0;
    return cudaErrorMemoryAllocation;
  }
  if (posix_memalign(&ptr, 256, size) != 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    *devPtr = 0;
    return cudaErrorMemoryAllocation;
  }
  allocation = (PsycheCudaRuntimeAllocation *)malloc(sizeof(*allocation));
  if (allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    free(ptr);
    *devPtr = 0;
    return cudaErrorMemoryAllocation;
  }
  allocation->ptr = ptr;
  allocation->size = size;
  allocation->pool = pool != 0 ? pool->handle : 0;
  allocation->managed = managed;
  allocation->async_alloc = async_alloc;
  allocation->read_mostly = 0;
  allocation->preferred_location = cudaInvalidDeviceId;
  allocation->accessed_by = cudaInvalidDeviceId;
  allocation->last_prefetch_location = cudaInvalidDeviceId;
  allocation->sync_memops = 0;
  allocation->buffer_id = psyche_cuda_runtime_next_buffer_id_locked();
  psyche_cuda_runtime_pool_account_alloc_locked(pool, size);
  allocation->next = psyche_cuda_runtime_allocations;
  psyche_cuda_runtime_allocations = allocation;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  *devPtr = ptr;
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_malloc_simulated(void **devPtr, size_t size) {
  return psyche_cuda_runtime_malloc_kind_simulated(devPtr, size, 0, 0, 0, 0);
}

static cudaError_t psyche_cuda_runtime_malloc_managed_simulated(
    void **devPtr,
    size_t size,
    unsigned int flags) {
  const unsigned int allowed_flags =
      cudaMemAttachGlobal | cudaMemAttachHost | cudaMemAttachSingle;
  if (devPtr == 0) {
    return cudaErrorInvalidValue;
  }
  if ((flags & ~allowed_flags) != 0) {
    *devPtr = 0;
    return cudaErrorInvalidValue;
  }
  return psyche_cuda_runtime_malloc_kind_simulated(devPtr, size, 1, 0, 0, 0);
}

static PsycheCudaRuntimeAllocation *psyche_cuda_runtime_find_managed_allocation_locked(
    const void *ptr,
    size_t count) {
  PsycheCudaRuntimeAllocation *allocation =
      psyche_cuda_runtime_find_allocation_locked(ptr, count);
  if (allocation == 0 || !allocation->managed) {
    return 0;
  }
  return allocation;
}

static cudaError_t psyche_cuda_runtime_validate_managed_range_locked(
    const void *ptr,
    size_t count,
    PsycheCudaRuntimeAllocation **allocation_out) {
  PsycheCudaRuntimeAllocation *allocation = 0;
  if (ptr == 0 || count == 0) {
    return cudaErrorInvalidValue;
  }
  allocation = psyche_cuda_runtime_find_managed_allocation_locked(ptr, count);
  if (allocation == 0) {
    return cudaErrorInvalidValue;
  }
  if (allocation_out != 0) {
    *allocation_out = allocation;
  }
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_managed_location_from_hint(
    int location_hint,
    int *location) {
  if (location_hint == cudaCpuDeviceId || location_hint == cudaInvalidDeviceId) {
    *location = location_hint;
    return cudaSuccess;
  }
  if (location_hint >= 0) {
    return cudaErrorInvalidDevice;
  }
  return cudaErrorInvalidValue;
}

static cudaError_t psyche_cuda_runtime_mem_advise_simulated(
    const void *devPtr,
    size_t count,
    cudaMemoryAdvise advice,
    int location_hint) {
  PsycheCudaRuntimeAllocation *allocation = 0;
  cudaError_t result = cudaSuccess;
  int location = cudaInvalidDeviceId;
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  result = psyche_cuda_runtime_validate_managed_range_locked(devPtr, count, &allocation);
  if (result != cudaSuccess) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return result;
  }
  switch (advice) {
    case cudaMemAdviceSetReadMostly:
      allocation->read_mostly = 1;
      break;
    case cudaMemAdviceUnsetReadMostly:
      allocation->read_mostly = 0;
      break;
    case cudaMemAdviceSetPreferredLocation:
      result = psyche_cuda_runtime_managed_location_from_hint(location_hint, &location);
      if (result == cudaSuccess && location == cudaCpuDeviceId) {
        allocation->preferred_location = location;
      } else if (result == cudaSuccess) {
        result = cudaErrorInvalidValue;
      }
      break;
    case cudaMemAdviceUnsetPreferredLocation:
      allocation->preferred_location = cudaInvalidDeviceId;
      break;
    case cudaMemAdviceSetAccessedBy:
      result = cudaErrorInvalidDevice;
      break;
    case cudaMemAdviceUnsetAccessedBy:
      allocation->accessed_by = cudaInvalidDeviceId;
      break;
    default:
      result = cudaErrorInvalidValue;
      break;
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return result;
}

static cudaError_t psyche_cuda_runtime_mem_prefetch_simulated(
    const void *devPtr,
    size_t count,
    int location_hint) {
  PsycheCudaRuntimeAllocation *allocation = 0;
  cudaError_t result = cudaSuccess;
  int location = cudaInvalidDeviceId;
  result = psyche_cuda_runtime_managed_location_from_hint(location_hint, &location);
  if (result != cudaSuccess) {
    return result;
  }
  if (location != cudaCpuDeviceId) {
    return cudaErrorInvalidValue;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  result = psyche_cuda_runtime_validate_managed_range_locked(devPtr, count, &allocation);
  if (result == cudaSuccess) {
    allocation->last_prefetch_location = cudaCpuDeviceId;
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return result;
}

static cudaError_t psyche_cuda_runtime_fill_accessed_by(void *data, size_t dataSize) {
  int *values = (int *)data;
  size_t count = dataSize / sizeof(int);
  if (data == 0 || dataSize == 0 || (dataSize % sizeof(int)) != 0) {
    return cudaErrorInvalidValue;
  }
  for (size_t index = 0; index < count; index++) {
    values[index] = cudaInvalidDeviceId;
  }
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_mem_range_get_attribute_locked(
    void *data,
    size_t dataSize,
    cudaMemRangeAttribute attribute,
    const void *devPtr,
    size_t count) {
  PsycheCudaRuntimeAllocation *allocation = 0;
  cudaError_t result =
      psyche_cuda_runtime_validate_managed_range_locked(devPtr, count, &allocation);
  if (result != cudaSuccess) {
    return result;
  }
  if (data == 0) {
    return cudaErrorInvalidValue;
  }
  switch (attribute) {
    case cudaMemRangeAttributeReadMostly:
      if (dataSize != sizeof(int)) {
        return cudaErrorInvalidValue;
      }
      *(int *)data = allocation->read_mostly;
      return cudaSuccess;
    case cudaMemRangeAttributePreferredLocation:
      if (dataSize != sizeof(int)) {
        return cudaErrorInvalidValue;
      }
      *(int *)data = allocation->preferred_location;
      return cudaSuccess;
    case cudaMemRangeAttributeAccessedBy:
      return psyche_cuda_runtime_fill_accessed_by(data, dataSize);
    case cudaMemRangeAttributeLastPrefetchLocation:
      if (dataSize != sizeof(int)) {
        return cudaErrorInvalidValue;
      }
      *(int *)data = allocation->last_prefetch_location;
      return cudaSuccess;
    case cudaMemRangeAttributePreferredLocationType:
      if (dataSize != sizeof(cudaMemLocationType)) {
        return cudaErrorInvalidValue;
      }
      *(cudaMemLocationType *)data =
          allocation->preferred_location == cudaCpuDeviceId ?
          cudaMemLocationTypeHost :
          cudaMemLocationTypeInvalid;
      return cudaSuccess;
    case cudaMemRangeAttributePreferredLocationId:
      if (dataSize != sizeof(int)) {
        return cudaErrorInvalidValue;
      }
      *(int *)data = allocation->preferred_location;
      return cudaSuccess;
    case cudaMemRangeAttributeLastPrefetchLocationType:
      if (dataSize != sizeof(cudaMemLocationType)) {
        return cudaErrorInvalidValue;
      }
      *(cudaMemLocationType *)data =
          allocation->last_prefetch_location == cudaCpuDeviceId ?
          cudaMemLocationTypeHost :
          cudaMemLocationTypeInvalid;
      return cudaSuccess;
    case cudaMemRangeAttributeLastPrefetchLocationId:
      if (dataSize != sizeof(int)) {
        return cudaErrorInvalidValue;
      }
      *(int *)data = allocation->last_prefetch_location;
      return cudaSuccess;
    default:
      return cudaErrorInvalidValue;
  }
}

static cudaError_t psyche_cuda_runtime_pointer_get_attributes_simulated(
    cudaPointerAttributes *attributes,
    const void *ptr) {
  PsycheCudaRuntimeAllocation *allocation = 0;
  PsycheCudaRuntimeHostAllocation *host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *mapped_host_allocation = 0;
  if (attributes == 0 || ptr == 0) {
    return cudaErrorInvalidValue;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  allocation = psyche_cuda_runtime_find_allocation_locked(ptr, 1);
  host_allocation = psyche_cuda_runtime_find_host_allocation_locked(ptr, 1);
  mapped_host_allocation = psyche_cuda_runtime_find_mapped_host_allocation_locked(ptr, 1);
  if (allocation == 0 && host_allocation == 0 && mapped_host_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorInvalidValue;
  }
  memset(attributes, 0, sizeof(*attributes));
  attributes->device = cudaInvalidDeviceId;
  if (allocation != 0) {
    /*
     * cudaMemoryTypeManaged here means "allocated through cudaMallocManaged
     * under CPU-backed simulation", not GPU residency, migration, or real UVA.
     */
    attributes->type = allocation->managed ? cudaMemoryTypeManaged : cudaMemoryTypeDevice;
    attributes->devicePointer = allocation->ptr;
    attributes->hostPointer = allocation->managed ? allocation->ptr : 0;
  } else {
    attributes->type = cudaMemoryTypeHost;
    attributes->devicePointer = mapped_host_allocation != 0 ? mapped_host_allocation->ptr : 0;
    attributes->hostPointer = host_allocation != 0 ? host_allocation->ptr : mapped_host_allocation->ptr;
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_mem_pool_get_attribute_locked(
    cudaMemPool_t memPool,
    cudaMemPoolAttr attr,
    void *value) {
  PsycheCudaRuntimeMemoryPool *record = 0;
  if (memPool == 0 || value == 0) {
    return cudaErrorInvalidValue;
  }
  record = psyche_cuda_runtime_find_pool_locked(memPool);
  if (record == 0) {
    return cudaErrorInvalidResourceHandle;
  }
  switch (attr) {
    case cudaMemPoolReuseFollowEventDependencies:
      *(int *)value = record->reuse_follow_event_dependencies;
      return cudaSuccess;
    case cudaMemPoolReuseAllowOpportunistic:
      *(int *)value = record->reuse_allow_opportunistic;
      return cudaSuccess;
    case cudaMemPoolReuseAllowInternalDependencies:
      *(int *)value = record->reuse_allow_internal_dependencies;
      return cudaSuccess;
    case cudaMemPoolAttrReleaseThreshold:
      *(cuuint64_t *)value = record->release_threshold;
      return cudaSuccess;
    case cudaMemPoolAttrReservedMemCurrent:
      *(cuuint64_t *)value = record->reserved_current;
      return cudaSuccess;
    case cudaMemPoolAttrReservedMemHigh:
      *(cuuint64_t *)value = record->reserved_high;
      return cudaSuccess;
    case cudaMemPoolAttrUsedMemCurrent:
      *(cuuint64_t *)value = record->used_current;
      return cudaSuccess;
    case cudaMemPoolAttrUsedMemHigh:
      *(cuuint64_t *)value = record->used_high;
      return cudaSuccess;
    case cudaMemPoolAttrAllocationType:
      *(cudaMemAllocationType *)value = record->alloc_type;
      return cudaSuccess;
    case cudaMemPoolAttrExportHandleTypes:
      *(cudaMemAllocationHandleType *)value = record->handle_types;
      return cudaSuccess;
    case cudaMemPoolAttrLocationId:
      *(int *)value = record->location.id;
      return cudaSuccess;
    case cudaMemPoolAttrLocationType:
      *(cudaMemLocationType *)value = record->location.type;
      return cudaSuccess;
    case cudaMemPoolAttrMaxPoolSize:
      *(cuuint64_t *)value = (cuuint64_t)record->max_size;
      return cudaSuccess;
    case cudaMemPoolAttrHwDecompressEnabled:
      *(int *)value = 0;
      return cudaSuccess;
    default:
      return cudaErrorInvalidValue;
  }
}

static cudaError_t psyche_cuda_runtime_mem_pool_set_attribute_locked(
    cudaMemPool_t memPool,
    cudaMemPoolAttr attr,
    void *value) {
  PsycheCudaRuntimeMemoryPool *record = 0;
  if (memPool == 0 || value == 0) {
    return cudaErrorInvalidValue;
  }
  record = psyche_cuda_runtime_find_pool_locked(memPool);
  if (record == 0) {
    return cudaErrorInvalidResourceHandle;
  }
  switch (attr) {
    case cudaMemPoolReuseFollowEventDependencies:
      record->reuse_follow_event_dependencies = (*(int *)value) != 0;
      return cudaSuccess;
    case cudaMemPoolReuseAllowOpportunistic:
      record->reuse_allow_opportunistic = (*(int *)value) != 0;
      return cudaSuccess;
    case cudaMemPoolReuseAllowInternalDependencies:
      record->reuse_allow_internal_dependencies = (*(int *)value) != 0;
      return cudaSuccess;
    case cudaMemPoolAttrReleaseThreshold:
      record->release_threshold = *(cuuint64_t *)value;
      return cudaSuccess;
    case cudaMemPoolAttrReservedMemHigh:
      if (*(cuuint64_t *)value != 0) {
        return cudaErrorInvalidValue;
      }
      record->reserved_high = record->reserved_current;
      return cudaSuccess;
    case cudaMemPoolAttrUsedMemHigh:
      if (*(cuuint64_t *)value != 0) {
        return cudaErrorInvalidValue;
      }
      record->used_high = record->used_current;
      return cudaSuccess;
    case cudaMemPoolAttrReservedMemCurrent:
    case cudaMemPoolAttrUsedMemCurrent:
    case cudaMemPoolAttrAllocationType:
    case cudaMemPoolAttrExportHandleTypes:
    case cudaMemPoolAttrLocationId:
    case cudaMemPoolAttrLocationType:
    case cudaMemPoolAttrMaxPoolSize:
    case cudaMemPoolAttrHwDecompressEnabled:
      return cudaErrorInvalidValue;
    default:
      return cudaErrorInvalidValue;
  }
}

static cudaError_t psyche_cuda_runtime_mem_pool_create_simulated(
    cudaMemPool_t *memPool,
    const cudaMemPoolProps *poolProps) {
  PsycheCudaRuntimeMemoryPool *record = 0;
  cudaMemAllocationType alloc_type = cudaMemAllocationTypeInvalid;
  cudaMemLocation location;
  size_t max_size = 0;
  cudaError_t result = cudaSuccess;
  result = psyche_cuda_runtime_validate_pool_props(
      poolProps,
      &alloc_type,
      &location,
      &max_size);
  if (result != cudaSuccess) {
    *memPool = 0;
    return result;
  }
  record = (PsycheCudaRuntimeMemoryPool *)malloc(sizeof(*record));
  if (record == 0) {
    *memPool = 0;
    return cudaErrorMemoryAllocation;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  psyche_cuda_runtime_init_pool_defaults_locked(
      record,
      (cudaMemPool_t)record,
      0,
      alloc_type,
      location,
      max_size);
  record->next = psyche_cuda_runtime_memory_pools;
  psyche_cuda_runtime_memory_pools = record;
  *memPool = record->handle;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_mem_pool_destroy_simulated(cudaMemPool_t memPool) {
  PsycheCudaRuntimeMemoryPool **link = &psyche_cuda_runtime_memory_pools;
  PsycheCudaRuntimeMemoryPool *record = 0;
  if (
      memPool == 0 ||
      memPool == (cudaMemPool_t)&psyche_cuda_runtime_default_host_pool ||
      memPool == (cudaMemPool_t)&psyche_cuda_runtime_default_managed_pool) {
    return cudaErrorInvalidValue;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  while (*link != 0 && (*link)->handle != memPool) {
    link = &(*link)->next;
  }
  if (*link == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorInvalidResourceHandle;
  }
  record = *link;
  if (record->used_current != 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorInvalidValue;
  }
  *link = record->next;
  if (psyche_cuda_runtime_current_host_pool == memPool) {
    psyche_cuda_runtime_current_host_pool = 0;
  }
  if (psyche_cuda_runtime_current_managed_pool == memPool) {
    psyche_cuda_runtime_current_managed_pool = 0;
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  free(record);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_mem_pool_trim_to_simulated(
    cudaMemPool_t memPool,
    size_t minBytesToKeep) {
  PsycheCudaRuntimeMemoryPool *record = 0;
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  record = psyche_cuda_runtime_find_pool_locked(memPool);
  if (record == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return memPool == 0 ? cudaErrorInvalidValue : cudaErrorInvalidResourceHandle;
  }
  (void)minBytesToKeep;
  if (record->reserved_current < record->used_current) {
    record->reserved_current = record->used_current;
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_validate_mem_pool_location_request(
    const cudaMemLocation *location,
    cudaMemAllocationType type) {
  if (location == 0) {
    return cudaErrorInvalidValue;
  }
  if (location->type == cudaMemLocationTypeDevice) {
    return cudaErrorInvalidDevice;
  }
  if (!psyche_cuda_runtime_pool_location_supported(location, type)) {
    return cudaErrorInvalidValue;
  }
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_malloc_pitch_simulated(
    void **devPtr,
    size_t *pitch,
    size_t width,
    size_t height) {
  size_t simulated_pitch = 0;
  size_t bytes = 0;
  cudaError_t result = cudaSuccess;
  if (width == 0 || height == 0) {
    *devPtr = 0;
    *pitch = 0;
    return cudaErrorInvalidValue;
  }
  if (!psyche_cuda_runtime_align_up_size_checked(width, 16, &simulated_pitch)) {
    *devPtr = 0;
    *pitch = 0;
    return cudaErrorInvalidValue;
  }
  if (!psyche_cuda_runtime_mul_size_checked(simulated_pitch, height, &bytes)) {
    *devPtr = 0;
    *pitch = 0;
    return cudaErrorInvalidValue;
  }
  result = psyche_cuda_runtime_malloc_simulated(devPtr, bytes);
  if (result != cudaSuccess) {
    *pitch = 0;
    return result;
  }
  *pitch = simulated_pitch;
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_free_simulated(
    void *devPtr,
    cudaError_t missing_error,
    cudaError_t null_error) {
  PsycheCudaRuntimeAllocation **link = &psyche_cuda_runtime_allocations;
  PsycheCudaRuntimeAllocation *allocation = 0;
  if (devPtr == 0) {
    return null_error;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  while (*link != 0 && (*link)->ptr != devPtr) {
    link = &(*link)->next;
  }
  if (*link == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return missing_error;
  }
  allocation = *link;
  *link = allocation->next;
  psyche_cuda_runtime_pool_account_free_locked(
      psyche_cuda_runtime_find_pool_locked(allocation->pool),
      allocation->size);
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  free(allocation->ptr);
  free(allocation);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_host_alloc_simulated(
    void **pHost,
    size_t size,
    unsigned int flags,
    int device_mapped) {
  PsycheCudaRuntimeHostAllocation *allocation = 0;
  void *ptr = 0;
  const unsigned int allowed_flags =
      cudaHostAllocPortable | cudaHostAllocMapped | cudaHostAllocWriteCombined;
  if ((flags & ~allowed_flags) != 0) {
    *pHost = 0;
    return cudaErrorInvalidValue;
  }
  if (size == 0) {
    *pHost = 0;
    return cudaErrorInvalidValue;
  }
  ptr = malloc(size);
  if (ptr == 0) {
    *pHost = 0;
    return cudaErrorMemoryAllocation;
  }
  allocation = (PsycheCudaRuntimeHostAllocation *)malloc(sizeof(*allocation));
  if (allocation == 0) {
    free(ptr);
    *pHost = 0;
    return cudaErrorMemoryAllocation;
  }
  allocation->ptr = ptr;
  allocation->size = size;
  allocation->flags = flags;
  allocation->owns_memory = 1;
  allocation->registered = 0;
  allocation->device_mapped = device_mapped;
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  allocation->next = psyche_cuda_runtime_host_allocations;
  psyche_cuda_runtime_host_allocations = allocation;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  *pHost = ptr;
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_host_register_simulated(
    void *ptr,
    size_t size,
    unsigned int flags) {
  PsycheCudaRuntimeHostAllocation *allocation = 0;
  const unsigned int allowed_flags =
      cudaHostRegisterPortable |
      cudaHostRegisterMapped |
      cudaHostRegisterIoMemory |
      cudaHostRegisterReadOnly;
  if (!psyche_cuda_runtime_range_is_valid(ptr, size)) {
    return cudaErrorInvalidValue;
  }
  if ((flags & ~allowed_flags) != 0) {
    return cudaErrorInvalidValue;
  }
  if ((flags & cudaHostRegisterIoMemory) != 0) {
    return cudaErrorNotSupported;
  }
  allocation = (PsycheCudaRuntimeHostAllocation *)malloc(sizeof(*allocation));
  if (allocation == 0) {
    return cudaErrorMemoryAllocation;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  if (psyche_cuda_runtime_host_range_registered_locked(ptr, size)) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    free(allocation);
    return cudaErrorHostMemoryAlreadyRegistered;
  }
  if (psyche_cuda_runtime_find_touched_allocation_locked(ptr, size) != 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    free(allocation);
    return cudaErrorInvalidValue;
  }
  allocation->ptr = ptr;
  allocation->size = size;
  allocation->flags = flags;
  allocation->owns_memory = 0;
  allocation->registered = 1;
  allocation->device_mapped = (flags & cudaHostRegisterMapped) != 0;
  allocation->next = psyche_cuda_runtime_host_allocations;
  psyche_cuda_runtime_host_allocations = allocation;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_free_host_simulated(
    void *ptr,
    cudaError_t missing_error,
    cudaError_t null_error) {
  PsycheCudaRuntimeHostAllocation **link = &psyche_cuda_runtime_host_allocations;
  PsycheCudaRuntimeHostAllocation *allocation = 0;
  if (ptr == 0) {
    return null_error;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  while (*link != 0 && (*link)->ptr != ptr) {
    link = &(*link)->next;
  }
  if (*link == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return missing_error;
  }
  allocation = *link;
  if (!allocation->owns_memory) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorInvalidValue;
  }
  *link = allocation->next;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  free(allocation->ptr);
  free(allocation);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_host_unregister_simulated(
    void *ptr,
    cudaError_t missing_error,
    cudaError_t null_error) {
  PsycheCudaRuntimeHostAllocation **link = &psyche_cuda_runtime_host_allocations;
  PsycheCudaRuntimeHostAllocation *allocation = 0;
  if (ptr == 0) {
    return null_error;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  while (*link != 0 && (*link)->ptr != ptr) {
    link = &(*link)->next;
  }
  if (*link == 0 || !(*link)->registered) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return missing_error;
  }
  allocation = *link;
  *link = allocation->next;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  free(allocation);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_host_get_flags_simulated(
    unsigned int *pFlags,
    void *pHost) {
  PsycheCudaRuntimeHostAllocation *allocation = 0;
  if (pFlags == 0 || pHost == 0) {
    return cudaErrorInvalidValue;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  allocation = psyche_cuda_runtime_find_host_allocation_locked(pHost, 1);
  if (allocation == 0 || !allocation->owns_memory) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorInvalidValue;
  }
  *pFlags = allocation->flags;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_host_get_device_pointer_simulated(
    void **pDevice,
    void *pHost,
    unsigned int flags) {
  PsycheCudaRuntimeHostAllocation *allocation = 0;
  if (pDevice == 0) {
    return cudaErrorInvalidValue;
  }
  if (flags != 0 || pHost == 0) {
    *pDevice = 0;
    return cudaErrorInvalidValue;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  allocation = psyche_cuda_runtime_find_host_allocation_locked(pHost, 1);
  if (allocation == 0 || !allocation->device_mapped) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    *pDevice = 0;
    return cudaErrorInvalidValue;
  }
  *pDevice = pHost;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static void psyche_cuda_runtime_free_all_simulated(void) {
  PsycheCudaRuntimeAllocation *allocation = 0;
  PsycheCudaRuntimeHostAllocation *host_allocation = 0;
  PsycheCudaRuntimeStream *stream = 0;
  PsycheCudaRuntimeEvent *event = 0;
  PsycheCudaRuntimeMemoryPool *pool = 0;
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  allocation = psyche_cuda_runtime_allocations;
  psyche_cuda_runtime_allocations = 0;
  host_allocation = psyche_cuda_runtime_host_allocations;
  psyche_cuda_runtime_host_allocations = 0;
  stream = psyche_cuda_runtime_streams;
  psyche_cuda_runtime_streams = 0;
  event = psyche_cuda_runtime_events;
  psyche_cuda_runtime_events = 0;
  pool = psyche_cuda_runtime_memory_pools;
  psyche_cuda_runtime_memory_pools = 0;
  psyche_cuda_runtime_current_host_pool = 0;
  psyche_cuda_runtime_current_managed_pool = 0;
  memset(&psyche_cuda_runtime_default_host_pool, 0, sizeof(psyche_cuda_runtime_default_host_pool));
  memset(&psyche_cuda_runtime_default_managed_pool, 0, sizeof(psyche_cuda_runtime_default_managed_pool));
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  while (allocation != 0) {
    PsycheCudaRuntimeAllocation *next = allocation->next;
    free(allocation->ptr);
    free(allocation);
    allocation = next;
  }
  while (host_allocation != 0) {
    PsycheCudaRuntimeHostAllocation *next = host_allocation->next;
    if (host_allocation->owns_memory) {
      free(host_allocation->ptr);
    }
    free(host_allocation);
    host_allocation = next;
  }
  while (stream != 0) {
    PsycheCudaRuntimeStream *next = stream->next;
    free(stream);
    stream = next;
  }
  while (event != 0) {
    PsycheCudaRuntimeEvent *next = event->next;
    free(event);
    event = next;
  }
  while (pool != 0) {
    PsycheCudaRuntimeMemoryPool *next = pool->next;
    free(pool);
    pool = next;
  }
}

static int psyche_cuda_runtime_has_simulated_state(void) {
  int has_state = 0;
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  has_state =
      psyche_cuda_runtime_allocations != 0 ||
      psyche_cuda_runtime_host_allocations != 0 ||
      psyche_cuda_runtime_streams != 0 ||
      psyche_cuda_runtime_events != 0 ||
      psyche_cuda_runtime_memory_pools != 0 ||
      psyche_cuda_runtime_default_host_pool.handle != 0 ||
      psyche_cuda_runtime_default_managed_pool.handle != 0 ||
      psyche_cuda_runtime_current_host_pool != 0 ||
      psyche_cuda_runtime_current_managed_pool != 0;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return has_state;
}

static cudaError_t psyche_cuda_runtime_memcpy_simulated(
    void *dst,
    const void *src,
    size_t count,
    cudaMemcpyKind kind) {
  PsycheCudaRuntimeAllocation *dst_allocation = 0;
  PsycheCudaRuntimeAllocation *src_allocation = 0;
  PsycheCudaRuntimeAllocation *dst_touched_allocation = 0;
  PsycheCudaRuntimeAllocation *src_touched_allocation = 0;
  PsycheCudaRuntimeAllocation *dst_boundary_allocation = 0;
  PsycheCudaRuntimeAllocation *src_boundary_allocation = 0;
  PsycheCudaRuntimeHostAllocation *dst_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *src_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *dst_touched_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *src_touched_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *dst_boundary_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *src_boundary_mapped_host_allocation = 0;
  int dst_simulated_device = 0;
  int src_simulated_device = 0;
  int dst_device_accessible = 0;
  int src_device_accessible = 0;
  int dst_default_boundary = 0;
  int src_default_boundary = 0;

  if (kind < cudaMemcpyHostToHost || kind > cudaMemcpyDefault) {
    return cudaErrorInvalidValue;
  }
  if (count == 0) {
    return cudaSuccess;
  }
  if (dst == 0 || src == 0) {
    return cudaErrorInvalidValue;
  }

  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  dst_allocation = psyche_cuda_runtime_find_allocation_locked(dst, count);
  src_allocation = psyche_cuda_runtime_find_allocation_locked(src, count);
  dst_touched_allocation = psyche_cuda_runtime_find_touched_allocation_locked(dst, count);
  src_touched_allocation = psyche_cuda_runtime_find_touched_allocation_locked(src, count);
  dst_boundary_allocation =
      psyche_cuda_runtime_find_end_boundary_allocation_locked(dst, count);
  src_boundary_allocation =
      psyche_cuda_runtime_find_end_boundary_allocation_locked(src, count);
  dst_mapped_host_allocation =
      psyche_cuda_runtime_find_mapped_host_allocation_locked(dst, count);
  src_mapped_host_allocation =
      psyche_cuda_runtime_find_mapped_host_allocation_locked(src, count);
  dst_touched_mapped_host_allocation =
      psyche_cuda_runtime_find_touched_mapped_host_allocation_locked(dst, count);
  src_touched_mapped_host_allocation =
      psyche_cuda_runtime_find_touched_mapped_host_allocation_locked(src, count);
  dst_boundary_mapped_host_allocation =
      psyche_cuda_runtime_find_end_boundary_mapped_host_allocation_locked(dst, count);
  src_boundary_mapped_host_allocation =
      psyche_cuda_runtime_find_end_boundary_mapped_host_allocation_locked(src, count);
  dst_simulated_device = dst_allocation != 0;
  src_simulated_device = src_allocation != 0;
  dst_device_accessible = dst_simulated_device || dst_mapped_host_allocation != 0;
  src_device_accessible = src_simulated_device || src_mapped_host_allocation != 0;
  dst_default_boundary =
      kind == cudaMemcpyDefault &&
      (dst_boundary_allocation != 0 || dst_boundary_mapped_host_allocation != 0);
  src_default_boundary =
      kind == cudaMemcpyDefault &&
      (src_boundary_allocation != 0 || src_boundary_mapped_host_allocation != 0);

  if (
      (dst_touched_allocation != 0 && dst_allocation == 0) ||
      (src_touched_allocation != 0 && src_allocation == 0) ||
      (dst_touched_mapped_host_allocation != 0 && dst_mapped_host_allocation == 0) ||
      (src_touched_mapped_host_allocation != 0 && src_mapped_host_allocation == 0) ||
      (dst_default_boundary && !dst_device_accessible) ||
      (src_default_boundary && !src_device_accessible) ||
      (kind == cudaMemcpyHostToHost && (dst_simulated_device || src_simulated_device)) ||
      (kind == cudaMemcpyHostToDevice && (!dst_device_accessible || src_simulated_device)) ||
      (kind == cudaMemcpyDeviceToHost && (dst_simulated_device || !src_device_accessible)) ||
      (kind == cudaMemcpyDeviceToDevice && (!dst_device_accessible || !src_device_accessible))) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorInvalidValue;
  }

  memmove(dst, src, count);
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_validate_row_copy_locked(
    void *dst,
    const void *src,
    size_t count,
    cudaMemcpyKind kind) {
  PsycheCudaRuntimeAllocation *dst_allocation = 0;
  PsycheCudaRuntimeAllocation *src_allocation = 0;
  PsycheCudaRuntimeAllocation *dst_touched_allocation = 0;
  PsycheCudaRuntimeAllocation *src_touched_allocation = 0;
  PsycheCudaRuntimeAllocation *dst_boundary_allocation = 0;
  PsycheCudaRuntimeAllocation *src_boundary_allocation = 0;
  PsycheCudaRuntimeHostAllocation *dst_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *src_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *dst_touched_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *src_touched_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *dst_boundary_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *src_boundary_mapped_host_allocation = 0;
  int dst_simulated_device = 0;
  int src_simulated_device = 0;
  int dst_device_accessible = 0;
  int src_device_accessible = 0;
  int dst_default_boundary = 0;
  int src_default_boundary = 0;

  dst_allocation = psyche_cuda_runtime_find_allocation_locked(dst, count);
  src_allocation = psyche_cuda_runtime_find_allocation_locked(src, count);
  dst_touched_allocation = psyche_cuda_runtime_find_touched_allocation_locked(dst, count);
  src_touched_allocation = psyche_cuda_runtime_find_touched_allocation_locked(src, count);
  dst_boundary_allocation =
      psyche_cuda_runtime_find_end_boundary_allocation_locked(dst, count);
  src_boundary_allocation =
      psyche_cuda_runtime_find_end_boundary_allocation_locked(src, count);
  dst_mapped_host_allocation =
      psyche_cuda_runtime_find_mapped_host_allocation_locked(dst, count);
  src_mapped_host_allocation =
      psyche_cuda_runtime_find_mapped_host_allocation_locked(src, count);
  dst_touched_mapped_host_allocation =
      psyche_cuda_runtime_find_touched_mapped_host_allocation_locked(dst, count);
  src_touched_mapped_host_allocation =
      psyche_cuda_runtime_find_touched_mapped_host_allocation_locked(src, count);
  dst_boundary_mapped_host_allocation =
      psyche_cuda_runtime_find_end_boundary_mapped_host_allocation_locked(dst, count);
  src_boundary_mapped_host_allocation =
      psyche_cuda_runtime_find_end_boundary_mapped_host_allocation_locked(src, count);
  dst_simulated_device = dst_allocation != 0;
  src_simulated_device = src_allocation != 0;
  dst_device_accessible = dst_simulated_device || dst_mapped_host_allocation != 0;
  src_device_accessible = src_simulated_device || src_mapped_host_allocation != 0;
  dst_default_boundary =
      kind == cudaMemcpyDefault &&
      (dst_boundary_allocation != 0 || dst_boundary_mapped_host_allocation != 0);
  src_default_boundary =
      kind == cudaMemcpyDefault &&
      (src_boundary_allocation != 0 || src_boundary_mapped_host_allocation != 0);

  if (
      (dst_touched_allocation != 0 && dst_allocation == 0) ||
      (src_touched_allocation != 0 && src_allocation == 0) ||
      (dst_touched_mapped_host_allocation != 0 && dst_mapped_host_allocation == 0) ||
      (src_touched_mapped_host_allocation != 0 && src_mapped_host_allocation == 0) ||
      (dst_default_boundary && !dst_device_accessible) ||
      (src_default_boundary && !src_device_accessible) ||
      (kind == cudaMemcpyHostToHost && (dst_simulated_device || src_simulated_device)) ||
      (kind == cudaMemcpyHostToDevice && (!dst_device_accessible || src_simulated_device)) ||
      (kind == cudaMemcpyDeviceToHost && (dst_simulated_device || !src_device_accessible)) ||
      (kind == cudaMemcpyDeviceToDevice && (!dst_device_accessible || !src_device_accessible))) {
    return cudaErrorInvalidValue;
  }
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_offset_pointer(
    const void *base,
    size_t pitch,
    size_t row,
    const void **row_ptr) {
  uintptr_t address = (uintptr_t)base;
  size_t offset = 0;
  if (!psyche_cuda_runtime_mul_size_checked(pitch, row, &offset)) {
    return cudaErrorInvalidValue;
  }
  if (offset > UINTPTR_MAX - address) {
    return cudaErrorInvalidValue;
  }
  *row_ptr = (const void *)(address + offset);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_memcpy_2d_simulated(
    void *dst,
    size_t dpitch,
    const void *src,
    size_t spitch,
    size_t width,
    size_t height,
    cudaMemcpyKind kind) {
  if (kind < cudaMemcpyHostToHost || kind > cudaMemcpyDefault) {
    return cudaErrorInvalidValue;
  }
  if (width == 0 || height == 0) {
    return cudaSuccess;
  }
  if (dst == 0 || src == 0 || width > dpitch || width > spitch) {
    return cudaErrorInvalidValue;
  }

  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  PsycheCudaRuntimeAllocation *dst_expected_allocation = 0;
  PsycheCudaRuntimeAllocation *src_expected_allocation = 0;
  PsycheCudaRuntimeHostAllocation *dst_expected_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *src_expected_mapped_host_allocation = 0;
  int dst_expected_set = 0;
  int src_expected_set = 0;
  for (size_t row = 0; row < height; row++) {
    const void *src_row = 0;
    const void *dst_row_const = 0;
    void *dst_row = 0;
    cudaError_t result =
        psyche_cuda_runtime_offset_pointer(src, spitch, row, &src_row);
    if (result != cudaSuccess) {
      pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
      return result;
    }
    result = psyche_cuda_runtime_offset_pointer(dst, dpitch, row, &dst_row_const);
    if (result != cudaSuccess) {
      pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
      return result;
    }
    dst_row = (void *)dst_row_const;
    result = psyche_cuda_runtime_validate_row_copy_locked(dst_row, src_row, width, kind);
    if (result != cudaSuccess) {
      pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
      return result;
    }
    if (
        kind == cudaMemcpyHostToDevice ||
        kind == cudaMemcpyDeviceToDevice ||
        kind == cudaMemcpyDefault) {
      PsycheCudaRuntimeAllocation *allocation =
          psyche_cuda_runtime_find_allocation_locked(dst_row, width);
      PsycheCudaRuntimeHostAllocation *mapped_host_allocation =
          psyche_cuda_runtime_find_mapped_host_allocation_locked(dst_row, width);
      if (!dst_expected_set) {
        dst_expected_allocation = allocation;
        dst_expected_mapped_host_allocation = mapped_host_allocation;
        dst_expected_set = 1;
      } else if (
          allocation != dst_expected_allocation ||
          mapped_host_allocation != dst_expected_mapped_host_allocation) {
        pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
        return cudaErrorInvalidValue;
      }
    }
    if (
        kind == cudaMemcpyDeviceToHost ||
        kind == cudaMemcpyDeviceToDevice ||
        kind == cudaMemcpyDefault) {
      PsycheCudaRuntimeAllocation *allocation =
          psyche_cuda_runtime_find_allocation_locked(src_row, width);
      PsycheCudaRuntimeHostAllocation *mapped_host_allocation =
          psyche_cuda_runtime_find_mapped_host_allocation_locked(src_row, width);
      if (!src_expected_set) {
        src_expected_allocation = allocation;
        src_expected_mapped_host_allocation = mapped_host_allocation;
        src_expected_set = 1;
      } else if (
          allocation != src_expected_allocation ||
          mapped_host_allocation != src_expected_mapped_host_allocation) {
        pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
        return cudaErrorInvalidValue;
      }
    }
  }
  for (size_t row = 0; row < height; row++) {
    const void *src_row = 0;
    const void *dst_row_const = 0;
    (void)psyche_cuda_runtime_offset_pointer(src, spitch, row, &src_row);
    (void)psyche_cuda_runtime_offset_pointer(dst, dpitch, row, &dst_row_const);
    memmove((void *)dst_row_const, src_row, width);
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_offset_3d_pointer(
    const void *base,
    size_t pitch,
    size_t slice_height,
    size_t z,
    size_t y,
    size_t row,
    size_t x_offset,
    const void **row_ptr) {
  uintptr_t address = (uintptr_t)base;
  size_t slice_rows = 0;
  size_t y_rows = 0;
  size_t row_index = 0;
  size_t row_offset = 0;
  size_t total_offset = 0;
  if (!psyche_cuda_runtime_mul_size_checked(slice_height, z, &slice_rows)) {
    return cudaErrorInvalidValue;
  }
  if (!psyche_cuda_runtime_add_size_checked(slice_rows, y, &y_rows)) {
    return cudaErrorInvalidValue;
  }
  if (!psyche_cuda_runtime_add_size_checked(y_rows, row, &row_index)) {
    return cudaErrorInvalidValue;
  }
  if (!psyche_cuda_runtime_mul_size_checked(pitch, row_index, &row_offset)) {
    return cudaErrorInvalidValue;
  }
  if (!psyche_cuda_runtime_add_size_checked(row_offset, x_offset, &total_offset)) {
    return cudaErrorInvalidValue;
  }
  if (total_offset > UINTPTR_MAX - address) {
    return cudaErrorInvalidValue;
  }
  *row_ptr = (const void *)(address + total_offset);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_validate_3d_side(
    const cudaPitchedPtr *ptr,
    const cudaPos *pos,
    const cudaExtent *extent) {
  size_t required_pitch = 0;
  size_t required_xsize = 0;
  size_t required_height = 0;
  if (!psyche_cuda_runtime_add_size_checked(extent->width, pos->x, &required_pitch)) {
    return cudaErrorInvalidValue;
  }
  if (ptr->pitch < required_pitch) {
    return cudaErrorInvalidValue;
  }
  if (
      ptr->xsize != 0 &&
      (
          !psyche_cuda_runtime_add_size_checked(extent->width, pos->x, &required_xsize) ||
          ptr->xsize < required_xsize)) {
    return cudaErrorInvalidValue;
  }
  if (!psyche_cuda_runtime_add_size_checked(extent->height, pos->y, &required_height)) {
    return cudaErrorInvalidValue;
  }
  if ((extent->depth > 1 || pos->z > 0) && ptr->ysize == 0) {
    return cudaErrorInvalidValue;
  }
  if (ptr->ysize != 0 && ptr->ysize < required_height) {
    return cudaErrorInvalidValue;
  }
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_memcpy_3d_simulated(
    const cudaMemcpy3DParms *params) {
  cudaError_t result = cudaSuccess;
  void *staged_copy = 0;
  size_t staged_rows = 0;
  size_t staged_bytes = 0;

  if (params == 0) {
    return cudaErrorInvalidValue;
  }
  if (params->kind < cudaMemcpyHostToHost || params->kind > cudaMemcpyDefault) {
    return cudaErrorInvalidValue;
  }
  if (params->srcArray != 0 || params->dstArray != 0) {
    return cudaErrorNotSupported;
  }
  if (params->extent.width == 0 || params->extent.height == 0 || params->extent.depth == 0) {
    return cudaSuccess;
  }
  if (params->srcPtr.ptr == 0 || params->dstPtr.ptr == 0) {
    return cudaErrorInvalidValue;
  }

  result =
      psyche_cuda_runtime_validate_3d_side(&params->srcPtr, &params->srcPos, &params->extent);
  if (result != cudaSuccess) {
    return result;
  }
  result =
      psyche_cuda_runtime_validate_3d_side(&params->dstPtr, &params->dstPos, &params->extent);
  if (result != cudaSuccess) {
    return result;
  }
  if (
      !psyche_cuda_runtime_mul_size_checked(
          params->extent.height,
          params->extent.depth,
          &staged_rows) ||
      !psyche_cuda_runtime_mul_size_checked(
          params->extent.width,
          staged_rows,
          &staged_bytes)) {
    return cudaErrorInvalidValue;
  }

  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  PsycheCudaRuntimeAllocation *dst_expected_allocation = 0;
  PsycheCudaRuntimeAllocation *src_expected_allocation = 0;
  PsycheCudaRuntimeHostAllocation *dst_expected_mapped_host_allocation = 0;
  PsycheCudaRuntimeHostAllocation *src_expected_mapped_host_allocation = 0;
  int dst_expected_set = 0;
  int src_expected_set = 0;
  for (size_t z = 0; z < params->extent.depth; z++) {
    for (size_t row = 0; row < params->extent.height; row++) {
      const void *src_row = 0;
      const void *dst_row_const = 0;
      void *dst_row = 0;
      size_t src_z = 0;
      size_t dst_z = 0;
      if (
          !psyche_cuda_runtime_add_size_checked(params->srcPos.z, z, &src_z) ||
          !psyche_cuda_runtime_add_size_checked(params->dstPos.z, z, &dst_z)) {
        pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
        return cudaErrorInvalidValue;
      }
      result = psyche_cuda_runtime_offset_3d_pointer(
          params->srcPtr.ptr,
          params->srcPtr.pitch,
          params->srcPtr.ysize,
          src_z,
          params->srcPos.y,
          row,
          params->srcPos.x,
          &src_row);
      if (result != cudaSuccess) {
        pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
        return result;
      }
      result = psyche_cuda_runtime_offset_3d_pointer(
          params->dstPtr.ptr,
          params->dstPtr.pitch,
          params->dstPtr.ysize,
          dst_z,
          params->dstPos.y,
          row,
          params->dstPos.x,
          &dst_row_const);
      if (result != cudaSuccess) {
        pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
        return result;
      }
      dst_row = (void *)dst_row_const;
      result = psyche_cuda_runtime_validate_row_copy_locked(
          dst_row,
          src_row,
          params->extent.width,
          params->kind);
      if (result != cudaSuccess) {
        pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
        return result;
      }
      if (
          params->kind == cudaMemcpyHostToDevice ||
          params->kind == cudaMemcpyDeviceToDevice ||
          params->kind == cudaMemcpyDefault) {
        PsycheCudaRuntimeAllocation *allocation =
            psyche_cuda_runtime_find_allocation_locked(dst_row, params->extent.width);
        PsycheCudaRuntimeHostAllocation *mapped_host_allocation =
            psyche_cuda_runtime_find_mapped_host_allocation_locked(
                dst_row,
                params->extent.width);
        if (!dst_expected_set) {
          dst_expected_allocation = allocation;
          dst_expected_mapped_host_allocation = mapped_host_allocation;
          dst_expected_set = 1;
        } else if (
            allocation != dst_expected_allocation ||
            mapped_host_allocation != dst_expected_mapped_host_allocation) {
          pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
          return cudaErrorInvalidValue;
        }
      }
      if (
          params->kind == cudaMemcpyDeviceToHost ||
          params->kind == cudaMemcpyDeviceToDevice ||
          params->kind == cudaMemcpyDefault) {
        PsycheCudaRuntimeAllocation *allocation =
            psyche_cuda_runtime_find_allocation_locked(src_row, params->extent.width);
        PsycheCudaRuntimeHostAllocation *mapped_host_allocation =
            psyche_cuda_runtime_find_mapped_host_allocation_locked(
                src_row,
                params->extent.width);
        if (!src_expected_set) {
          src_expected_allocation = allocation;
          src_expected_mapped_host_allocation = mapped_host_allocation;
          src_expected_set = 1;
        } else if (
            allocation != src_expected_allocation ||
            mapped_host_allocation != src_expected_mapped_host_allocation) {
          pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
          return cudaErrorInvalidValue;
        }
      }
    }
  }
  staged_copy = malloc(staged_bytes);
  if (staged_copy == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorMemoryAllocation;
  }
  for (size_t z = 0; z < params->extent.depth; z++) {
    for (size_t row = 0; row < params->extent.height; row++) {
      const void *src_row = 0;
      size_t src_z = 0;
      size_t staged_row = 0;
      (void)psyche_cuda_runtime_add_size_checked(params->srcPos.z, z, &src_z);
      (void)psyche_cuda_runtime_mul_size_checked(params->extent.height, z, &staged_row);
      (void)psyche_cuda_runtime_add_size_checked(staged_row, row, &staged_row);
      (void)psyche_cuda_runtime_offset_3d_pointer(
          params->srcPtr.ptr,
          params->srcPtr.pitch,
          params->srcPtr.ysize,
          src_z,
          params->srcPos.y,
          row,
          params->srcPos.x,
          &src_row);
      memcpy(
          (char *)staged_copy + (staged_row * params->extent.width),
          src_row,
          params->extent.width);
    }
  }
  for (size_t z = 0; z < params->extent.depth; z++) {
    for (size_t row = 0; row < params->extent.height; row++) {
      const void *dst_row_const = 0;
      size_t dst_z = 0;
      size_t staged_row = 0;
      (void)psyche_cuda_runtime_add_size_checked(params->dstPos.z, z, &dst_z);
      (void)psyche_cuda_runtime_mul_size_checked(params->extent.height, z, &staged_row);
      (void)psyche_cuda_runtime_add_size_checked(staged_row, row, &staged_row);
      (void)psyche_cuda_runtime_offset_3d_pointer(
          params->dstPtr.ptr,
          params->dstPtr.pitch,
          params->dstPtr.ysize,
          dst_z,
          params->dstPos.y,
          row,
          params->dstPos.x,
          &dst_row_const);
      memcpy(
          (void *)dst_row_const,
          (const char *)staged_copy + (staged_row * params->extent.width),
          params->extent.width);
    }
  }
  free(staged_copy);
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_memset_simulated(
    void *devPtr,
    int value,
    size_t count) {
  PsycheCudaRuntimeAllocation *allocation = 0;
  PsycheCudaRuntimeHostAllocation *mapped_host_allocation = 0;
  if (count == 0) {
    return cudaSuccess;
  }
  if (devPtr == 0) {
    return cudaErrorInvalidValue;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  allocation = psyche_cuda_runtime_find_allocation_locked(devPtr, count);
  mapped_host_allocation =
      psyche_cuda_runtime_find_mapped_host_allocation_locked(devPtr, count);
  if (allocation == 0 && mapped_host_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return cudaErrorInvalidValue;
  }
  memset(devPtr, (unsigned char)value, count);
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

static cudaError_t psyche_cuda_runtime_memset_2d_simulated(
    void *devPtr,
    size_t pitch,
    int value,
    size_t width,
    size_t height) {
  if (width == 0 || height == 0) {
    return cudaSuccess;
  }
  if (devPtr == 0 || width > pitch) {
    return cudaErrorInvalidValue;
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  PsycheCudaRuntimeAllocation *expected_allocation = 0;
  PsycheCudaRuntimeHostAllocation *expected_mapped_host_allocation = 0;
  int expected_set = 0;
  for (size_t row = 0; row < height; row++) {
    const void *row_ptr_const = 0;
    void *row_ptr = 0;
    PsycheCudaRuntimeAllocation *allocation = 0;
    PsycheCudaRuntimeHostAllocation *mapped_host_allocation = 0;
    cudaError_t result =
        psyche_cuda_runtime_offset_pointer(devPtr, pitch, row, &row_ptr_const);
    if (result != cudaSuccess) {
      pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
      return result;
    }
    row_ptr = (void *)row_ptr_const;
    allocation = psyche_cuda_runtime_find_allocation_locked(row_ptr, width);
    mapped_host_allocation =
        psyche_cuda_runtime_find_mapped_host_allocation_locked(row_ptr, width);
    if (allocation == 0 && mapped_host_allocation == 0) {
      pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
      return cudaErrorInvalidValue;
    }
    if (!expected_set) {
      expected_allocation = allocation;
      expected_mapped_host_allocation = mapped_host_allocation;
      expected_set = 1;
    } else if (
        allocation != expected_allocation ||
        mapped_host_allocation != expected_mapped_host_allocation) {
      pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
      return cudaErrorInvalidValue;
    }
  }
  for (size_t row = 0; row < height; row++) {
    const void *row_ptr_const = 0;
    (void)psyche_cuda_runtime_offset_pointer(devPtr, pitch, row, &row_ptr_const);
    memset((void *)row_ptr_const, (unsigned char)value, width);
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return cudaSuccess;
}

PSYCHE_CUDA_STUB_API cudaError_t cudaGetDeviceCount(int *count) {
  if (count == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *count = 0;
  return psyche_cuda_runtime_record(cudaSuccess);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaGetDevice(int *device) {
  if (device == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *device = -1;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaSetDevice(int device) {
  (void)device;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaGetDeviceProperties(void *prop, int device) {
  (void)device;
  if (prop == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaDeviceGetAttribute(int *value, int attr, int device) {
  (void)attr;
  (void)device;
  if (value == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *value = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemGetInfo(size_t *free, size_t *total) {
  if (free == 0 || total == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  /* Successful accounting with zero bytes is not evidence of a CUDA device. */
  *free = 0;
  *total = 0;
  return psyche_cuda_runtime_record(cudaSuccess);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaDriverGetVersion(int *driverVersion) {
  if (driverVersion == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  /* Version probes are allowed, but version 0 means "stub/no driver". */
  *driverVersion = 0;
  return psyche_cuda_runtime_record(cudaSuccess);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaRuntimeGetVersion(int *runtimeVersion) {
  if (runtimeVersion == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  /* Version probes are allowed, but version 0 means "stub/no runtime". */
  *runtimeVersion = 0;
  return psyche_cuda_runtime_record(cudaSuccess);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaFree(void *devPtr) {
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(psyche_cuda_runtime_free_simulated(
        devPtr,
        cudaErrorInvalidValue,
        cudaSuccess));
  }
  return psyche_cuda_runtime_record(psyche_cuda_runtime_free_simulated(
      devPtr,
      cudaErrorNoDevice,
      cudaErrorNoDevice));
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMalloc(void **devPtr, size_t size) {
  if (devPtr == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(psyche_cuda_runtime_malloc_simulated(devPtr, size));
  }
  (void)size;
  *devPtr = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMallocAsync(
    void **devPtr,
    size_t size,
    cudaStream_t stream) {
  cudaError_t stream_result = cudaSuccess;
  if (devPtr == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    *devPtr = 0;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  stream_result = psyche_cuda_runtime_validate_async_stream(stream);
  if (stream_result != cudaSuccess) {
    *devPtr = 0;
    return psyche_cuda_runtime_record(stream_result);
  }
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_malloc_kind_simulated(devPtr, size, 0, 0, 1, 1));
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMallocFromPoolAsync(
    void **ptr,
    size_t size,
    cudaMemPool_t memPool,
    cudaStream_t stream) {
  cudaError_t stream_result = cudaSuccess;
  if (ptr == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    *ptr = 0;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  stream_result = psyche_cuda_runtime_validate_async_stream(stream);
  if (stream_result != cudaSuccess) {
    *ptr = 0;
    return psyche_cuda_runtime_record(stream_result);
  }
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_malloc_kind_simulated(ptr, size, 0, memPool, 0, 1));
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMallocManaged(
    void **devPtr,
    size_t size,
    unsigned int flags) {
  if (devPtr == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_malloc_managed_simulated(devPtr, size, flags));
  }
  (void)size;
  (void)flags;
  *devPtr = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMallocPitch(
    void **devPtr,
    size_t *pitch,
    size_t width,
    size_t height) {
  if (devPtr == 0 || pitch == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_malloc_pitch_simulated(devPtr, pitch, width, height));
  }
  (void)width;
  (void)height;
  *devPtr = 0;
  *pitch = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaFreeAsync(void *devPtr, cudaStream_t stream) {
  cudaError_t stream_result = cudaSuccess;
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(devPtr == 0 ? cudaErrorInvalidValue : cudaErrorNoDevice);
  }
  stream_result = psyche_cuda_runtime_validate_async_stream(stream);
  if (stream_result != cudaSuccess) {
    return psyche_cuda_runtime_record(stream_result);
  }
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_free_simulated(devPtr, cudaErrorInvalidValue, cudaErrorInvalidValue));
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolCreate(
    cudaMemPool_t *memPool,
    const cudaMemPoolProps *poolProps) {
  if (memPool == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    *memPool = 0;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_mem_pool_create_simulated(memPool, poolProps));
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolDestroy(cudaMemPool_t memPool) {
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_mem_pool_destroy_simulated(memPool));
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolGetAttribute(
    cudaMemPool_t memPool,
    cudaMemPoolAttr attr,
    void *value) {
  cudaError_t result = cudaSuccess;
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  result = psyche_cuda_runtime_mem_pool_get_attribute_locked(memPool, attr, value);
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(result);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolSetAttribute(
    cudaMemPool_t memPool,
    cudaMemPoolAttr attr,
    void *value) {
  cudaError_t result = cudaSuccess;
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  result = psyche_cuda_runtime_mem_pool_set_attribute_locked(memPool, attr, value);
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(result);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolTrimTo(
    cudaMemPool_t memPool,
    size_t minBytesToKeep) {
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_mem_pool_trim_to_simulated(memPool, minBytesToKeep));
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemGetDefaultMemPool(
    cudaMemPool_t *memPool,
    cudaMemLocation *location,
    cudaMemAllocationType type) {
  cudaError_t result = cudaSuccess;
  if (memPool == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    *memPool = 0;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  result = psyche_cuda_runtime_validate_mem_pool_location_request(location, type);
  if (result != cudaSuccess) {
    *memPool = 0;
    return psyche_cuda_runtime_record(result);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  *memPool = psyche_cuda_runtime_default_pool_locked(type)->handle;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(cudaSuccess);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemGetMemPool(
    cudaMemPool_t *memPool,
    cudaMemLocation *location,
    cudaMemAllocationType type) {
  cudaError_t result = cudaSuccess;
  if (memPool == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    *memPool = 0;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  result = psyche_cuda_runtime_validate_mem_pool_location_request(location, type);
  if (result != cudaSuccess) {
    *memPool = 0;
    return psyche_cuda_runtime_record(result);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  *memPool = psyche_cuda_runtime_current_pool_locked(type)->handle;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(cudaSuccess);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemSetMemPool(
    cudaMemLocation *location,
    cudaMemAllocationType type,
    cudaMemPool_t memPool) {
  PsycheCudaRuntimeMemoryPool *record = 0;
  cudaError_t result = cudaSuccess;
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  result = psyche_cuda_runtime_validate_mem_pool_location_request(location, type);
  if (result != cudaSuccess) {
    return psyche_cuda_runtime_record(result);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  record = psyche_cuda_runtime_find_pool_locked(memPool);
  if (record == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return psyche_cuda_runtime_record(
        memPool == 0 ? cudaErrorInvalidValue : cudaErrorInvalidResourceHandle);
  }
  if (record->alloc_type != type || record->location.type != location->type) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (type == cudaMemAllocationTypeManaged) {
    psyche_cuda_runtime_current_managed_pool = record->handle;
  } else {
    psyche_cuda_runtime_current_host_pool = record->handle;
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(cudaSuccess);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaDeviceGetDefaultMemPool(
    cudaMemPool_t *memPool,
    int device) {
  (void)device;
  if (memPool == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *memPool = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaDeviceGetMemPool(cudaMemPool_t *memPool, int device) {
  (void)device;
  if (memPool == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *memPool = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaDeviceSetMemPool(int device, cudaMemPool_t memPool) {
  (void)device;
  (void)memPool;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolGetAccess(
    cudaMemAccessFlags *flags,
    cudaMemPool_t memPool,
    cudaMemLocation *location) {
  (void)memPool;
  (void)location;
  if (flags == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *flags = cudaMemAccessFlagsProtNone;
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_simulated_memory_enabled() ?
      cudaErrorNotSupported :
      cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolSetAccess(
    cudaMemPool_t memPool,
    const cudaMemAccessDesc *descList,
    size_t count) {
  (void)memPool;
  (void)descList;
  (void)count;
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_simulated_memory_enabled() ?
      cudaErrorNotSupported :
      cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolExportToShareableHandle(
    void *shareableHandle,
    cudaMemPool_t memPool,
    cudaMemAllocationHandleType handleType,
    unsigned int flags) {
  (void)memPool;
  (void)handleType;
  (void)flags;
  if (shareableHandle == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  memset(shareableHandle, 0, sizeof(void *));
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_simulated_memory_enabled() ?
      cudaErrorNotSupported :
      cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolImportFromShareableHandle(
    cudaMemPool_t *memPool,
    void *shareableHandle,
    cudaMemAllocationHandleType handleType,
    unsigned int flags) {
  (void)shareableHandle;
  (void)handleType;
  (void)flags;
  if (memPool == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *memPool = 0;
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_simulated_memory_enabled() ?
      cudaErrorNotSupported :
      cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolExportPointer(
    cudaMemPoolPtrExportData *exportData,
    void *ptr) {
  (void)ptr;
  if (exportData == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  memset(exportData, 0, sizeof(*exportData));
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_simulated_memory_enabled() ?
      cudaErrorNotSupported :
      cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPoolImportPointer(
    void **ptr,
    cudaMemPool_t memPool,
    cudaMemPoolPtrExportData *exportData) {
  (void)memPool;
  (void)exportData;
  if (ptr == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *ptr = 0;
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_simulated_memory_enabled() ?
      cudaErrorNotSupported :
      cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemAdvise(
    const void *devPtr,
    size_t count,
    cudaMemoryAdvise advice,
    int device) {
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_mem_advise_simulated(devPtr, count, advice, device));
  }
  (void)devPtr;
  (void)count;
  (void)advice;
  (void)device;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemPrefetchAsync(
    const void *devPtr,
    size_t count,
    int dstDevice,
    cudaStream_t stream) {
  cudaError_t stream_result = cudaSuccess;
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    stream_result = psyche_cuda_runtime_validate_async_stream(stream);
    if (stream_result != cudaSuccess) {
      return psyche_cuda_runtime_record(stream_result);
    }
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_mem_prefetch_simulated(devPtr, count, dstDevice));
  }
  (void)devPtr;
  (void)count;
  (void)dstDevice;
  (void)stream;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemRangeGetAttribute(
    void *data,
    size_t dataSize,
    cudaMemRangeAttribute attribute,
    const void *devPtr,
    size_t count) {
  cudaError_t result = cudaSuccess;
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  result = psyche_cuda_runtime_mem_range_get_attribute_locked(
      data,
      dataSize,
      attribute,
      devPtr,
      count);
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(result);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemRangeGetAttributes(
    void **data,
    size_t *dataSizes,
    cudaMemRangeAttribute *attributes,
    size_t numAttributes,
    const void *devPtr,
    size_t count) {
  cudaError_t result = cudaSuccess;
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  if (numAttributes == 0) {
    return psyche_cuda_runtime_record(cudaSuccess);
  }
  if (data == 0 || dataSizes == 0 || attributes == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  for (size_t index = 0; index < numAttributes; index++) {
    result = psyche_cuda_runtime_mem_range_get_attribute_locked(
        data[index],
        dataSizes[index],
        attributes[index],
        devPtr,
        count);
    if (result != cudaSuccess) {
      break;
    }
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(result);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaPointerGetAttributes(
    cudaPointerAttributes *attributes,
    const void *ptr) {
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  return psyche_cuda_runtime_record(
      psyche_cuda_runtime_pointer_get_attributes_simulated(attributes, ptr));
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMallocHost(void **ptr, size_t size) {
  if (ptr == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_host_alloc_simulated(ptr, size, cudaHostAllocDefault, 0));
  }
  (void)size;
  *ptr = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaHostAlloc(
    void **pHost,
    size_t size,
    unsigned int flags) {
  if (pHost == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(psyche_cuda_runtime_host_alloc_simulated(
        pHost,
        size,
        flags,
        (flags & cudaHostAllocMapped) != 0));
  }
  (void)size;
  (void)flags;
  *pHost = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaFreeHost(void *ptr) {
  cudaError_t cleanup_result = psyche_cuda_runtime_free_host_simulated(
      ptr,
      cudaErrorInvalidValue,
      cudaErrorInvalidValue);
  if (cleanup_result == cudaSuccess) {
    return psyche_cuda_runtime_record(cudaSuccess);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cleanup_result);
  }
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaHostRegister(
    void *ptr,
    size_t size,
    unsigned int flags) {
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_host_register_simulated(ptr, size, flags));
  }
  (void)ptr;
  (void)size;
  (void)flags;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaHostUnregister(void *ptr) {
  cudaError_t cleanup_result = psyche_cuda_runtime_host_unregister_simulated(
      ptr,
      cudaErrorHostMemoryNotRegistered,
      cudaErrorInvalidValue);
  if (cleanup_result == cudaSuccess) {
    return psyche_cuda_runtime_record(cudaSuccess);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cleanup_result);
  }
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaHostGetFlags(
    unsigned int *pFlags,
    void *pHost) {
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_host_get_flags_simulated(pFlags, pHost));
  }
  if (pFlags == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *pFlags = 0;
  (void)pHost;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaHostGetDevicePointer(
    void **pDevice,
    void *pHost,
    unsigned int flags) {
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_host_get_device_pointer_simulated(pDevice, pHost, flags));
  }
  if (pDevice == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *pDevice = 0;
  (void)pHost;
  (void)flags;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemcpy(
    void *dst,
    const void *src,
    size_t count,
    cudaMemcpyKind kind) {
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_memcpy_simulated(dst, src, count, kind));
  }
  if (kind == cudaMemcpyHostToHost) {
    if (count == 0) {
      return psyche_cuda_runtime_record(cudaSuccess);
    }
    if (dst == 0 || src == 0) {
      return psyche_cuda_runtime_record(cudaErrorInvalidValue);
    }
    memmove(dst, src, count);
    return psyche_cuda_runtime_record(cudaSuccess);
  }
  if (kind < cudaMemcpyHostToHost || kind > cudaMemcpyDefault) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemcpyAsync(
    void *dst,
    const void *src,
    size_t count,
    cudaMemcpyKind kind,
    cudaStream_t stream) {
  cudaError_t stream_result = psyche_cuda_runtime_validate_async_stream(stream);
  if (stream_result != cudaSuccess) {
    return psyche_cuda_runtime_record(stream_result);
  }
  if (kind < cudaMemcpyHostToHost || kind > cudaMemcpyDefault) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_memcpy_simulated(dst, src, count, kind));
  }
  if (kind == cudaMemcpyHostToHost) {
    if (count == 0) {
      return psyche_cuda_runtime_record(cudaSuccess);
    }
    if (dst == 0 || src == 0) {
      return psyche_cuda_runtime_record(cudaErrorInvalidValue);
    }
    memmove(dst, src, count);
    return psyche_cuda_runtime_record(cudaSuccess);
  }
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemcpy2D(
    void *dst,
    size_t dpitch,
    const void *src,
    size_t spitch,
    size_t width,
    size_t height,
    cudaMemcpyKind kind) {
  if (kind < cudaMemcpyHostToHost || kind > cudaMemcpyDefault) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(psyche_cuda_runtime_memcpy_2d_simulated(
        dst,
        dpitch,
        src,
        spitch,
        width,
        height,
        kind));
  }
  if (width == 0 || height == 0) {
    return psyche_cuda_runtime_record(cudaSuccess);
  }
  if (kind == cudaMemcpyHostToHost) {
    if (dst == 0 || src == 0 || width > dpitch || width > spitch) {
      return psyche_cuda_runtime_record(cudaErrorInvalidValue);
    }
    for (size_t row = 0; row < height; row++) {
      const void *src_row = 0;
      const void *dst_row_const = 0;
      if (
          psyche_cuda_runtime_offset_pointer(src, spitch, row, &src_row) != cudaSuccess ||
          psyche_cuda_runtime_offset_pointer(dst, dpitch, row, &dst_row_const) != cudaSuccess) {
        return psyche_cuda_runtime_record(cudaErrorInvalidValue);
      }
    }
    for (size_t row = 0; row < height; row++) {
      const void *src_row = 0;
      const void *dst_row_const = 0;
      unsigned char *dst_row = 0;
      (void)psyche_cuda_runtime_offset_pointer(src, spitch, row, &src_row);
      (void)psyche_cuda_runtime_offset_pointer(dst, dpitch, row, &dst_row_const);
      dst_row = (unsigned char *)dst_row_const;
      memmove(dst_row, src_row, width);
    }
    return psyche_cuda_runtime_record(cudaSuccess);
  }
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemcpy2DAsync(
    void *dst,
    size_t dpitch,
    const void *src,
    size_t spitch,
    size_t width,
    size_t height,
    cudaMemcpyKind kind,
    cudaStream_t stream) {
  cudaError_t stream_result = psyche_cuda_runtime_validate_async_stream(stream);
  if (stream_result != cudaSuccess) {
    return psyche_cuda_runtime_record(stream_result);
  }
  return cudaMemcpy2D(dst, dpitch, src, spitch, width, height, kind);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemcpy3D(const cudaMemcpy3DParms *p) {
  if (p == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (p->kind < cudaMemcpyHostToHost || p->kind > cudaMemcpyDefault) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled() || p->kind == cudaMemcpyHostToHost) {
    return psyche_cuda_runtime_record(psyche_cuda_runtime_memcpy_3d_simulated(p));
  }
  if (p->extent.width == 0 || p->extent.height == 0 || p->extent.depth == 0) {
    return psyche_cuda_runtime_record(cudaSuccess);
  }
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemcpy3DAsync(
    const cudaMemcpy3DParms *p,
    cudaStream_t stream) {
  cudaError_t stream_result = psyche_cuda_runtime_validate_async_stream(stream);
  if (stream_result != cudaSuccess) {
    return psyche_cuda_runtime_record(stream_result);
  }
  return cudaMemcpy3D(p);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemset(
    void *devPtr,
    int value,
    size_t count) {
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_memset_simulated(devPtr, value, count));
  }
  (void)devPtr;
  (void)value;
  (void)count;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemset2D(
    void *devPtr,
    size_t pitch,
    int value,
    size_t width,
    size_t height) {
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_memset_2d_simulated(devPtr, pitch, value, width, height));
  }
  (void)devPtr;
  (void)pitch;
  (void)value;
  (void)width;
  (void)height;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemsetAsync(
    void *devPtr,
    int value,
    size_t count,
    cudaStream_t stream) {
  cudaError_t stream_result = psyche_cuda_runtime_validate_async_stream(stream);
  if (stream_result != cudaSuccess) {
    return psyche_cuda_runtime_record(stream_result);
  }
  if (psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_memset_simulated(devPtr, value, count));
  }
  (void)devPtr;
  (void)value;
  (void)count;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaMemset2DAsync(
    void *devPtr,
    size_t pitch,
    int value,
    size_t width,
    size_t height,
    cudaStream_t stream) {
  cudaError_t stream_result = psyche_cuda_runtime_validate_async_stream(stream);
  if (stream_result != cudaSuccess) {
    return psyche_cuda_runtime_record(stream_result);
  }
  return cudaMemset2D(devPtr, pitch, value, width, height);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaDeviceSynchronize(void) {
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaDeviceReset(void) {
  if (
      !psyche_cuda_runtime_simulated_memory_enabled() &&
      !psyche_cuda_runtime_has_simulated_state()) {
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  psyche_cuda_runtime_free_all_simulated();
  return psyche_cuda_runtime_record(cudaSuccess);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaStreamCreate(cudaStream_t *pStream) {
  return cudaStreamCreateWithFlags(pStream, cudaStreamDefault);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaStreamCreateWithFlags(
    cudaStream_t *pStream,
    unsigned int flags) {
  if (pStream == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_sync_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_create_stream_simulated(pStream, flags, 0));
  }
  *pStream = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaStreamCreateWithPriority(
    cudaStream_t *pStream,
    unsigned int flags,
    int priority) {
  if (pStream == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_sync_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_create_stream_simulated(pStream, flags, priority));
  }
  *pStream = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaStreamDestroy(cudaStream_t stream) {
  if (psyche_cuda_runtime_simulated_sync_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_destroy_stream_simulated(stream));
  }
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaStreamSynchronize(cudaStream_t stream) {
  if (psyche_cuda_runtime_simulated_sync_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_validate_stream_simulated(stream));
  }
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaStreamQuery(cudaStream_t stream) {
  if (psyche_cuda_runtime_simulated_sync_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_validate_stream_simulated(stream));
  }
  (void)stream;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaStreamGetFlags(
    cudaStream_t stream,
    unsigned int *flags) {
  PsycheCudaRuntimeStream *record = 0;
  cudaError_t result = cudaSuccess;
  if (flags == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *flags = 0;
  if (!psyche_cuda_runtime_simulated_sync_enabled()) {
    (void)stream;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  if (stream == 0) {
    return psyche_cuda_runtime_record(cudaSuccess);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  record = psyche_cuda_runtime_find_stream_locked(stream);
  if (record == 0) {
    result = cudaErrorInvalidResourceHandle;
  } else {
    *flags = record->flags;
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(result);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaStreamGetPriority(
    cudaStream_t stream,
    int *priority) {
  PsycheCudaRuntimeStream *record = 0;
  cudaError_t result = cudaSuccess;
  if (priority == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *priority = 0;
  if (!psyche_cuda_runtime_simulated_sync_enabled()) {
    (void)stream;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  if (stream == 0) {
    return psyche_cuda_runtime_record(cudaSuccess);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  record = psyche_cuda_runtime_find_stream_locked(stream);
  if (record == 0) {
    result = cudaErrorInvalidResourceHandle;
  } else {
    *priority = record->priority;
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(result);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaDeviceGetStreamPriorityRange(
    int *leastPriority,
    int *greatestPriority) {
  if (leastPriority == 0 || greatestPriority == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *leastPriority = 0;
  *greatestPriority = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaEventCreate(cudaEvent_t *event) {
  return cudaEventCreateWithFlags(event, cudaEventDefault);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaEventCreateWithFlags(
    cudaEvent_t *event,
    unsigned int flags) {
  if (event == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (psyche_cuda_runtime_simulated_sync_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_create_event_simulated(event, flags));
  }
  *event = 0;
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaEventDestroy(cudaEvent_t event) {
  if (psyche_cuda_runtime_simulated_sync_enabled()) {
    return psyche_cuda_runtime_record(
        psyche_cuda_runtime_destroy_event_simulated(event));
  }
  return psyche_cuda_runtime_record(cudaErrorNoDevice);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaEventRecord(cudaEvent_t event, cudaStream_t stream) {
  return cudaEventRecordWithFlags(event, stream, 0);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaEventRecordWithFlags(
    cudaEvent_t event,
    cudaStream_t stream,
    unsigned int flags) {
  PsycheCudaRuntimeEvent *record = 0;
  cudaError_t stream_result = cudaSuccess;
  if (flags != 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  if (!psyche_cuda_runtime_simulated_sync_enabled()) {
    (void)event;
    (void)stream;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  stream_result = psyche_cuda_runtime_validate_stream_simulated(stream);
  if (stream_result != cudaSuccess) {
    return psyche_cuda_runtime_record(stream_result);
  }
  if (event == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidResourceHandle);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  record = psyche_cuda_runtime_find_event_locked(event);
  if (record == 0) {
    pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
    return psyche_cuda_runtime_record(cudaErrorInvalidResourceHandle);
  }
  psyche_cuda_runtime_now(&record->recorded_at);
  record->recorded = 1;
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(cudaSuccess);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaEventSynchronize(cudaEvent_t event) {
  cudaError_t query_result = cudaSuccess;
  if (!psyche_cuda_runtime_simulated_sync_enabled()) {
    (void)event;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  query_result = cudaEventQuery(event);
  return psyche_cuda_runtime_record(query_result);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaEventQuery(cudaEvent_t event) {
  PsycheCudaRuntimeEvent *record = 0;
  cudaError_t result = cudaSuccess;
  if (!psyche_cuda_runtime_simulated_sync_enabled()) {
    (void)event;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  if (event == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidResourceHandle);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  record = psyche_cuda_runtime_find_event_locked(event);
  if (record == 0) {
    result = cudaErrorInvalidResourceHandle;
  } else if (!record->recorded) {
    result = cudaErrorNotReady;
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(result);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaEventElapsedTime(
    float *ms,
    cudaEvent_t start,
    cudaEvent_t end) {
  PsycheCudaRuntimeEvent *start_record = 0;
  PsycheCudaRuntimeEvent *end_record = 0;
  cudaError_t result = cudaSuccess;
  if (ms == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidValue);
  }
  *ms = 0.0f;
  if (!psyche_cuda_runtime_simulated_sync_enabled()) {
    (void)start;
    (void)end;
    return psyche_cuda_runtime_record(cudaErrorNoDevice);
  }
  if (start == 0 || end == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidResourceHandle);
  }
  pthread_mutex_lock(&psyche_cuda_runtime_allocation_mutex);
  start_record = psyche_cuda_runtime_find_event_locked(start);
  end_record = psyche_cuda_runtime_find_event_locked(end);
  if (
      start_record == 0 ||
      end_record == 0 ||
      !start_record->recorded ||
      !end_record->recorded ||
      (start_record->flags & cudaEventDisableTiming) != 0 ||
      (end_record->flags & cudaEventDisableTiming) != 0) {
    result = cudaErrorInvalidResourceHandle;
  } else {
    *ms = psyche_cuda_runtime_elapsed_ms(
        &start_record->recorded_at,
        &end_record->recorded_at);
    if (*ms < 0.0f) {
      *ms = 0.0f;
    }
  }
  pthread_mutex_unlock(&psyche_cuda_runtime_allocation_mutex);
  return psyche_cuda_runtime_record(result);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaLaunchKernel(
    const void *func,
    dim3 gridDim,
    dim3 blockDim,
    void **args,
    size_t sharedMem,
    cudaStream_t stream) {
  const PsycheCudaRuntimeKernelDescriptor *descriptor = 0;
  cudaError_t stream_result = cudaSuccess;
  if (!psyche_cuda_runtime_simulated_memory_enabled()) {
    return psyche_cuda_runtime_record(cudaErrorNotSupported);
  }
  if (func == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidDeviceFunction);
  }
  descriptor = psyche_cuda_runtime_find_kernel_descriptor(func);
  if (descriptor == 0) {
    return psyche_cuda_runtime_record(cudaErrorInvalidDeviceFunction);
  }
  stream_result = psyche_cuda_runtime_validate_stream_simulated(stream);
  if (stream_result != cudaSuccess) {
    return psyche_cuda_runtime_record(stream_result);
  }
  if (sharedMem != 0) {
    return psyche_cuda_runtime_record(cudaErrorNotSupported);
  }
  return psyche_cuda_runtime_record(descriptor->launch(gridDim, blockDim, args));
}

PSYCHE_CUDA_STUB_API cudaError_t cudaPeekAtLastError(void) {
  return (cudaError_t)atomic_load(&psyche_cuda_runtime_last_error);
}

PSYCHE_CUDA_STUB_API cudaError_t cudaGetLastError(void) {
  return (cudaError_t)atomic_exchange(&psyche_cuda_runtime_last_error, cudaSuccess);
}

PSYCHE_CUDA_STUB_API const char *cudaGetErrorName(cudaError_t error) {
  return psyche_cuda_stub_error_name((int)error);
}

PSYCHE_CUDA_STUB_API const char *cudaGetErrorString(cudaError_t error) {
  return psyche_cuda_stub_error_string((int)error);
}
