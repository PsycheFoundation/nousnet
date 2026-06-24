#define _POSIX_C_SOURCE 200809L

#include "cuda_compat_stub.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

_Static_assert(
    sizeof(CUdeviceptr) >= sizeof(uintptr_t),
    "CUdeviceptr must hold host pointer values in simulated memory mode");

static _Atomic int psyche_cuda_driver_initialized = 0;

#if defined(__APPLE__)
CUresult psyche_cuda_metal_launch_vector_add_f32(
    const float *a,
    const float *b,
    float *out,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX);
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
CUresult psyche_cuda_metal_launch_axpby_f32(
    float *x,
    const float *y,
    float alpha,
    float beta,
    unsigned int n,
    size_t bytes,
    unsigned int gridDimX,
    unsigned int blockDimX);
#endif

enum {
  PSYCHE_CUDA_MODULE_MAGIC = 0x5053594D,
  PSYCHE_CUDA_FUNCTION_MAGIC = 0x50535946,
  PSYCHE_CUDA_DRIVER_MODULE_FUNCTION_COUNT = 4
};

typedef struct PsycheCudaDriverAllocation {
  CUdeviceptr dptr;
  void *ptr;
  size_t size;
  unsigned long long buffer_id;
  CUmemoryPool pool;
  int managed;
  int async_alloc;
  int read_mostly;
  int preferred_location;
  int accessed_by;
  int last_prefetch_location;
  int sync_memops;
  struct PsycheCudaDriverAllocation *next;
} PsycheCudaDriverAllocation;

typedef struct PsycheCudaDriverHostAllocation {
  void *ptr;
  size_t size;
  unsigned int flags;
  int owns_memory;
  int registered;
  int device_mapped;
  struct PsycheCudaDriverHostAllocation *next;
} PsycheCudaDriverHostAllocation;

typedef enum PsycheCudaDriverRangeRelation {
  PSYCHE_CUDA_DRIVER_RANGE_NO_OVERLAP = 0,
  PSYCHE_CUDA_DRIVER_RANGE_EXACT_ALIAS = 1,
  PSYCHE_CUDA_DRIVER_RANGE_PARTIAL_OVERLAP = 2,
} PsycheCudaDriverRangeRelation;

typedef struct PsycheCudaDriverStream {
  CUstream handle;
  unsigned int flags;
  int priority;
  struct PsycheCudaDriverStream *next;
} PsycheCudaDriverStream;

typedef struct PsycheCudaDriverEvent {
  CUevent handle;
  unsigned int flags;
  int recorded;
  struct timespec recorded_at;
  struct PsycheCudaDriverEvent *next;
} PsycheCudaDriverEvent;

typedef struct PsycheCudaDriverMemoryPool {
  CUmemoryPool handle;
  int is_default;
  CUmemAllocationType alloc_type;
  CUmemAllocationHandleType handle_types;
  CUmemLocation location;
  size_t max_size;
  cuuint64_t release_threshold;
  cuuint64_t reserved_current;
  cuuint64_t reserved_high;
  cuuint64_t used_current;
  cuuint64_t used_high;
  int reuse_follow_event_dependencies;
  int reuse_allow_opportunistic;
  int reuse_allow_internal_dependencies;
  struct PsycheCudaDriverMemoryPool *next;
} PsycheCudaDriverMemoryPool;

typedef enum {
  PSYCHE_CUDA_KERNEL_VECTOR_ADD_F32 = 1,
  PSYCHE_CUDA_KERNEL_SAXPY_F32 = 2,
  PSYCHE_CUDA_KERNEL_SCALE_F32 = 3,
  PSYCHE_CUDA_KERNEL_AXPBY_F32 = 4
} PsycheCudaDriverKernelKind;

typedef enum {
  PSYCHE_CUDA_KERNEL_PARAM_BUFFER_F32_RO = 1,
  PSYCHE_CUDA_KERNEL_PARAM_BUFFER_F32_RW = 2,
  PSYCHE_CUDA_KERNEL_PARAM_SCALAR_F32 = 3,
  PSYCHE_CUDA_KERNEL_PARAM_U32 = 4
} PsycheCudaDriverKernelParamKind;

typedef CUresult (*PsycheCudaDriverKernelLaunchFn)(
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    void **kernelParams);

typedef struct PsycheCudaDriverKernelDescriptor {
  PsycheCudaDriverKernelKind kind;
  const char *name;
  unsigned int param_count;
  PsycheCudaDriverKernelParamKind params[5];
  PsycheCudaDriverKernelLaunchFn launch;
} PsycheCudaDriverKernelDescriptor;

typedef struct PsycheCudaDriverFunction {
  unsigned int magic;
  CUfunction handle;
  PsycheCudaDriverKernelKind kind;
  const PsycheCudaDriverKernelDescriptor *descriptor;
  char name[64];
  struct PsycheCudaDriverModule *module;
} PsycheCudaDriverFunction;

typedef struct PsycheCudaDriverModule {
  unsigned int magic;
  CUmodule handle;
  PsycheCudaDriverFunction functions[PSYCHE_CUDA_DRIVER_MODULE_FUNCTION_COUNT];
  struct PsycheCudaDriverModule *next;
} PsycheCudaDriverModule;

static pthread_mutex_t psyche_cuda_driver_allocation_mutex = PTHREAD_MUTEX_INITIALIZER;
static PsycheCudaDriverAllocation *psyche_cuda_driver_allocations = 0;
static PsycheCudaDriverHostAllocation *psyche_cuda_driver_host_allocations = 0;
static PsycheCudaDriverStream *psyche_cuda_driver_streams = 0;
static PsycheCudaDriverEvent *psyche_cuda_driver_events = 0;
static PsycheCudaDriverMemoryPool *psyche_cuda_driver_memory_pools = 0;
static PsycheCudaDriverModule *psyche_cuda_driver_modules = 0;
static PsycheCudaDriverMemoryPool psyche_cuda_driver_default_host_pool;
static PsycheCudaDriverMemoryPool psyche_cuda_driver_default_managed_pool;
static CUmemoryPool psyche_cuda_driver_current_host_pool = 0;
static CUmemoryPool psyche_cuda_driver_current_managed_pool = 0;
static uintptr_t psyche_cuda_driver_next_stream_handle = 0xD50000000001ULL;
static uintptr_t psyche_cuda_driver_next_event_handle = 0xD60000000001ULL;
static unsigned long long psyche_cuda_driver_next_buffer_id = 1;

static CUresult psyche_cuda_driver_launch_vector_add_f32(
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    void **kernelParams);
static CUresult psyche_cuda_driver_launch_saxpy_f32(
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    void **kernelParams);
static CUresult psyche_cuda_driver_launch_scale_f32(
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    void **kernelParams);
static CUresult psyche_cuda_driver_launch_axpby_f32(
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    void **kernelParams);

static const PsycheCudaDriverKernelDescriptor psyche_cuda_driver_kernel_descriptors[] = {
    {
        PSYCHE_CUDA_KERNEL_VECTOR_ADD_F32,
        "vector_add_f32",
        4,
        {
            PSYCHE_CUDA_KERNEL_PARAM_BUFFER_F32_RO,
            PSYCHE_CUDA_KERNEL_PARAM_BUFFER_F32_RO,
            PSYCHE_CUDA_KERNEL_PARAM_BUFFER_F32_RW,
            PSYCHE_CUDA_KERNEL_PARAM_U32
        },
        psyche_cuda_driver_launch_vector_add_f32
    },
    {
        PSYCHE_CUDA_KERNEL_SAXPY_F32,
        "saxpy_f32",
        4,
        {
            PSYCHE_CUDA_KERNEL_PARAM_BUFFER_F32_RO,
            PSYCHE_CUDA_KERNEL_PARAM_BUFFER_F32_RW,
            PSYCHE_CUDA_KERNEL_PARAM_SCALAR_F32,
            PSYCHE_CUDA_KERNEL_PARAM_U32
        },
        psyche_cuda_driver_launch_saxpy_f32
    },
    {
        PSYCHE_CUDA_KERNEL_SCALE_F32,
        "scale_f32",
        3,
        {
            PSYCHE_CUDA_KERNEL_PARAM_BUFFER_F32_RW,
            PSYCHE_CUDA_KERNEL_PARAM_SCALAR_F32,
            PSYCHE_CUDA_KERNEL_PARAM_U32
        },
        psyche_cuda_driver_launch_scale_f32
    },
    {
        PSYCHE_CUDA_KERNEL_AXPBY_F32,
        "axpby_f32",
        5,
        {
            PSYCHE_CUDA_KERNEL_PARAM_BUFFER_F32_RW,
            PSYCHE_CUDA_KERNEL_PARAM_BUFFER_F32_RO,
            PSYCHE_CUDA_KERNEL_PARAM_SCALAR_F32,
            PSYCHE_CUDA_KERNEL_PARAM_SCALAR_F32,
            PSYCHE_CUDA_KERNEL_PARAM_U32
        },
        psyche_cuda_driver_launch_axpby_f32
    }
};

_Static_assert(
    sizeof(psyche_cuda_driver_kernel_descriptors) /
        sizeof(psyche_cuda_driver_kernel_descriptors[0]) ==
        PSYCHE_CUDA_DRIVER_MODULE_FUNCTION_COUNT,
    "registered kernel table must match module function slots");

_Static_assert(
    PSYCHE_CUDA_DRIVER_MODULE_FUNCTION_COUNT <= sizeof(unsigned int) * CHAR_BIT,
    "registered kernel declaration mask must fit in unsigned int");

/*
 * Keep simulated memory validation and the actual read/write/fill/free under
 * the allocation mutex. Moving operations outside the lock would let a
 * concurrent free invalidate CPU-backed simulated memory after validation.
 */

PSYCHE_CUDA_STUB_API CUresult cuGetErrorName(CUresult error, const char **pStr);
PSYCHE_CUDA_STUB_API CUresult cuGetErrorString(CUresult error, const char **pStr);

PSYCHE_CUDA_STUB_API const char *psyche_cuda_compat_stub_version(void) {
  return "psyche-cuda-compat-stub/0.1";
}

PSYCHE_CUDA_STUB_API int psyche_cuda_compat_stub_is_stub(void) {
  return 1;
}

static int psyche_cuda_driver_simulated_memory_enabled(void) {
  const char *value = getenv("PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY");
  return
      value != 0 &&
      (
          strcmp(value, "1") == 0 ||
          strcasecmp(value, "true") == 0 ||
          strcasecmp(value, "yes") == 0 ||
          strcasecmp(value, "on") == 0);
}

static int psyche_cuda_driver_metal_kernels_required(void) {
  const char *value = getenv("PSYCHE_CUDA_COMPAT_METAL_KERNELS");
  return value != 0 && (
      strcasecmp(value, "required") == 0 ||
      strcasecmp(value, "require") == 0 ||
      strcasecmp(value, "must") == 0);
}

static int psyche_cuda_driver_metal_kernels_enabled(void) {
  const char *value = getenv("PSYCHE_CUDA_COMPAT_METAL_KERNELS");
  return
      value != 0 &&
      (
          strcmp(value, "1") == 0 ||
          strcasecmp(value, "true") == 0 ||
          strcasecmp(value, "yes") == 0 ||
          strcasecmp(value, "on") == 0 ||
          psyche_cuda_driver_metal_kernels_required());
}

static int psyche_cuda_driver_simulated_sync_enabled(void) {
  return psyche_cuda_driver_simulated_memory_enabled();
}

static void psyche_cuda_driver_now(struct timespec *ts) {
  (void)clock_gettime(CLOCK_MONOTONIC, ts);
}

static float psyche_cuda_driver_elapsed_ms(
    const struct timespec *start,
    const struct timespec *end) {
  double seconds = (double)(end->tv_sec - start->tv_sec);
  double nanoseconds = (double)(end->tv_nsec - start->tv_nsec);
  return (float)(seconds * 1000.0 + nanoseconds / 1000000.0);
}

static PsycheCudaDriverStream *psyche_cuda_driver_find_stream_locked(CUstream stream) {
  PsycheCudaDriverStream *record = psyche_cuda_driver_streams;
  while (record != 0) {
    if (record->handle == stream) {
      return record;
    }
    record = record->next;
  }
  return 0;
}

static PsycheCudaDriverEvent *psyche_cuda_driver_find_event_locked(CUevent event) {
  PsycheCudaDriverEvent *record = psyche_cuda_driver_events;
  while (record != 0) {
    if (record->handle == event) {
      return record;
    }
    record = record->next;
  }
  return 0;
}

static CUstream psyche_cuda_driver_next_stream_handle_locked(void) {
  CUstream handle = (CUstream)psyche_cuda_driver_next_stream_handle;
  psyche_cuda_driver_next_stream_handle += 2;
  return handle;
}

static CUevent psyche_cuda_driver_next_event_handle_locked(void) {
  CUevent handle = (CUevent)psyche_cuda_driver_next_event_handle;
  psyche_cuda_driver_next_event_handle += 2;
  return handle;
}

static CUresult psyche_cuda_driver_validate_stream_simulated(CUstream stream) {
  CUresult result = CUDA_SUCCESS;
  if (stream == 0) {
    return CUDA_SUCCESS;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  if (psyche_cuda_driver_find_stream_locked(stream) == 0) {
    result = CUDA_ERROR_INVALID_HANDLE;
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

static CUresult psyche_cuda_driver_validate_async_stream(CUstream stream) {
  if (stream == 0) {
    return CUDA_SUCCESS;
  }
  if (!psyche_cuda_driver_simulated_sync_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  return psyche_cuda_driver_validate_stream_simulated(stream);
}

static CUresult psyche_cuda_driver_create_stream_simulated(
    CUstream *phStream,
    unsigned int flags,
    int priority) {
  PsycheCudaDriverStream *record = 0;
  const unsigned int allowed_flags = CU_STREAM_NON_BLOCKING;
  if ((flags & ~allowed_flags) != 0) {
    *phStream = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  record = (PsycheCudaDriverStream *)malloc(sizeof(*record));
  if (record == 0) {
    *phStream = 0;
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  (void)priority;
  record->flags = flags;
  record->priority = 0;
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  record->handle = psyche_cuda_driver_next_stream_handle_locked();
  record->next = psyche_cuda_driver_streams;
  psyche_cuda_driver_streams = record;
  *phStream = record->handle;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_destroy_stream_simulated(CUstream stream) {
  PsycheCudaDriverStream **link = &psyche_cuda_driver_streams;
  PsycheCudaDriverStream *record = 0;
  if (stream == 0) {
    return CUDA_ERROR_INVALID_HANDLE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  while (*link != 0 && (*link)->handle != stream) {
    link = &(*link)->next;
  }
  if (*link == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_HANDLE;
  }
  record = *link;
  *link = record->next;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  free(record);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_create_event_simulated(
    CUevent *phEvent,
    unsigned int flags) {
  PsycheCudaDriverEvent *record = 0;
  const unsigned int allowed_flags = CU_EVENT_BLOCKING_SYNC | CU_EVENT_DISABLE_TIMING;
  if ((flags & CU_EVENT_INTERPROCESS) != 0) {
    *phEvent = 0;
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  if ((flags & ~allowed_flags) != 0) {
    *phEvent = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  record = (PsycheCudaDriverEvent *)malloc(sizeof(*record));
  if (record == 0) {
    *phEvent = 0;
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  record->flags = flags;
  record->recorded = 0;
  record->recorded_at.tv_sec = 0;
  record->recorded_at.tv_nsec = 0;
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  record->handle = psyche_cuda_driver_next_event_handle_locked();
  record->next = psyche_cuda_driver_events;
  psyche_cuda_driver_events = record;
  *phEvent = record->handle;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_destroy_event_simulated(CUevent event) {
  PsycheCudaDriverEvent **link = &psyche_cuda_driver_events;
  PsycheCudaDriverEvent *record = 0;
  if (event == 0) {
    return CUDA_ERROR_INVALID_HANDLE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  while (*link != 0 && (*link)->handle != event) {
    link = &(*link)->next;
  }
  if (*link == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_HANDLE;
  }
  record = *link;
  *link = record->next;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  free(record);
  return CUDA_SUCCESS;
}

static int psyche_cuda_driver_mul_size_checked(
    size_t count,
    size_t element_size,
    size_t *bytes) {
  if (element_size != 0 && count > SIZE_MAX / element_size) {
    return 0;
  }
  *bytes = count * element_size;
  return 1;
}

static int psyche_cuda_driver_add_size_checked(
    size_t left,
    size_t right,
    size_t *sum) {
  if (right > SIZE_MAX - left) {
    return 0;
  }
  *sum = left + right;
  return 1;
}

static int psyche_cuda_driver_align_up_size_checked(
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
  return psyche_cuda_driver_add_size_checked(value, increment, aligned);
}

static int psyche_cuda_driver_range_contains(
    PsycheCudaDriverAllocation *allocation,
    CUdeviceptr dptr,
    size_t count) {
  uintptr_t base = (uintptr_t)allocation->dptr;
  uintptr_t address = (uintptr_t)dptr;
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

static int psyche_cuda_driver_host_range_contains(
    PsycheCudaDriverHostAllocation *allocation,
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

static int psyche_cuda_driver_ranges_overlap(
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

static PsycheCudaDriverRangeRelation psyche_cuda_driver_range_relation(
    CUdeviceptr first,
    CUdeviceptr second,
    size_t bytes) {
  if (bytes == 0) {
    return PSYCHE_CUDA_DRIVER_RANGE_NO_OVERLAP;
  }
  if (first == second) {
    return PSYCHE_CUDA_DRIVER_RANGE_EXACT_ALIAS;
  }
  if (psyche_cuda_driver_ranges_overlap(
      (const void *)(uintptr_t)first,
      bytes,
      (const void *)(uintptr_t)second,
      bytes)) {
    return PSYCHE_CUDA_DRIVER_RANGE_PARTIAL_OVERLAP;
  }
  return PSYCHE_CUDA_DRIVER_RANGE_NO_OVERLAP;
}

static int psyche_cuda_driver_range_is_valid(const void *ptr, size_t size) {
  uintptr_t base = (uintptr_t)ptr;
  return ptr != 0 && size != 0 && size <= UINTPTR_MAX - base;
}

static PsycheCudaDriverAllocation *psyche_cuda_driver_find_allocation_locked(
    CUdeviceptr dptr,
    size_t count) {
  PsycheCudaDriverAllocation *allocation = psyche_cuda_driver_allocations;
  while (allocation != 0) {
    if (psyche_cuda_driver_range_contains(allocation, dptr, count)) {
      return allocation;
    }
    allocation = allocation->next;
  }
  return 0;
}

static PsycheCudaDriverHostAllocation *psyche_cuda_driver_find_host_allocation_locked(
    const void *ptr,
    size_t count) {
  PsycheCudaDriverHostAllocation *allocation = psyche_cuda_driver_host_allocations;
  while (allocation != 0) {
    if (psyche_cuda_driver_host_range_contains(allocation, ptr, count)) {
      return allocation;
    }
    allocation = allocation->next;
  }
  return 0;
}

static PsycheCudaDriverHostAllocation *psyche_cuda_driver_find_mapped_host_allocation_locked(
    CUdeviceptr dptr,
    size_t count) {
  PsycheCudaDriverHostAllocation *allocation = psyche_cuda_driver_host_allocations;
  const void *ptr = (const void *)(uintptr_t)dptr;
  while (allocation != 0) {
    if (
        allocation->device_mapped &&
        psyche_cuda_driver_host_range_contains(allocation, ptr, count)) {
      return allocation;
    }
    allocation = allocation->next;
  }
  return 0;
}

static PsycheCudaDriverModule *psyche_cuda_driver_find_module_locked(CUmodule module) {
  PsycheCudaDriverModule *record = psyche_cuda_driver_modules;
  while (record != 0) {
    if (record->handle == module && record->magic == PSYCHE_CUDA_MODULE_MAGIC) {
      return record;
    }
    record = record->next;
  }
  return 0;
}

static PsycheCudaDriverFunction *psyche_cuda_driver_module_function_at(
    PsycheCudaDriverModule *module,
    unsigned int index) {
  if (module == 0 || index >= PSYCHE_CUDA_DRIVER_MODULE_FUNCTION_COUNT) {
    return 0;
  }
  return &module->functions[index];
}

static void psyche_cuda_driver_register_module_function(
    PsycheCudaDriverModule *module,
    PsycheCudaDriverFunction *function,
    const PsycheCudaDriverKernelDescriptor *descriptor) {
  if (module == 0 || function == 0 || descriptor == 0) {
    return;
  }
  function->magic = PSYCHE_CUDA_FUNCTION_MAGIC;
  function->handle = (CUfunction)function;
  function->kind = descriptor->kind;
  function->descriptor = descriptor;
  function->module = module;
  (void)snprintf(function->name, sizeof(function->name), "%s", descriptor->name);
}

static PsycheCudaDriverFunction *psyche_cuda_driver_find_function_locked(CUfunction function) {
  PsycheCudaDriverModule *module = psyche_cuda_driver_modules;
  while (module != 0) {
    unsigned int i = 0;
    if (module->magic != PSYCHE_CUDA_MODULE_MAGIC) {
      module = module->next;
      continue;
    }
    for (i = 0; i < PSYCHE_CUDA_DRIVER_MODULE_FUNCTION_COUNT; i++) {
      PsycheCudaDriverFunction *candidate = psyche_cuda_driver_module_function_at(module, i);
      if (
          candidate != 0 &&
          candidate->handle == function &&
          candidate->magic == PSYCHE_CUDA_FUNCTION_MAGIC) {
        return candidate;
      }
    }
    module = module->next;
  }
  return 0;
}

static int psyche_cuda_driver_token_equals(
    const char *token,
    size_t token_length,
    const char *expected) {
  return strlen(expected) == token_length && strncmp(token, expected, token_length) == 0;
}

static CUresult psyche_cuda_driver_parse_module_blob(
    const char *blob,
    unsigned int *declared_functions) {
  static const char module_magic[] = "PSYCHE_CUDA_MODULE_V1";
  static const char functions_prefix[] = "functions=";
  const char *cursor = blob;
  size_t magic_length = strlen(module_magic);
  size_t prefix_length = strlen(functions_prefix);
  unsigned int mask = 0;
  if (declared_functions == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  *declared_functions = 0;
  if (blob == 0 || strncmp(blob, module_magic, magic_length) != 0) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  cursor += magic_length;
  if (*cursor == '\r') {
    cursor++;
  }
  if (*cursor != '\n') {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  cursor++;
  if (strncmp(cursor, functions_prefix, prefix_length) != 0) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  cursor += prefix_length;
  if (*cursor == '\0' || *cursor == '\r' || *cursor == '\n' || *cursor == ',') {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  while (*cursor != '\0' && *cursor != '\r' && *cursor != '\n') {
    const char *token = cursor;
    unsigned int bit = 0;
    size_t token_length = 0;
    while (*cursor != '\0' && *cursor != ',' && *cursor != '\r' && *cursor != '\n') {
      cursor++;
    }
    token_length = (size_t)(cursor - token);
    if (token_length == 0) {
      return CUDA_ERROR_NOT_SUPPORTED;
    }
    {
      unsigned int i = 0;
      for (i = 0; i < PSYCHE_CUDA_DRIVER_MODULE_FUNCTION_COUNT; i++) {
        if (psyche_cuda_driver_token_equals(
                token,
                token_length,
                psyche_cuda_driver_kernel_descriptors[i].name)) {
          bit = 1u << i;
          break;
        }
      }
    }
    if (bit == 0) {
      return CUDA_ERROR_NOT_SUPPORTED;
    }
    if ((mask & bit) != 0) {
      return CUDA_ERROR_NOT_SUPPORTED;
    }
    mask |= bit;
    if (*cursor == ',') {
      cursor++;
      if (*cursor == '\0' || *cursor == ',' || *cursor == '\r' || *cursor == '\n') {
        return CUDA_ERROR_NOT_SUPPORTED;
      }
    }
  }
  if (*cursor == '\r') {
    cursor++;
  }
  if (*cursor == '\n') {
    cursor++;
  }
  if (*cursor != '\0') {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  if (mask == 0) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  *declared_functions = mask;
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_create_psyche_module_from_blob(
    CUmodule *module,
    const char *blob) {
  PsycheCudaDriverModule *record = 0;
  unsigned int declared_functions = 0;
  CUresult parse_result = psyche_cuda_driver_parse_module_blob(blob, &declared_functions);
  if (parse_result != CUDA_SUCCESS) {
    *module = 0;
    return parse_result;
  }
  record = (PsycheCudaDriverModule *)calloc(1, sizeof(*record));
  if (record == 0) {
    *module = 0;
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  record->magic = PSYCHE_CUDA_MODULE_MAGIC;
  record->handle = (CUmodule)record;
  {
    unsigned int i = 0;
    for (i = 0; i < PSYCHE_CUDA_DRIVER_MODULE_FUNCTION_COUNT; i++) {
      if ((declared_functions & (1u << i)) != 0) {
        psyche_cuda_driver_register_module_function(
            record,
            &record->functions[i],
            &psyche_cuda_driver_kernel_descriptors[i]);
      }
    }
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  record->next = psyche_cuda_driver_modules;
  psyche_cuda_driver_modules = record;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  *module = record->handle;
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_launch_vector_add_f32(
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    void **kernelParams) {
  CUdeviceptr a_dptr = 0;
  CUdeviceptr b_dptr = 0;
  CUdeviceptr out_dptr = 0;
  unsigned int n = 0;
  size_t bytes = 0;
  size_t thread_count = 0;
  PsycheCudaDriverAllocation *a_allocation = 0;
  PsycheCudaDriverAllocation *b_allocation = 0;
  PsycheCudaDriverAllocation *out_allocation = 0;
  const float *a = 0;
  const float *b = 0;
  float *out = 0;
  unsigned int i = 0;
  if (
      kernelParams == 0 ||
      kernelParams[0] == 0 ||
      kernelParams[1] == 0 ||
      kernelParams[2] == 0 ||
      kernelParams[3] == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      gridDimX == 0 ||
      blockDimX == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      gridDimY != 1 ||
      gridDimZ != 1 ||
      blockDimY != 1 ||
      blockDimZ != 1) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  a_dptr = *(const CUdeviceptr *)kernelParams[0];
  b_dptr = *(const CUdeviceptr *)kernelParams[1];
  out_dptr = *(const CUdeviceptr *)kernelParams[2];
  n = *(const unsigned int *)kernelParams[3];
  if (n == 0) {
    return CUDA_SUCCESS;
  }
  if ((size_t)n > SIZE_MAX / sizeof(float)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  bytes = (size_t)n * sizeof(float);
  if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  thread_count = (size_t)gridDimX * (size_t)blockDimX;
  if (thread_count < (size_t)n) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  a_allocation = psyche_cuda_driver_find_allocation_locked(a_dptr, bytes);
  b_allocation = psyche_cuda_driver_find_allocation_locked(b_dptr, bytes);
  out_allocation = psyche_cuda_driver_find_allocation_locked(out_dptr, bytes);
  if (a_allocation == 0 || b_allocation == 0 || out_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  a = (const float *)(uintptr_t)a_dptr;
  b = (const float *)(uintptr_t)b_dptr;
  out = (float *)(uintptr_t)out_dptr;
#if defined(__APPLE__)
  if (psyche_cuda_driver_metal_kernels_enabled()) {
    PsycheCudaDriverRangeRelation out_a_relation =
        psyche_cuda_driver_range_relation(out_dptr, a_dptr, bytes);
    PsycheCudaDriverRangeRelation out_b_relation =
        psyche_cuda_driver_range_relation(out_dptr, b_dptr, bytes);
    if (
        out_a_relation == PSYCHE_CUDA_DRIVER_RANGE_PARTIAL_OVERLAP ||
        out_b_relation == PSYCHE_CUDA_DRIVER_RANGE_PARTIAL_OVERLAP) {
      if (psyche_cuda_driver_metal_kernels_required()) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return CUDA_ERROR_NOT_SUPPORTED;
      }
    } else {
      CUresult metal_result = psyche_cuda_metal_launch_vector_add_f32(
          a,
          b,
          out,
          n,
          bytes,
          gridDimX,
          blockDimX);
      if (metal_result == CUDA_SUCCESS || psyche_cuda_driver_metal_kernels_required()) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return metal_result;
      }
    }
  }
#else
  if (psyche_cuda_driver_metal_kernels_required()) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_NOT_SUPPORTED;
  }
#endif
  for (i = 0; i < n; i++) {
    out[i] = a[i] + b[i];
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_launch_saxpy_f32(
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    void **kernelParams) {
  CUdeviceptr x_dptr = 0;
  CUdeviceptr y_dptr = 0;
  float alpha = 0.0f;
  unsigned int n = 0;
  size_t bytes = 0;
  size_t thread_count = 0;
  PsycheCudaDriverAllocation *x_allocation = 0;
  PsycheCudaDriverAllocation *y_allocation = 0;
  const float *x = 0;
  float *y = 0;
  unsigned int i = 0;
  if (
      kernelParams == 0 ||
      kernelParams[0] == 0 ||
      kernelParams[1] == 0 ||
      kernelParams[2] == 0 ||
      kernelParams[3] == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      gridDimX == 0 ||
      blockDimX == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      gridDimY != 1 ||
      gridDimZ != 1 ||
      blockDimY != 1 ||
      blockDimZ != 1) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  x_dptr = *(const CUdeviceptr *)kernelParams[0];
  y_dptr = *(const CUdeviceptr *)kernelParams[1];
  alpha = *(const float *)kernelParams[2];
  n = *(const unsigned int *)kernelParams[3];
  if (n == 0) {
    return CUDA_SUCCESS;
  }
  if ((size_t)n > SIZE_MAX / sizeof(float)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  bytes = (size_t)n * sizeof(float);
  if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  thread_count = (size_t)gridDimX * (size_t)blockDimX;
  if (thread_count < (size_t)n) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  x_allocation = psyche_cuda_driver_find_allocation_locked(x_dptr, bytes);
  y_allocation = psyche_cuda_driver_find_allocation_locked(y_dptr, bytes);
  if (x_allocation == 0 || y_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  x = (const float *)(uintptr_t)x_dptr;
  y = (float *)(uintptr_t)y_dptr;
#if defined(__APPLE__)
  if (psyche_cuda_driver_metal_kernels_enabled()) {
    PsycheCudaDriverRangeRelation x_y_relation =
        psyche_cuda_driver_range_relation(x_dptr, y_dptr, bytes);
    if (x_y_relation == PSYCHE_CUDA_DRIVER_RANGE_PARTIAL_OVERLAP) {
      if (psyche_cuda_driver_metal_kernels_required()) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return CUDA_ERROR_NOT_SUPPORTED;
      }
    } else {
      CUresult metal_result = psyche_cuda_metal_launch_saxpy_f32(
          x,
          y,
          alpha,
          n,
          bytes,
          gridDimX,
          blockDimX);
      if (metal_result == CUDA_SUCCESS || psyche_cuda_driver_metal_kernels_required()) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return metal_result;
      }
    }
  }
#else
  if (psyche_cuda_driver_metal_kernels_required()) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_NOT_SUPPORTED;
  }
#endif
  for (i = 0; i < n; i++) {
    y[i] = alpha * x[i] + y[i];
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_launch_scale_f32(
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    void **kernelParams) {
  CUdeviceptr x_dptr = 0;
  float alpha = 0.0f;
  unsigned int n = 0;
  size_t bytes = 0;
  size_t thread_count = 0;
  PsycheCudaDriverAllocation *x_allocation = 0;
  float *x = 0;
  unsigned int i = 0;
  if (
      kernelParams == 0 ||
      kernelParams[0] == 0 ||
      kernelParams[1] == 0 ||
      kernelParams[2] == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      gridDimX == 0 ||
      blockDimX == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      gridDimY != 1 ||
      gridDimZ != 1 ||
      blockDimY != 1 ||
      blockDimZ != 1) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  x_dptr = *(const CUdeviceptr *)kernelParams[0];
  alpha = *(const float *)kernelParams[1];
  n = *(const unsigned int *)kernelParams[2];
  if (n == 0) {
    return CUDA_SUCCESS;
  }
  if ((size_t)n > SIZE_MAX / sizeof(float)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  bytes = (size_t)n * sizeof(float);
  if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  thread_count = (size_t)gridDimX * (size_t)blockDimX;
  if (thread_count < (size_t)n) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  x_allocation = psyche_cuda_driver_find_allocation_locked(x_dptr, bytes);
  if (x_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  x = (float *)(uintptr_t)x_dptr;
#if defined(__APPLE__)
  if (psyche_cuda_driver_metal_kernels_enabled()) {
    CUresult metal_result = psyche_cuda_metal_launch_scale_f32(
        x,
        alpha,
        n,
        bytes,
        gridDimX,
        blockDimX);
    if (metal_result == CUDA_SUCCESS || psyche_cuda_driver_metal_kernels_required()) {
      pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
      return metal_result;
    }
  }
#else
  if (psyche_cuda_driver_metal_kernels_required()) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_NOT_SUPPORTED;
  }
#endif
  for (i = 0; i < n; i++) {
    x[i] = alpha * x[i];
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_launch_axpby_f32(
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    void **kernelParams) {
  CUdeviceptr x_dptr = 0;
  CUdeviceptr y_dptr = 0;
  float alpha = 0.0f;
  float beta = 0.0f;
  unsigned int n = 0;
  size_t bytes = 0;
  size_t thread_count = 0;
  PsycheCudaDriverAllocation *x_allocation = 0;
  PsycheCudaDriverAllocation *y_allocation = 0;
  float *x = 0;
  const float *y = 0;
  unsigned int i = 0;
  if (
      kernelParams == 0 ||
      kernelParams[0] == 0 ||
      kernelParams[1] == 0 ||
      kernelParams[2] == 0 ||
      kernelParams[3] == 0 ||
      kernelParams[4] == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      gridDimX == 0 ||
      blockDimX == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      gridDimY != 1 ||
      gridDimZ != 1 ||
      blockDimY != 1 ||
      blockDimZ != 1) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  x_dptr = *(const CUdeviceptr *)kernelParams[0];
  y_dptr = *(const CUdeviceptr *)kernelParams[1];
  alpha = *(const float *)kernelParams[2];
  beta = *(const float *)kernelParams[3];
  n = *(const unsigned int *)kernelParams[4];
  if (n == 0) {
    return CUDA_SUCCESS;
  }
  if ((size_t)n > SIZE_MAX / sizeof(float)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  bytes = (size_t)n * sizeof(float);
  if ((size_t)gridDimX > SIZE_MAX / (size_t)blockDimX) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  thread_count = (size_t)gridDimX * (size_t)blockDimX;
  if (thread_count < (size_t)n) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  x_allocation = psyche_cuda_driver_find_allocation_locked(x_dptr, bytes);
  y_allocation = psyche_cuda_driver_find_allocation_locked(y_dptr, bytes);
  if (x_allocation == 0 || y_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  x = (float *)(uintptr_t)x_dptr;
  y = (const float *)(uintptr_t)y_dptr;
#if defined(__APPLE__)
  if (psyche_cuda_driver_metal_kernels_enabled()) {
    PsycheCudaDriverRangeRelation x_y_relation =
        psyche_cuda_driver_range_relation(x_dptr, y_dptr, bytes);
    if (x_y_relation == PSYCHE_CUDA_DRIVER_RANGE_PARTIAL_OVERLAP) {
      if (psyche_cuda_driver_metal_kernels_required()) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return CUDA_ERROR_NOT_SUPPORTED;
      }
    } else {
      CUresult metal_result = psyche_cuda_metal_launch_axpby_f32(
          x,
          y,
          alpha,
          beta,
          n,
          bytes,
          gridDimX,
          blockDimX);
      if (metal_result == CUDA_SUCCESS || psyche_cuda_driver_metal_kernels_required()) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return metal_result;
      }
    }
  }
#else
  if (psyche_cuda_driver_metal_kernels_required()) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_NOT_SUPPORTED;
  }
#endif
  for (i = 0; i < n; i++) {
    x[i] = alpha * x[i] + beta * y[i];
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static int psyche_cuda_driver_host_range_registered_locked(
    const void *ptr,
    size_t size) {
  PsycheCudaDriverHostAllocation *allocation = psyche_cuda_driver_host_allocations;
  while (allocation != 0) {
    if (psyche_cuda_driver_ranges_overlap(ptr, size, allocation->ptr, allocation->size)) {
      return 1;
    }
    allocation = allocation->next;
  }
  return 0;
}

static PsycheCudaDriverHostAllocation *
psyche_cuda_driver_find_touched_mapped_host_allocation_locked(
    CUdeviceptr dptr,
    size_t count) {
  PsycheCudaDriverHostAllocation *allocation = psyche_cuda_driver_host_allocations;
  uintptr_t address = (uintptr_t)dptr;
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

static PsycheCudaDriverAllocation *psyche_cuda_driver_find_touched_allocation_locked(
    CUdeviceptr dptr,
    size_t count) {
  PsycheCudaDriverAllocation *allocation = psyche_cuda_driver_allocations;
  uintptr_t address = (uintptr_t)dptr;
  uintptr_t copy_end = 0;
  int copy_end_overflow = count > UINTPTR_MAX - address;
  copy_end = copy_end_overflow ? UINTPTR_MAX : address + count;
  while (allocation != 0) {
    uintptr_t base = (uintptr_t)allocation->dptr;
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

static unsigned long long psyche_cuda_driver_next_buffer_id_locked(void) {
  unsigned long long id = psyche_cuda_driver_next_buffer_id;
  psyche_cuda_driver_next_buffer_id += 1;
  if (psyche_cuda_driver_next_buffer_id == 0) {
    psyche_cuda_driver_next_buffer_id = 1;
  }
  return id;
}

static int psyche_cuda_driver_pool_reserved_zeroed(const unsigned char *reserved) {
  for (size_t index = 0; index < 54; index++) {
    if (reserved[index] != 0) {
      return 0;
    }
  }
  return 1;
}

static void psyche_cuda_driver_init_pool_defaults_locked(
    PsycheCudaDriverMemoryPool *pool,
    CUmemoryPool handle,
    int is_default,
    CUmemAllocationType alloc_type,
    CUmemLocation location,
    size_t max_size) {
  memset(pool, 0, sizeof(*pool));
  pool->handle = handle;
  pool->is_default = is_default;
  pool->alloc_type = alloc_type;
  pool->handle_types = CU_MEM_HANDLE_TYPE_NONE;
  pool->location = location;
  pool->max_size = max_size;
  pool->reuse_follow_event_dependencies = 1;
  pool->reuse_allow_opportunistic = 1;
  pool->reuse_allow_internal_dependencies = 1;
}

static PsycheCudaDriverMemoryPool *psyche_cuda_driver_default_host_pool_locked(void) {
  CUmemLocation location;
  if (psyche_cuda_driver_default_host_pool.handle == 0) {
    location.id = CU_DEVICE_CPU;
    location.type = CU_MEM_LOCATION_TYPE_HOST;
    psyche_cuda_driver_init_pool_defaults_locked(
        &psyche_cuda_driver_default_host_pool,
        (CUmemoryPool)&psyche_cuda_driver_default_host_pool,
        1,
        CU_MEM_ALLOCATION_TYPE_PINNED,
        location,
        0);
  }
  return &psyche_cuda_driver_default_host_pool;
}

static PsycheCudaDriverMemoryPool *psyche_cuda_driver_default_managed_pool_locked(void) {
  CUmemLocation location;
  if (psyche_cuda_driver_default_managed_pool.handle == 0) {
    location.id = CU_DEVICE_INVALID;
    location.type = CU_MEM_LOCATION_TYPE_NONE;
    psyche_cuda_driver_init_pool_defaults_locked(
        &psyche_cuda_driver_default_managed_pool,
        (CUmemoryPool)&psyche_cuda_driver_default_managed_pool,
        1,
        CU_MEM_ALLOCATION_TYPE_MANAGED,
        location,
        0);
  }
  return &psyche_cuda_driver_default_managed_pool;
}

static PsycheCudaDriverMemoryPool *psyche_cuda_driver_default_pool_locked(
    CUmemAllocationType type) {
  if (type == CU_MEM_ALLOCATION_TYPE_MANAGED) {
    return psyche_cuda_driver_default_managed_pool_locked();
  }
  return psyche_cuda_driver_default_host_pool_locked();
}

static PsycheCudaDriverMemoryPool *psyche_cuda_driver_current_pool_locked(
    CUmemAllocationType type) {
  if (type == CU_MEM_ALLOCATION_TYPE_MANAGED) {
    if (psyche_cuda_driver_current_managed_pool == 0) {
      return psyche_cuda_driver_default_managed_pool_locked();
    }
    return (PsycheCudaDriverMemoryPool *)psyche_cuda_driver_current_managed_pool;
  }
  if (psyche_cuda_driver_current_host_pool == 0) {
    return psyche_cuda_driver_default_host_pool_locked();
  }
  return (PsycheCudaDriverMemoryPool *)psyche_cuda_driver_current_host_pool;
}

static PsycheCudaDriverMemoryPool *psyche_cuda_driver_find_pool_locked(CUmemoryPool pool) {
  PsycheCudaDriverMemoryPool *record = 0;
  if (pool == 0) {
    return 0;
  }
  record = psyche_cuda_driver_default_host_pool_locked();
  if (record->handle == pool) {
    return record;
  }
  record = psyche_cuda_driver_default_managed_pool_locked();
  if (record->handle == pool) {
    return record;
  }
  record = psyche_cuda_driver_memory_pools;
  while (record != 0) {
    if (record->handle == pool) {
      return record;
    }
    record = record->next;
  }
  return 0;
}

static int psyche_cuda_driver_pool_location_supported(
    const CUmemLocation *location,
    CUmemAllocationType type) {
  if (location == 0) {
    return 0;
  }
  if (type == CU_MEM_ALLOCATION_TYPE_PINNED) {
    return location->type == CU_MEM_LOCATION_TYPE_HOST;
  }
  if (type == CU_MEM_ALLOCATION_TYPE_MANAGED) {
    return location->type == CU_MEM_LOCATION_TYPE_NONE;
  }
  return 0;
}

static CUresult psyche_cuda_driver_validate_pool_props(
    const CUmemPoolProps *poolProps,
    CUmemAllocationType *alloc_type,
    CUmemLocation *location,
    size_t *max_size) {
  if (poolProps == 0 || alloc_type == 0 || location == 0 || max_size == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      poolProps->usage != 0 ||
      poolProps->win32SecurityAttributes != 0 ||
      !psyche_cuda_driver_pool_reserved_zeroed(poolProps->reserved)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (poolProps->handleTypes != CU_MEM_HANDLE_TYPE_NONE) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  if (poolProps->allocType != CU_MEM_ALLOCATION_TYPE_PINNED) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (poolProps->location.type == CU_MEM_LOCATION_TYPE_DEVICE) {
    return CUDA_ERROR_INVALID_DEVICE;
  }
  if (!psyche_cuda_driver_pool_location_supported(&poolProps->location, poolProps->allocType)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  *alloc_type = poolProps->allocType;
  *location = poolProps->location;
  *max_size = poolProps->maxSize;
  return CUDA_SUCCESS;
}

static void psyche_cuda_driver_pool_account_alloc_locked(
    PsycheCudaDriverMemoryPool *pool,
    size_t bytesize) {
  if (pool == 0) {
    return;
  }
  pool->reserved_current += (cuuint64_t)bytesize;
  pool->used_current += (cuuint64_t)bytesize;
  if (pool->reserved_current > pool->reserved_high) {
    pool->reserved_high = pool->reserved_current;
  }
  if (pool->used_current > pool->used_high) {
    pool->used_high = pool->used_current;
  }
}

static int psyche_cuda_driver_pool_can_account_alloc_locked(
    PsycheCudaDriverMemoryPool *pool,
    size_t bytesize) {
  cuuint64_t amount = (cuuint64_t)bytesize;
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

static void psyche_cuda_driver_pool_account_free_locked(
    PsycheCudaDriverMemoryPool *pool,
    size_t bytesize) {
  if (pool == 0) {
    return;
  }
  if ((cuuint64_t)bytesize > pool->used_current) {
    pool->used_current = 0;
  } else {
    pool->used_current -= (cuuint64_t)bytesize;
  }
  if ((cuuint64_t)bytesize > pool->reserved_current) {
    pool->reserved_current = 0;
  } else {
    pool->reserved_current -= (cuuint64_t)bytesize;
  }
}

static CUresult psyche_cuda_driver_malloc_kind_simulated(
    CUdeviceptr *dptr,
    size_t bytesize,
    int managed,
    CUmemoryPool pool_handle,
    int use_current_pool,
    int async_alloc) {
  PsycheCudaDriverAllocation *allocation = 0;
  PsycheCudaDriverMemoryPool *pool = 0;
  void *ptr = 0;
  if (bytesize == 0) {
    *dptr = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  if (use_current_pool) {
    pool = psyche_cuda_driver_current_pool_locked(CU_MEM_ALLOCATION_TYPE_PINNED);
  } else if (pool_handle != 0) {
    pool = psyche_cuda_driver_find_pool_locked(pool_handle);
    if (pool == 0) {
      pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
      *dptr = 0;
      return CUDA_ERROR_INVALID_HANDLE;
    }
  }
  if (pool != 0 && pool->alloc_type == CU_MEM_ALLOCATION_TYPE_MANAGED) {
    managed = 1;
  }
  if (!psyche_cuda_driver_pool_can_account_alloc_locked(pool, bytesize)) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    *dptr = 0;
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  if (posix_memalign(&ptr, 256, bytesize) != 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    *dptr = 0;
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  allocation = (PsycheCudaDriverAllocation *)malloc(sizeof(*allocation));
  if (allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    free(ptr);
    *dptr = 0;
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  allocation->dptr = (CUdeviceptr)(uintptr_t)ptr;
  allocation->ptr = ptr;
  allocation->size = bytesize;
  allocation->pool = pool != 0 ? pool->handle : 0;
  allocation->managed = managed;
  allocation->async_alloc = async_alloc;
  allocation->read_mostly = 0;
  allocation->preferred_location = CU_DEVICE_INVALID;
  allocation->accessed_by = CU_DEVICE_INVALID;
  allocation->last_prefetch_location = CU_DEVICE_INVALID;
  allocation->sync_memops = 0;
  allocation->buffer_id = psyche_cuda_driver_next_buffer_id_locked();
  psyche_cuda_driver_pool_account_alloc_locked(pool, bytesize);
  allocation->next = psyche_cuda_driver_allocations;
  psyche_cuda_driver_allocations = allocation;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  *dptr = allocation->dptr;
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_malloc_simulated(CUdeviceptr *dptr, size_t bytesize) {
  return psyche_cuda_driver_malloc_kind_simulated(dptr, bytesize, 0, 0, 0, 0);
}

static CUresult psyche_cuda_driver_malloc_managed_simulated(
    CUdeviceptr *dptr,
    size_t bytesize,
    unsigned int flags) {
  const unsigned int allowed_flags =
      CU_MEM_ATTACH_GLOBAL | CU_MEM_ATTACH_HOST | CU_MEM_ATTACH_SINGLE;
  if (dptr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if ((flags & ~allowed_flags) != 0) {
    *dptr = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  return psyche_cuda_driver_malloc_kind_simulated(dptr, bytesize, 1, 0, 0, 0);
}

static PsycheCudaDriverAllocation *psyche_cuda_driver_find_managed_allocation_locked(
    CUdeviceptr dptr,
    size_t count) {
  PsycheCudaDriverAllocation *allocation =
      psyche_cuda_driver_find_allocation_locked(dptr, count);
  if (allocation == 0 || !allocation->managed) {
    return 0;
  }
  return allocation;
}

static CUresult psyche_cuda_driver_validate_managed_range_locked(
    CUdeviceptr dptr,
    size_t count,
    PsycheCudaDriverAllocation **allocation_out) {
  PsycheCudaDriverAllocation *allocation = 0;
  if (dptr == 0 || count == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  allocation = psyche_cuda_driver_find_managed_allocation_locked(dptr, count);
  if (allocation == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (allocation_out != 0) {
    *allocation_out = allocation;
  }
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_managed_location_from_hint(int location_hint, int *location) {
  if (location_hint == CU_DEVICE_CPU || location_hint == CU_DEVICE_INVALID) {
    *location = location_hint;
    return CUDA_SUCCESS;
  }
  if (location_hint >= 0) {
    return CUDA_ERROR_INVALID_DEVICE;
  }
  return CUDA_ERROR_INVALID_VALUE;
}

static CUresult psyche_cuda_driver_mem_advise_simulated(
    CUdeviceptr devPtr,
    size_t count,
    CUmem_advise advice,
    int location_hint) {
  PsycheCudaDriverAllocation *allocation = 0;
  CUresult result = CUDA_SUCCESS;
  int location = CU_DEVICE_INVALID;
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  result = psyche_cuda_driver_validate_managed_range_locked(devPtr, count, &allocation);
  if (result != CUDA_SUCCESS) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return result;
  }
  switch (advice) {
    case CU_MEM_ADVISE_SET_READ_MOSTLY:
      allocation->read_mostly = 1;
      break;
    case CU_MEM_ADVISE_UNSET_READ_MOSTLY:
      allocation->read_mostly = 0;
      break;
    case CU_MEM_ADVISE_SET_PREFERRED_LOCATION:
      result = psyche_cuda_driver_managed_location_from_hint(location_hint, &location);
      if (result == CUDA_SUCCESS && location == CU_DEVICE_CPU) {
        allocation->preferred_location = location;
      } else if (result == CUDA_SUCCESS) {
        result = CUDA_ERROR_INVALID_VALUE;
      }
      break;
    case CU_MEM_ADVISE_UNSET_PREFERRED_LOCATION:
      allocation->preferred_location = CU_DEVICE_INVALID;
      break;
    case CU_MEM_ADVISE_SET_ACCESSED_BY:
      result = CUDA_ERROR_INVALID_DEVICE;
      break;
    case CU_MEM_ADVISE_UNSET_ACCESSED_BY:
      allocation->accessed_by = CU_DEVICE_INVALID;
      break;
    default:
      result = CUDA_ERROR_INVALID_VALUE;
      break;
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

static CUresult psyche_cuda_driver_mem_prefetch_simulated(
    CUdeviceptr devPtr,
    size_t count,
    int location_hint) {
  PsycheCudaDriverAllocation *allocation = 0;
  CUresult result = CUDA_SUCCESS;
  int location = CU_DEVICE_INVALID;
  result = psyche_cuda_driver_managed_location_from_hint(location_hint, &location);
  if (result != CUDA_SUCCESS) {
    return result;
  }
  if (location != CU_DEVICE_CPU) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  result = psyche_cuda_driver_validate_managed_range_locked(devPtr, count, &allocation);
  if (result == CUDA_SUCCESS) {
    allocation->last_prefetch_location = CU_DEVICE_CPU;
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

static CUresult psyche_cuda_driver_fill_accessed_by(void *data, size_t dataSize) {
  int *values = (int *)data;
  size_t count = dataSize / sizeof(int);
  if (data == 0 || dataSize == 0 || (dataSize % sizeof(int)) != 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  for (size_t index = 0; index < count; index++) {
    values[index] = CU_DEVICE_INVALID;
  }
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_mem_range_get_attribute_locked(
    void *data,
    size_t dataSize,
    CUmem_range_attribute attribute,
    CUdeviceptr devPtr,
    size_t count) {
  PsycheCudaDriverAllocation *allocation = 0;
  CUresult result = psyche_cuda_driver_validate_managed_range_locked(devPtr, count, &allocation);
  if (result != CUDA_SUCCESS) {
    return result;
  }
  if (data == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  switch (attribute) {
    case CU_MEM_RANGE_ATTRIBUTE_READ_MOSTLY:
      if (dataSize != sizeof(int)) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      *(int *)data = allocation->read_mostly;
      return CUDA_SUCCESS;
    case CU_MEM_RANGE_ATTRIBUTE_PREFERRED_LOCATION:
      if (dataSize != sizeof(int)) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      *(int *)data = allocation->preferred_location;
      return CUDA_SUCCESS;
    case CU_MEM_RANGE_ATTRIBUTE_ACCESSED_BY:
      return psyche_cuda_driver_fill_accessed_by(data, dataSize);
    case CU_MEM_RANGE_ATTRIBUTE_LAST_PREFETCH_LOCATION:
      if (dataSize != sizeof(int)) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      *(int *)data = allocation->last_prefetch_location;
      return CUDA_SUCCESS;
    case CU_MEM_RANGE_ATTRIBUTE_PREFERRED_LOCATION_TYPE:
      if (dataSize != sizeof(CUmemLocationType)) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      *(CUmemLocationType *)data =
          allocation->preferred_location == CU_DEVICE_CPU ?
          CU_MEM_LOCATION_TYPE_HOST :
          CU_MEM_LOCATION_TYPE_INVALID;
      return CUDA_SUCCESS;
    case CU_MEM_RANGE_ATTRIBUTE_PREFERRED_LOCATION_ID:
      if (dataSize != sizeof(int)) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      *(int *)data = allocation->preferred_location;
      return CUDA_SUCCESS;
    case CU_MEM_RANGE_ATTRIBUTE_LAST_PREFETCH_LOCATION_TYPE:
      if (dataSize != sizeof(CUmemLocationType)) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      *(CUmemLocationType *)data =
          allocation->last_prefetch_location == CU_DEVICE_CPU ?
          CU_MEM_LOCATION_TYPE_HOST :
          CU_MEM_LOCATION_TYPE_INVALID;
      return CUDA_SUCCESS;
    case CU_MEM_RANGE_ATTRIBUTE_LAST_PREFETCH_LOCATION_ID:
      if (dataSize != sizeof(int)) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      *(int *)data = allocation->last_prefetch_location;
      return CUDA_SUCCESS;
    default:
      return CUDA_ERROR_INVALID_VALUE;
  }
}

static CUresult psyche_cuda_driver_pointer_get_attribute_locked(
    void *data,
    CUpointer_attribute attribute,
    CUdeviceptr ptr) {
  PsycheCudaDriverAllocation *allocation = 0;
  PsycheCudaDriverHostAllocation *mapped_host_allocation = 0;
  if (data == 0 || ptr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  allocation = psyche_cuda_driver_find_allocation_locked(ptr, 1);
  mapped_host_allocation = psyche_cuda_driver_find_mapped_host_allocation_locked(ptr, 1);
  if (allocation == 0 && mapped_host_allocation == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  switch (attribute) {
    case CU_POINTER_ATTRIBUTE_CONTEXT:
      *(CUcontext *)data = 0;
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_MEMORY_TYPE:
      /*
       * CPU-backed simulated managed allocation. The CUDA Driver API has no
       * cudaMemoryTypeManaged enum here; CU_MEMORYTYPE_UNIFIED is the
       * least-lying CUDA-shaped answer for a cuMemAllocManaged record. It does
       * not imply GPU residency, migration, page faulting, or real CUDA UVA.
       */
      *(unsigned int *)data =
          allocation != 0 ?
          (allocation->managed ? CU_MEMORYTYPE_UNIFIED : CU_MEMORYTYPE_DEVICE) :
          CU_MEMORYTYPE_HOST;
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_DEVICE_POINTER:
      *(CUdeviceptr *)data = ptr;
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_HOST_POINTER:
      *(void **)data = (void *)(uintptr_t)ptr;
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_SYNC_MEMOPS:
      if (allocation == 0) {
        *(int *)data = 0;
      } else {
        *(int *)data = allocation->sync_memops;
      }
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_BUFFER_ID:
      if (allocation == 0) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      *(unsigned long long *)data = allocation->buffer_id;
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_IS_MANAGED:
      *(int *)data = allocation != 0 && allocation->managed;
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL:
      *(int *)data = CU_DEVICE_INVALID;
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_RANGE_START_ADDR:
      if (allocation != 0) {
        *(CUdeviceptr *)data = allocation->dptr;
      } else {
        *(CUdeviceptr *)data = (CUdeviceptr)(uintptr_t)mapped_host_allocation->ptr;
      }
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_RANGE_SIZE:
      if (allocation != 0) {
        *(size_t *)data = allocation->size;
      } else {
        *(size_t *)data = mapped_host_allocation->size;
      }
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_MAPPED:
      *(int *)data = 1;
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_IS_LEGACY_CUDA_IPC_CAPABLE:
    case CU_POINTER_ATTRIBUTE_ALLOWED_HANDLE_TYPES:
    case CU_POINTER_ATTRIBUTE_IS_HW_DECOMPRESS_CAPABLE:
      *(int *)data = 0;
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_MEMPOOL_HANDLE:
      if (allocation == 0 || allocation->pool == 0) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      *(CUmemoryPool *)data = allocation->pool;
      return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_P2P_TOKENS:
      return CUDA_ERROR_NOT_SUPPORTED;
    default:
      return CUDA_ERROR_INVALID_VALUE;
  }
}

static CUresult psyche_cuda_driver_pointer_set_attribute_locked(
    const void *value,
    CUpointer_attribute attribute,
    CUdeviceptr ptr) {
  PsycheCudaDriverAllocation *allocation = 0;
  if (value == 0 || ptr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  allocation = psyche_cuda_driver_find_allocation_locked(ptr, 1);
  if (allocation == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (attribute != CU_POINTER_ATTRIBUTE_SYNC_MEMOPS) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  /*
   * SYNC_MEMOPS is metadata-only in this CPU-backed shim. It does not add CUDA
   * ordering semantics; simulated memory is host-coherent CPU memory.
   */
  allocation->sync_memops = (*(const int *)value) != 0;
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_mem_pool_get_attribute_locked(
    CUmemoryPool pool,
    CUmemPool_attribute attr,
    void *value) {
  PsycheCudaDriverMemoryPool *record = 0;
  if (pool == 0 || value == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  record = psyche_cuda_driver_find_pool_locked(pool);
  if (record == 0) {
    return CUDA_ERROR_INVALID_HANDLE;
  }
  switch (attr) {
    case CU_MEMPOOL_ATTR_REUSE_FOLLOW_EVENT_DEPENDENCIES:
      *(int *)value = record->reuse_follow_event_dependencies;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_REUSE_ALLOW_OPPORTUNISTIC:
      *(int *)value = record->reuse_allow_opportunistic;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_REUSE_ALLOW_INTERNAL_DEPENDENCIES:
      *(int *)value = record->reuse_allow_internal_dependencies;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_RELEASE_THRESHOLD:
      *(cuuint64_t *)value = record->release_threshold;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_RESERVED_MEM_CURRENT:
      *(cuuint64_t *)value = record->reserved_current;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_RESERVED_MEM_HIGH:
      *(cuuint64_t *)value = record->reserved_high;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_USED_MEM_CURRENT:
      *(cuuint64_t *)value = record->used_current;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_USED_MEM_HIGH:
      *(cuuint64_t *)value = record->used_high;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_ALLOCATION_TYPE:
      *(CUmemAllocationType *)value = record->alloc_type;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_EXPORT_HANDLE_TYPES:
      *(CUmemAllocationHandleType *)value = record->handle_types;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_LOCATION_ID:
      *(int *)value = record->location.id;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_LOCATION_TYPE:
      *(CUmemLocationType *)value = record->location.type;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_MAX_POOL_SIZE:
      *(cuuint64_t *)value = (cuuint64_t)record->max_size;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_HW_DECOMPRESS_ENABLED:
      *(int *)value = 0;
      return CUDA_SUCCESS;
    default:
      return CUDA_ERROR_INVALID_VALUE;
  }
}

static CUresult psyche_cuda_driver_mem_pool_set_attribute_locked(
    CUmemoryPool pool,
    CUmemPool_attribute attr,
    void *value) {
  PsycheCudaDriverMemoryPool *record = 0;
  if (pool == 0 || value == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  record = psyche_cuda_driver_find_pool_locked(pool);
  if (record == 0) {
    return CUDA_ERROR_INVALID_HANDLE;
  }
  switch (attr) {
    case CU_MEMPOOL_ATTR_REUSE_FOLLOW_EVENT_DEPENDENCIES:
      record->reuse_follow_event_dependencies = (*(int *)value) != 0;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_REUSE_ALLOW_OPPORTUNISTIC:
      record->reuse_allow_opportunistic = (*(int *)value) != 0;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_REUSE_ALLOW_INTERNAL_DEPENDENCIES:
      record->reuse_allow_internal_dependencies = (*(int *)value) != 0;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_RELEASE_THRESHOLD:
      record->release_threshold = *(cuuint64_t *)value;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_RESERVED_MEM_HIGH:
      if (*(cuuint64_t *)value != 0) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      record->reserved_high = record->reserved_current;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_USED_MEM_HIGH:
      if (*(cuuint64_t *)value != 0) {
        return CUDA_ERROR_INVALID_VALUE;
      }
      record->used_high = record->used_current;
      return CUDA_SUCCESS;
    case CU_MEMPOOL_ATTR_RESERVED_MEM_CURRENT:
    case CU_MEMPOOL_ATTR_USED_MEM_CURRENT:
    case CU_MEMPOOL_ATTR_ALLOCATION_TYPE:
    case CU_MEMPOOL_ATTR_EXPORT_HANDLE_TYPES:
    case CU_MEMPOOL_ATTR_LOCATION_ID:
    case CU_MEMPOOL_ATTR_LOCATION_TYPE:
    case CU_MEMPOOL_ATTR_MAX_POOL_SIZE:
    case CU_MEMPOOL_ATTR_HW_DECOMPRESS_ENABLED:
      return CUDA_ERROR_INVALID_VALUE;
    default:
      return CUDA_ERROR_INVALID_VALUE;
  }
}

static CUresult psyche_cuda_driver_mem_pool_create_simulated(
    CUmemoryPool *pool_out,
    const CUmemPoolProps *poolProps) {
  PsycheCudaDriverMemoryPool *record = 0;
  CUmemAllocationType alloc_type = CU_MEM_ALLOCATION_TYPE_INVALID;
  CUmemLocation location;
  size_t max_size = 0;
  CUresult result = CUDA_SUCCESS;
  result = psyche_cuda_driver_validate_pool_props(
      poolProps,
      &alloc_type,
      &location,
      &max_size);
  if (result != CUDA_SUCCESS) {
    *pool_out = 0;
    return result;
  }
  record = (PsycheCudaDriverMemoryPool *)malloc(sizeof(*record));
  if (record == 0) {
    *pool_out = 0;
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  psyche_cuda_driver_init_pool_defaults_locked(
      record,
      (CUmemoryPool)record,
      0,
      alloc_type,
      location,
      max_size);
  record->next = psyche_cuda_driver_memory_pools;
  psyche_cuda_driver_memory_pools = record;
  *pool_out = record->handle;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_mem_pool_destroy_simulated(CUmemoryPool pool) {
  PsycheCudaDriverMemoryPool **link = &psyche_cuda_driver_memory_pools;
  PsycheCudaDriverMemoryPool *record = 0;
  if (
      pool == 0 ||
      pool == (CUmemoryPool)&psyche_cuda_driver_default_host_pool ||
      pool == (CUmemoryPool)&psyche_cuda_driver_default_managed_pool) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  while (*link != 0 && (*link)->handle != pool) {
    link = &(*link)->next;
  }
  if (*link == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_HANDLE;
  }
  record = *link;
  if (record->used_current != 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  *link = record->next;
  if (psyche_cuda_driver_current_host_pool == pool) {
    psyche_cuda_driver_current_host_pool = 0;
  }
  if (psyche_cuda_driver_current_managed_pool == pool) {
    psyche_cuda_driver_current_managed_pool = 0;
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  free(record);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_mem_pool_trim_to_simulated(
    CUmemoryPool pool,
    size_t minBytesToKeep) {
  PsycheCudaDriverMemoryPool *record = 0;
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  record = psyche_cuda_driver_find_pool_locked(pool);
  if (record == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return pool == 0 ? CUDA_ERROR_INVALID_VALUE : CUDA_ERROR_INVALID_HANDLE;
  }
  (void)minBytesToKeep;
  if (record->reserved_current < record->used_current) {
    record->reserved_current = record->used_current;
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_validate_mem_pool_location_request(
    const CUmemLocation *location,
    CUmemAllocationType type) {
  if (location == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (location->type == CU_MEM_LOCATION_TYPE_DEVICE) {
    return CUDA_ERROR_INVALID_DEVICE;
  }
  if (!psyche_cuda_driver_pool_location_supported(location, type)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_malloc_pitch_simulated(
    CUdeviceptr *dptr,
    size_t *pPitch,
    size_t WidthInBytes,
    size_t Height,
    unsigned int ElementSizeBytes) {
  size_t pitch = 0;
  size_t bytes = 0;
  size_t alignment = 16;
  CUresult result = CUDA_SUCCESS;
  if (
      WidthInBytes == 0 ||
      Height == 0 ||
      (ElementSizeBytes != 4 && ElementSizeBytes != 8 && ElementSizeBytes != 16)) {
    *dptr = 0;
    *pPitch = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (ElementSizeBytes > alignment) {
    alignment = ElementSizeBytes;
  }
  if (!psyche_cuda_driver_align_up_size_checked(WidthInBytes, alignment, &pitch)) {
    *dptr = 0;
    *pPitch = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!psyche_cuda_driver_mul_size_checked(pitch, Height, &bytes)) {
    *dptr = 0;
    *pPitch = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  result = psyche_cuda_driver_malloc_simulated(dptr, bytes);
  if (result != CUDA_SUCCESS) {
    *pPitch = 0;
    return result;
  }
  *pPitch = pitch;
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_free_simulated(
    CUdeviceptr dptr,
    CUresult missing_error) {
  PsycheCudaDriverAllocation **link = &psyche_cuda_driver_allocations;
  PsycheCudaDriverAllocation *allocation = 0;
  if (dptr == 0) {
    return missing_error;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  while (*link != 0 && (*link)->dptr != dptr) {
    link = &(*link)->next;
  }
  if (*link == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return missing_error;
  }
  allocation = *link;
  *link = allocation->next;
  psyche_cuda_driver_pool_account_free_locked(
      psyche_cuda_driver_find_pool_locked(allocation->pool),
      allocation->size);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  free(allocation->ptr);
  free(allocation);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_host_alloc_simulated(
    void **pp,
    size_t bytesize,
    unsigned int Flags,
    int device_mapped) {
  PsycheCudaDriverHostAllocation *allocation = 0;
  void *ptr = 0;
  const unsigned int allowed_flags =
      CU_MEMHOSTALLOC_PORTABLE |
      CU_MEMHOSTALLOC_DEVICEMAP |
      CU_MEMHOSTALLOC_WRITECOMBINED;
  if ((Flags & ~allowed_flags) != 0) {
    *pp = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (bytesize == 0) {
    *pp = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  ptr = malloc(bytesize);
  if (ptr == 0) {
    *pp = 0;
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  allocation = (PsycheCudaDriverHostAllocation *)malloc(sizeof(*allocation));
  if (allocation == 0) {
    free(ptr);
    *pp = 0;
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  allocation->ptr = ptr;
  allocation->size = bytesize;
  allocation->flags = Flags;
  allocation->owns_memory = 1;
  allocation->registered = 0;
  allocation->device_mapped = device_mapped;
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  allocation->next = psyche_cuda_driver_host_allocations;
  psyche_cuda_driver_host_allocations = allocation;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  *pp = ptr;
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_host_register_simulated(
    void *p,
    size_t bytesize,
    unsigned int Flags) {
  PsycheCudaDriverHostAllocation *allocation = 0;
  const unsigned int allowed_flags =
      CU_MEMHOSTREGISTER_PORTABLE |
      CU_MEMHOSTREGISTER_DEVICEMAP |
      CU_MEMHOSTREGISTER_IOMEMORY |
      CU_MEMHOSTREGISTER_READ_ONLY;
  if (!psyche_cuda_driver_range_is_valid(p, bytesize)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if ((Flags & ~allowed_flags) != 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if ((Flags & CU_MEMHOSTREGISTER_IOMEMORY) != 0) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  allocation = (PsycheCudaDriverHostAllocation *)malloc(sizeof(*allocation));
  if (allocation == 0) {
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  if (psyche_cuda_driver_host_range_registered_locked(p, bytesize)) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    free(allocation);
    return CUDA_ERROR_HOST_MEMORY_ALREADY_REGISTERED;
  }
  if (psyche_cuda_driver_find_touched_allocation_locked(
          (CUdeviceptr)(uintptr_t)p,
          bytesize) != 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    free(allocation);
    return CUDA_ERROR_INVALID_VALUE;
  }
  allocation->ptr = p;
  allocation->size = bytesize;
  allocation->flags = Flags;
  allocation->owns_memory = 0;
  allocation->registered = 1;
  allocation->device_mapped = (Flags & CU_MEMHOSTREGISTER_DEVICEMAP) != 0;
  allocation->next = psyche_cuda_driver_host_allocations;
  psyche_cuda_driver_host_allocations = allocation;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_free_host_simulated(
    void *p,
    CUresult missing_error) {
  PsycheCudaDriverHostAllocation **link = &psyche_cuda_driver_host_allocations;
  PsycheCudaDriverHostAllocation *allocation = 0;
  if (p == 0) {
    return missing_error;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  while (*link != 0 && (*link)->ptr != p) {
    link = &(*link)->next;
  }
  if (*link == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return missing_error;
  }
  allocation = *link;
  if (!allocation->owns_memory) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  *link = allocation->next;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  free(allocation->ptr);
  free(allocation);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_host_unregister_simulated(
    void *p,
    CUresult missing_error) {
  PsycheCudaDriverHostAllocation **link = &psyche_cuda_driver_host_allocations;
  PsycheCudaDriverHostAllocation *allocation = 0;
  if (p == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  while (*link != 0 && (*link)->ptr != p) {
    link = &(*link)->next;
  }
  if (*link == 0 || !(*link)->registered) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return missing_error;
  }
  allocation = *link;
  *link = allocation->next;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  free(allocation);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_host_get_flags_simulated(
    unsigned int *pFlags,
    void *p) {
  PsycheCudaDriverHostAllocation *allocation = 0;
  if (pFlags == 0 || p == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  allocation = psyche_cuda_driver_find_host_allocation_locked(p, 1);
  if (allocation == 0 || !allocation->owns_memory) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  *pFlags = allocation->flags;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_host_get_device_pointer_simulated(
    CUdeviceptr *pdptr,
    void *p,
    unsigned int Flags) {
  PsycheCudaDriverHostAllocation *allocation = 0;
  if (pdptr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (Flags != 0 || p == 0) {
    *pdptr = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  allocation = psyche_cuda_driver_find_host_allocation_locked(p, 1);
  if (allocation == 0 || !allocation->device_mapped) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    *pdptr = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  *pdptr = (CUdeviceptr)(uintptr_t)p;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_memcpy_htod_simulated(
    CUdeviceptr dstDevice,
    const void *srcHost,
    size_t ByteCount) {
  PsycheCudaDriverAllocation *dst_allocation = 0;
  PsycheCudaDriverHostAllocation *dst_mapped_host_allocation = 0;
  if (ByteCount == 0) {
    return CUDA_SUCCESS;
  }
  if (dstDevice == 0 || srcHost == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  dst_allocation = psyche_cuda_driver_find_allocation_locked(dstDevice, ByteCount);
  dst_mapped_host_allocation =
      psyche_cuda_driver_find_mapped_host_allocation_locked(dstDevice, ByteCount);
  if (dst_allocation == 0 && dst_mapped_host_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  memmove((void *)(uintptr_t)dstDevice, srcHost, ByteCount);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_memcpy_dtoh_simulated(
    void *dstHost,
    CUdeviceptr srcDevice,
    size_t ByteCount) {
  PsycheCudaDriverAllocation *src_allocation = 0;
  PsycheCudaDriverHostAllocation *src_mapped_host_allocation = 0;
  if (ByteCount == 0) {
    return CUDA_SUCCESS;
  }
  if (dstHost == 0 || srcDevice == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  src_allocation = psyche_cuda_driver_find_allocation_locked(srcDevice, ByteCount);
  src_mapped_host_allocation =
      psyche_cuda_driver_find_mapped_host_allocation_locked(srcDevice, ByteCount);
  if (src_allocation == 0 && src_mapped_host_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  memmove(dstHost, (const void *)(uintptr_t)srcDevice, ByteCount);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_memcpy_dtod_simulated(
    CUdeviceptr dstDevice,
    CUdeviceptr srcDevice,
    size_t ByteCount) {
  PsycheCudaDriverAllocation *dst_allocation = 0;
  PsycheCudaDriverAllocation *src_allocation = 0;
  PsycheCudaDriverHostAllocation *dst_mapped_host_allocation = 0;
  PsycheCudaDriverHostAllocation *src_mapped_host_allocation = 0;
  if (ByteCount == 0) {
    return CUDA_SUCCESS;
  }
  if (dstDevice == 0 || srcDevice == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  dst_allocation = psyche_cuda_driver_find_allocation_locked(dstDevice, ByteCount);
  src_allocation = psyche_cuda_driver_find_allocation_locked(srcDevice, ByteCount);
  dst_mapped_host_allocation =
      psyche_cuda_driver_find_mapped_host_allocation_locked(dstDevice, ByteCount);
  src_mapped_host_allocation =
      psyche_cuda_driver_find_mapped_host_allocation_locked(srcDevice, ByteCount);
  if (
      (dst_allocation == 0 && dst_mapped_host_allocation == 0) ||
      (src_allocation == 0 && src_mapped_host_allocation == 0)) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  memmove((void *)(uintptr_t)dstDevice, (const void *)(uintptr_t)srcDevice, ByteCount);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_memcpy_simulated(
    CUdeviceptr dst,
    CUdeviceptr src,
    size_t ByteCount) {
  PsycheCudaDriverAllocation *dst_allocation = 0;
  PsycheCudaDriverAllocation *src_allocation = 0;
  PsycheCudaDriverAllocation *dst_touched_allocation = 0;
  PsycheCudaDriverAllocation *src_touched_allocation = 0;
  PsycheCudaDriverHostAllocation *dst_mapped_host_allocation = 0;
  PsycheCudaDriverHostAllocation *src_mapped_host_allocation = 0;
  PsycheCudaDriverHostAllocation *dst_touched_mapped_host_allocation = 0;
  PsycheCudaDriverHostAllocation *src_touched_mapped_host_allocation = 0;
  int dst_device_accessible = 0;
  int src_device_accessible = 0;
  if (ByteCount == 0) {
    return CUDA_SUCCESS;
  }
  if (dst == 0 || src == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  dst_allocation = psyche_cuda_driver_find_allocation_locked(dst, ByteCount);
  src_allocation = psyche_cuda_driver_find_allocation_locked(src, ByteCount);
  dst_touched_allocation = psyche_cuda_driver_find_touched_allocation_locked(dst, ByteCount);
  src_touched_allocation = psyche_cuda_driver_find_touched_allocation_locked(src, ByteCount);
  dst_mapped_host_allocation =
      psyche_cuda_driver_find_mapped_host_allocation_locked(dst, ByteCount);
  src_mapped_host_allocation =
      psyche_cuda_driver_find_mapped_host_allocation_locked(src, ByteCount);
  dst_touched_mapped_host_allocation =
      psyche_cuda_driver_find_touched_mapped_host_allocation_locked(dst, ByteCount);
  src_touched_mapped_host_allocation =
      psyche_cuda_driver_find_touched_mapped_host_allocation_locked(src, ByteCount);
  dst_device_accessible = dst_allocation != 0 || dst_mapped_host_allocation != 0;
  src_device_accessible = src_allocation != 0 || src_mapped_host_allocation != 0;
  if (
      (dst_touched_allocation != 0 && dst_allocation == 0) ||
      (src_touched_allocation != 0 && src_allocation == 0) ||
      (dst_touched_mapped_host_allocation != 0 && dst_mapped_host_allocation == 0) ||
      (src_touched_mapped_host_allocation != 0 && src_mapped_host_allocation == 0) ||
      !dst_device_accessible ||
      !src_device_accessible) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, ByteCount);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_validate_row_copy_locked(
    CUdeviceptr dst,
    CUdeviceptr src,
    size_t ByteCount,
    int dst_device_memory,
    int src_device_memory) {
  PsycheCudaDriverAllocation *dst_allocation = 0;
  PsycheCudaDriverAllocation *src_allocation = 0;
  PsycheCudaDriverAllocation *dst_touched_allocation = 0;
  PsycheCudaDriverAllocation *src_touched_allocation = 0;
  PsycheCudaDriverHostAllocation *dst_mapped_host_allocation = 0;
  PsycheCudaDriverHostAllocation *src_mapped_host_allocation = 0;
  PsycheCudaDriverHostAllocation *dst_touched_mapped_host_allocation = 0;
  PsycheCudaDriverHostAllocation *src_touched_mapped_host_allocation = 0;
  int dst_device_accessible = 0;
  int src_device_accessible = 0;

  dst_allocation = psyche_cuda_driver_find_allocation_locked(dst, ByteCount);
  src_allocation = psyche_cuda_driver_find_allocation_locked(src, ByteCount);
  dst_touched_allocation = psyche_cuda_driver_find_touched_allocation_locked(dst, ByteCount);
  src_touched_allocation = psyche_cuda_driver_find_touched_allocation_locked(src, ByteCount);
  dst_mapped_host_allocation =
      psyche_cuda_driver_find_mapped_host_allocation_locked(dst, ByteCount);
  src_mapped_host_allocation =
      psyche_cuda_driver_find_mapped_host_allocation_locked(src, ByteCount);
  dst_touched_mapped_host_allocation =
      psyche_cuda_driver_find_touched_mapped_host_allocation_locked(dst, ByteCount);
  src_touched_mapped_host_allocation =
      psyche_cuda_driver_find_touched_mapped_host_allocation_locked(src, ByteCount);
  dst_device_accessible = dst_allocation != 0 || dst_mapped_host_allocation != 0;
  src_device_accessible = src_allocation != 0 || src_mapped_host_allocation != 0;

  if (
      (dst_touched_allocation != 0 && dst_allocation == 0) ||
      (src_touched_allocation != 0 && src_allocation == 0) ||
      (dst_touched_mapped_host_allocation != 0 && dst_mapped_host_allocation == 0) ||
      (src_touched_mapped_host_allocation != 0 && src_mapped_host_allocation == 0) ||
      (dst_device_memory && !dst_device_accessible) ||
      (src_device_memory && !src_device_accessible) ||
      (!dst_device_memory && dst_allocation != 0) ||
      (!src_device_memory && src_allocation != 0)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_offset_deviceptr(
    CUdeviceptr base,
    size_t pitch,
    size_t row,
    size_t x_offset,
    CUdeviceptr *row_ptr) {
  size_t row_offset = 0;
  size_t total_offset = 0;
  if (!psyche_cuda_driver_mul_size_checked(pitch, row, &row_offset)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!psyche_cuda_driver_add_size_checked(row_offset, x_offset, &total_offset)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (total_offset > UINTPTR_MAX - (uintptr_t)base) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  *row_ptr = (CUdeviceptr)((uintptr_t)base + total_offset);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_memcpy_2d_simulated(const CUDA_MEMCPY2D *pCopy) {
  CUdeviceptr src_base = 0;
  CUdeviceptr dst_base = 0;
  int src_device_memory = 0;
  int dst_device_memory = 0;
  size_t src_required_pitch = 0;
  size_t dst_required_pitch = 0;

  if (pCopy == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (pCopy->WidthInBytes == 0 || pCopy->Height == 0) {
    return CUDA_SUCCESS;
  }
  if (
      !psyche_cuda_driver_add_size_checked(
          pCopy->WidthInBytes,
          pCopy->srcXInBytes,
          &src_required_pitch) ||
      !psyche_cuda_driver_add_size_checked(
          pCopy->WidthInBytes,
          pCopy->dstXInBytes,
          &dst_required_pitch) ||
      pCopy->srcPitch < src_required_pitch ||
      pCopy->dstPitch < dst_required_pitch) {
    return CUDA_ERROR_INVALID_VALUE;
  }

  if (pCopy->srcMemoryType == CU_MEMORYTYPE_HOST) {
    if (pCopy->srcHost == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    src_base = (CUdeviceptr)(uintptr_t)pCopy->srcHost;
  } else if (pCopy->srcMemoryType == CU_MEMORYTYPE_DEVICE) {
    if (pCopy->srcDevice == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    src_base = pCopy->srcDevice;
    src_device_memory = 1;
  } else {
    return CUDA_ERROR_NOT_SUPPORTED;
  }

  if (pCopy->dstMemoryType == CU_MEMORYTYPE_HOST) {
    if (pCopy->dstHost == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    dst_base = (CUdeviceptr)(uintptr_t)pCopy->dstHost;
  } else if (pCopy->dstMemoryType == CU_MEMORYTYPE_DEVICE) {
    if (pCopy->dstDevice == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    dst_base = pCopy->dstDevice;
    dst_device_memory = 1;
  } else {
    return CUDA_ERROR_NOT_SUPPORTED;
  }

  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  PsycheCudaDriverAllocation *dst_expected_allocation = 0;
  PsycheCudaDriverAllocation *src_expected_allocation = 0;
  PsycheCudaDriverHostAllocation *dst_expected_mapped_host_allocation = 0;
  PsycheCudaDriverHostAllocation *src_expected_mapped_host_allocation = 0;
  int dst_expected_set = 0;
  int src_expected_set = 0;
  for (size_t row = 0; row < pCopy->Height; row++) {
    CUdeviceptr src_row = 0;
    CUdeviceptr dst_row = 0;
    size_t src_row_index = 0;
    size_t dst_row_index = 0;
    if (
        !psyche_cuda_driver_add_size_checked(row, pCopy->srcY, &src_row_index) ||
        !psyche_cuda_driver_add_size_checked(row, pCopy->dstY, &dst_row_index)) {
      pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
      return CUDA_ERROR_INVALID_VALUE;
    }
    CUresult result = psyche_cuda_driver_offset_deviceptr(
        src_base,
        pCopy->srcPitch,
        src_row_index,
        pCopy->srcXInBytes,
        &src_row);
    if (result != CUDA_SUCCESS) {
      pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
      return result;
    }
    result = psyche_cuda_driver_offset_deviceptr(
        dst_base,
        pCopy->dstPitch,
        dst_row_index,
        pCopy->dstXInBytes,
        &dst_row);
    if (result != CUDA_SUCCESS) {
      pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
      return result;
    }
    result = psyche_cuda_driver_validate_row_copy_locked(
        dst_row,
        src_row,
        pCopy->WidthInBytes,
        dst_device_memory,
        src_device_memory);
    if (result != CUDA_SUCCESS) {
      pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
      return result;
    }
    if (dst_device_memory) {
      PsycheCudaDriverAllocation *allocation =
          psyche_cuda_driver_find_allocation_locked(dst_row, pCopy->WidthInBytes);
      PsycheCudaDriverHostAllocation *mapped_host_allocation =
          psyche_cuda_driver_find_mapped_host_allocation_locked(dst_row, pCopy->WidthInBytes);
      if (!dst_expected_set) {
        dst_expected_allocation = allocation;
        dst_expected_mapped_host_allocation = mapped_host_allocation;
        dst_expected_set = 1;
      } else if (
          allocation != dst_expected_allocation ||
          mapped_host_allocation != dst_expected_mapped_host_allocation) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return CUDA_ERROR_INVALID_VALUE;
      }
    }
    if (src_device_memory) {
      PsycheCudaDriverAllocation *allocation =
          psyche_cuda_driver_find_allocation_locked(src_row, pCopy->WidthInBytes);
      PsycheCudaDriverHostAllocation *mapped_host_allocation =
          psyche_cuda_driver_find_mapped_host_allocation_locked(src_row, pCopy->WidthInBytes);
      if (!src_expected_set) {
        src_expected_allocation = allocation;
        src_expected_mapped_host_allocation = mapped_host_allocation;
        src_expected_set = 1;
      } else if (
          allocation != src_expected_allocation ||
          mapped_host_allocation != src_expected_mapped_host_allocation) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return CUDA_ERROR_INVALID_VALUE;
      }
    }
  }
  for (size_t row = 0; row < pCopy->Height; row++) {
    CUdeviceptr src_row = 0;
    CUdeviceptr dst_row = 0;
    size_t src_row_index = 0;
    size_t dst_row_index = 0;
    (void)psyche_cuda_driver_add_size_checked(row, pCopy->srcY, &src_row_index);
    (void)psyche_cuda_driver_add_size_checked(row, pCopy->dstY, &dst_row_index);
    (void)psyche_cuda_driver_offset_deviceptr(
        src_base,
        pCopy->srcPitch,
        src_row_index,
        pCopy->srcXInBytes,
        &src_row);
    (void)psyche_cuda_driver_offset_deviceptr(
        dst_base,
        pCopy->dstPitch,
        dst_row_index,
        pCopy->dstXInBytes,
        &dst_row);
    memmove((void *)(uintptr_t)dst_row, (const void *)(uintptr_t)src_row, pCopy->WidthInBytes);
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_offset_3d_deviceptr(
    CUdeviceptr base,
    size_t pitch,
    size_t slice_height,
    size_t z,
    size_t y,
    size_t row,
    size_t x_offset,
    CUdeviceptr *row_ptr) {
  size_t slice_rows = 0;
  size_t y_rows = 0;
  size_t row_index = 0;
  if (!psyche_cuda_driver_mul_size_checked(slice_height, z, &slice_rows)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!psyche_cuda_driver_add_size_checked(slice_rows, y, &y_rows)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!psyche_cuda_driver_add_size_checked(y_rows, row, &row_index)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  return psyche_cuda_driver_offset_deviceptr(base, pitch, row_index, x_offset, row_ptr);
}

static CUresult psyche_cuda_driver_validate_3d_side(
    size_t pitch,
    size_t slice_height,
    size_t x_offset,
    size_t y_offset,
    size_t z_offset,
    size_t width,
    size_t height,
    size_t depth) {
  size_t required_pitch = 0;
  size_t required_height = 0;
  if (!psyche_cuda_driver_add_size_checked(width, x_offset, &required_pitch)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (pitch < required_pitch) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!psyche_cuda_driver_add_size_checked(height, y_offset, &required_height)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if ((depth > 1 || z_offset > 0) && slice_height == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (slice_height != 0 && slice_height < required_height) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_memcpy_3d_simulated(const CUDA_MEMCPY3D *pCopy) {
  CUdeviceptr src_base = 0;
  CUdeviceptr dst_base = 0;
  int src_device_memory = 0;
  int dst_device_memory = 0;
  void *staged_copy = 0;
  size_t staged_rows = 0;
  size_t staged_bytes = 0;
  CUresult result = CUDA_SUCCESS;

  if (pCopy == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      pCopy->srcLOD != 0 ||
      pCopy->dstLOD != 0 ||
      pCopy->reserved0 != 0 ||
      pCopy->reserved1 != 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (
      (pCopy->srcMemoryType != CU_MEMORYTYPE_HOST &&
       pCopy->srcMemoryType != CU_MEMORYTYPE_DEVICE) ||
      (pCopy->dstMemoryType != CU_MEMORYTYPE_HOST &&
       pCopy->dstMemoryType != CU_MEMORYTYPE_DEVICE)) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  if (pCopy->WidthInBytes == 0 || pCopy->Height == 0 || pCopy->Depth == 0) {
    return CUDA_SUCCESS;
  }

  if (pCopy->srcMemoryType == CU_MEMORYTYPE_HOST) {
    if (pCopy->srcHost == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    src_base = (CUdeviceptr)(uintptr_t)pCopy->srcHost;
  } else if (pCopy->srcMemoryType == CU_MEMORYTYPE_DEVICE) {
    if (pCopy->srcDevice == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    src_base = pCopy->srcDevice;
    src_device_memory = 1;
  }

  if (pCopy->dstMemoryType == CU_MEMORYTYPE_HOST) {
    if (pCopy->dstHost == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    dst_base = (CUdeviceptr)(uintptr_t)pCopy->dstHost;
  } else if (pCopy->dstMemoryType == CU_MEMORYTYPE_DEVICE) {
    if (pCopy->dstDevice == 0) {
      return CUDA_ERROR_INVALID_VALUE;
    }
    dst_base = pCopy->dstDevice;
    dst_device_memory = 1;
  }

  result = psyche_cuda_driver_validate_3d_side(
      pCopy->srcPitch,
      pCopy->srcHeight,
      pCopy->srcXInBytes,
      pCopy->srcY,
      pCopy->srcZ,
      pCopy->WidthInBytes,
      pCopy->Height,
      pCopy->Depth);
  if (result != CUDA_SUCCESS) {
    return result;
  }
  result = psyche_cuda_driver_validate_3d_side(
      pCopy->dstPitch,
      pCopy->dstHeight,
      pCopy->dstXInBytes,
      pCopy->dstY,
      pCopy->dstZ,
      pCopy->WidthInBytes,
      pCopy->Height,
      pCopy->Depth);
  if (result != CUDA_SUCCESS) {
    return result;
  }
  if (
      !psyche_cuda_driver_mul_size_checked(pCopy->Height, pCopy->Depth, &staged_rows) ||
      !psyche_cuda_driver_mul_size_checked(pCopy->WidthInBytes, staged_rows, &staged_bytes)) {
    return CUDA_ERROR_INVALID_VALUE;
  }

  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  PsycheCudaDriverAllocation *dst_expected_allocation = 0;
  PsycheCudaDriverAllocation *src_expected_allocation = 0;
  PsycheCudaDriverHostAllocation *dst_expected_mapped_host_allocation = 0;
  PsycheCudaDriverHostAllocation *src_expected_mapped_host_allocation = 0;
  int dst_expected_set = 0;
  int src_expected_set = 0;
  for (size_t z = 0; z < pCopy->Depth; z++) {
    for (size_t row = 0; row < pCopy->Height; row++) {
      CUdeviceptr src_row = 0;
      CUdeviceptr dst_row = 0;
      size_t src_z = 0;
      size_t dst_z = 0;
      if (
          !psyche_cuda_driver_add_size_checked(pCopy->srcZ, z, &src_z) ||
          !psyche_cuda_driver_add_size_checked(pCopy->dstZ, z, &dst_z)) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return CUDA_ERROR_INVALID_VALUE;
      }
      result = psyche_cuda_driver_offset_3d_deviceptr(
          src_base,
          pCopy->srcPitch,
          pCopy->srcHeight,
          src_z,
          pCopy->srcY,
          row,
          pCopy->srcXInBytes,
          &src_row);
      if (result != CUDA_SUCCESS) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return result;
      }
      result = psyche_cuda_driver_offset_3d_deviceptr(
          dst_base,
          pCopy->dstPitch,
          pCopy->dstHeight,
          dst_z,
          pCopy->dstY,
          row,
          pCopy->dstXInBytes,
          &dst_row);
      if (result != CUDA_SUCCESS) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return result;
      }
      result = psyche_cuda_driver_validate_row_copy_locked(
          dst_row,
          src_row,
          pCopy->WidthInBytes,
          dst_device_memory,
          src_device_memory);
      if (result != CUDA_SUCCESS) {
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return result;
      }
      if (dst_device_memory) {
        PsycheCudaDriverAllocation *allocation =
            psyche_cuda_driver_find_allocation_locked(dst_row, pCopy->WidthInBytes);
        PsycheCudaDriverHostAllocation *mapped_host_allocation =
            psyche_cuda_driver_find_mapped_host_allocation_locked(dst_row, pCopy->WidthInBytes);
        if (!dst_expected_set) {
          dst_expected_allocation = allocation;
          dst_expected_mapped_host_allocation = mapped_host_allocation;
          dst_expected_set = 1;
        } else if (
            allocation != dst_expected_allocation ||
            mapped_host_allocation != dst_expected_mapped_host_allocation) {
          pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
          return CUDA_ERROR_INVALID_VALUE;
        }
      }
      if (src_device_memory) {
        PsycheCudaDriverAllocation *allocation =
            psyche_cuda_driver_find_allocation_locked(src_row, pCopy->WidthInBytes);
        PsycheCudaDriverHostAllocation *mapped_host_allocation =
            psyche_cuda_driver_find_mapped_host_allocation_locked(src_row, pCopy->WidthInBytes);
        if (!src_expected_set) {
          src_expected_allocation = allocation;
          src_expected_mapped_host_allocation = mapped_host_allocation;
          src_expected_set = 1;
        } else if (
            allocation != src_expected_allocation ||
            mapped_host_allocation != src_expected_mapped_host_allocation) {
          pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
          return CUDA_ERROR_INVALID_VALUE;
        }
      }
    }
  }
  staged_copy = malloc(staged_bytes);
  if (staged_copy == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  for (size_t z = 0; z < pCopy->Depth; z++) {
    for (size_t row = 0; row < pCopy->Height; row++) {
      CUdeviceptr src_row = 0;
      size_t src_z = 0;
      size_t staged_row = 0;
      (void)psyche_cuda_driver_add_size_checked(pCopy->srcZ, z, &src_z);
      (void)psyche_cuda_driver_mul_size_checked(pCopy->Height, z, &staged_row);
      (void)psyche_cuda_driver_add_size_checked(staged_row, row, &staged_row);
      (void)psyche_cuda_driver_offset_3d_deviceptr(
          src_base,
          pCopy->srcPitch,
          pCopy->srcHeight,
          src_z,
          pCopy->srcY,
          row,
          pCopy->srcXInBytes,
          &src_row);
      memcpy(
          (char *)staged_copy + (staged_row * pCopy->WidthInBytes),
          (const void *)(uintptr_t)src_row,
          pCopy->WidthInBytes);
    }
  }
  for (size_t z = 0; z < pCopy->Depth; z++) {
    for (size_t row = 0; row < pCopy->Height; row++) {
      CUdeviceptr dst_row = 0;
      size_t dst_z = 0;
      size_t staged_row = 0;
      (void)psyche_cuda_driver_add_size_checked(pCopy->dstZ, z, &dst_z);
      (void)psyche_cuda_driver_mul_size_checked(pCopy->Height, z, &staged_row);
      (void)psyche_cuda_driver_add_size_checked(staged_row, row, &staged_row);
      (void)psyche_cuda_driver_offset_3d_deviceptr(
          dst_base,
          pCopy->dstPitch,
          pCopy->dstHeight,
          dst_z,
          pCopy->dstY,
          row,
          pCopy->dstXInBytes,
          &dst_row);
      memcpy(
          (void *)(uintptr_t)dst_row,
          (const char *)staged_copy + (staged_row * pCopy->WidthInBytes),
          pCopy->WidthInBytes);
    }
  }
  free(staged_copy);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_memset_d8_simulated(
    CUdeviceptr dstDevice,
    unsigned char uc,
    size_t N) {
  PsycheCudaDriverAllocation *dst_allocation = 0;
  PsycheCudaDriverHostAllocation *dst_mapped_host_allocation = 0;
  if (N == 0) {
    return CUDA_SUCCESS;
  }
  if (dstDevice == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  dst_allocation = psyche_cuda_driver_find_allocation_locked(dstDevice, N);
  dst_mapped_host_allocation =
      psyche_cuda_driver_find_mapped_host_allocation_locked(dstDevice, N);
  if (dst_allocation == 0 && dst_mapped_host_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  memset((void *)(uintptr_t)dstDevice, uc, N);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_memset_2d_pattern_simulated(
    CUdeviceptr dstDevice,
    size_t dstPitch,
    const void *pattern,
    size_t element_size,
    size_t alignment,
    size_t Width,
    size_t Height) {
  size_t row_bytes = 0;
  if (!psyche_cuda_driver_mul_size_checked(Width, element_size, &row_bytes)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (Width == 0 || Height == 0) {
    return CUDA_SUCCESS;
  }
  if (
      dstDevice == 0 ||
      ((uintptr_t)dstDevice % alignment) != 0 ||
      (dstPitch % alignment) != 0 ||
      row_bytes > dstPitch) {
    return CUDA_ERROR_INVALID_VALUE;
  }

  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  PsycheCudaDriverAllocation *expected_allocation = 0;
  PsycheCudaDriverHostAllocation *expected_mapped_host_allocation = 0;
  int expected_set = 0;
  for (size_t row = 0; row < Height; row++) {
    CUdeviceptr row_ptr = 0;
    PsycheCudaDriverAllocation *allocation = 0;
    PsycheCudaDriverHostAllocation *mapped_host_allocation = 0;
    CUresult result =
        psyche_cuda_driver_offset_deviceptr(dstDevice, dstPitch, row, 0, &row_ptr);
    if (result != CUDA_SUCCESS) {
      pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
      return result;
    }
    allocation = psyche_cuda_driver_find_allocation_locked(row_ptr, row_bytes);
    mapped_host_allocation =
        psyche_cuda_driver_find_mapped_host_allocation_locked(row_ptr, row_bytes);
    if (allocation == 0 && mapped_host_allocation == 0) {
      pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
      return CUDA_ERROR_INVALID_VALUE;
    }
    if (!expected_set) {
      expected_allocation = allocation;
      expected_mapped_host_allocation = mapped_host_allocation;
      expected_set = 1;
    } else if (
        allocation != expected_allocation ||
        mapped_host_allocation != expected_mapped_host_allocation) {
      pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
      return CUDA_ERROR_INVALID_VALUE;
    }
  }
  for (size_t row = 0; row < Height; row++) {
    CUdeviceptr row_ptr = 0;
    (void)psyche_cuda_driver_offset_deviceptr(dstDevice, dstPitch, row, 0, &row_ptr);
    for (size_t index = 0; index < Width; index++) {
      memcpy((void *)((uintptr_t)row_ptr + index * element_size), pattern, element_size);
    }
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

static CUresult psyche_cuda_driver_memset_pattern_simulated(
    CUdeviceptr dstDevice,
    const void *pattern,
    size_t element_size,
    size_t alignment,
    size_t N) {
  PsycheCudaDriverAllocation *dst_allocation = 0;
  PsycheCudaDriverHostAllocation *dst_mapped_host_allocation = 0;
  size_t bytes = 0;
  if (!psyche_cuda_driver_mul_size_checked(N, element_size, &bytes)) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (N == 0) {
    return CUDA_SUCCESS;
  }
  if (dstDevice == 0 || ((uintptr_t)dstDevice % alignment) != 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  dst_allocation = psyche_cuda_driver_find_allocation_locked(dstDevice, bytes);
  dst_mapped_host_allocation =
      psyche_cuda_driver_find_mapped_host_allocation_locked(dstDevice, bytes);
  if (dst_allocation == 0 && dst_mapped_host_allocation == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  /* Apple Silicon is little-endian; native integer bytes match this shim target. */
  for (size_t index = 0; index < N; index++) {
    memcpy((void *)((uintptr_t)dstDevice + index * element_size), pattern, element_size);
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuInit(unsigned int flags) {
  if (flags != 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  atomic_store(&psyche_cuda_driver_initialized, 1);
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuDriverGetVersion(int *driverVersion) {
  if (driverVersion == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  /* Version probes are allowed before init, but version 0 means "stub/no driver". */
  *driverVersion = 0;
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuDeviceGetCount(int *count) {
  if (count == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *count = 0;
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuDeviceGet(CUdevice *device, int ordinal) {
  (void)ordinal;
  if (device == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  return CUDA_ERROR_INVALID_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuDeviceGetName(char *name, int len, CUdevice dev) {
  (void)name;
  (void)len;
  (void)dev;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  return CUDA_ERROR_INVALID_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuDeviceTotalMem(size_t *bytes, CUdevice dev) {
  (void)dev;
  if (bytes == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *bytes = 0;
  return CUDA_ERROR_INVALID_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuDeviceGetAttribute(int *pi, int attrib, CUdevice dev) {
  (void)attrib;
  (void)dev;
  if (pi == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *pi = 0;
  return CUDA_ERROR_INVALID_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuCtxGetCurrent(CUcontext *pctx) {
  if (pctx == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *pctx = 0;
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuCtxSynchronize(void) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemGetInfo(size_t *free, size_t *total) {
  if (free == 0 || total == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  /* Successful accounting with zero bytes is not evidence of a CUDA device. */
  *free = 0;
  *total = 0;
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuMemAlloc(CUdeviceptr *dptr, size_t bytesize) {
  if (dptr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_malloc_simulated(dptr, bytesize);
  }
  (void)bytesize;
  *dptr = 0;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize) {
  return cuMemAlloc(dptr, bytesize);
}

PSYCHE_CUDA_STUB_API CUresult cuMemAllocAsync(
    CUdeviceptr *dptr,
    size_t bytesize,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (dptr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    *dptr = 0;
    return CUDA_ERROR_NO_DEVICE;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    *dptr = 0;
    return stream_result;
  }
  return psyche_cuda_driver_malloc_kind_simulated(dptr, bytesize, 0, 0, 1, 1);
}

PSYCHE_CUDA_STUB_API CUresult cuMemAllocFromPoolAsync(
    CUdeviceptr *dptr,
    size_t bytesize,
    CUmemoryPool pool,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (dptr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    *dptr = 0;
    return CUDA_ERROR_NO_DEVICE;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    *dptr = 0;
    return stream_result;
  }
  return psyche_cuda_driver_malloc_kind_simulated(dptr, bytesize, 0, pool, 0, 1);
}

PSYCHE_CUDA_STUB_API CUresult cuMemAllocManaged(
    CUdeviceptr *dptr,
    size_t bytesize,
    unsigned int flags) {
  if (dptr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_malloc_managed_simulated(dptr, bytesize, flags);
  }
  (void)bytesize;
  (void)flags;
  *dptr = 0;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemAllocPitch(
    CUdeviceptr *dptr,
    size_t *pPitch,
    size_t WidthInBytes,
    size_t Height,
    unsigned int ElementSizeBytes) {
  if (dptr == 0 || pPitch == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_malloc_pitch_simulated(
        dptr,
        pPitch,
        WidthInBytes,
        Height,
        ElementSizeBytes);
  }
  *dptr = 0;
  *pPitch = 0;
  (void)WidthInBytes;
  (void)Height;
  (void)ElementSizeBytes;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemAllocPitch_v2(
    CUdeviceptr *dptr,
    size_t *pPitch,
    size_t WidthInBytes,
    size_t Height,
    unsigned int ElementSizeBytes) {
  return cuMemAllocPitch(dptr, pPitch, WidthInBytes, Height, ElementSizeBytes);
}

PSYCHE_CUDA_STUB_API CUresult cuMemFree(CUdeviceptr dptr) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_free_simulated(dptr, CUDA_ERROR_INVALID_VALUE);
  }
  return psyche_cuda_driver_free_simulated(dptr, CUDA_ERROR_NO_DEVICE);
}

PSYCHE_CUDA_STUB_API CUresult cuMemFree_v2(CUdeviceptr dptr) {
  return cuMemFree(dptr);
}

PSYCHE_CUDA_STUB_API CUresult cuMemFreeAsync(CUdeviceptr dptr, CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return dptr == 0 ? CUDA_ERROR_INVALID_VALUE : CUDA_ERROR_NO_DEVICE;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  return psyche_cuda_driver_free_simulated(dptr, CUDA_ERROR_INVALID_VALUE);
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolCreate(
    CUmemoryPool *pool,
    const CUmemPoolProps *poolProps) {
  if (pool == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    *pool = 0;
    return CUDA_ERROR_NO_DEVICE;
  }
  return psyche_cuda_driver_mem_pool_create_simulated(pool, poolProps);
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolDestroy(CUmemoryPool pool) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  return psyche_cuda_driver_mem_pool_destroy_simulated(pool);
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolGetAttribute(
    CUmemoryPool pool,
    CUmemPool_attribute attr,
    void *value) {
  CUresult result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  result = psyche_cuda_driver_mem_pool_get_attribute_locked(pool, attr, value);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolSetAttribute(
    CUmemoryPool pool,
    CUmemPool_attribute attr,
    void *value) {
  CUresult result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  result = psyche_cuda_driver_mem_pool_set_attribute_locked(pool, attr, value);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolTrimTo(
    CUmemoryPool pool,
    size_t minBytesToKeep) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  return psyche_cuda_driver_mem_pool_trim_to_simulated(pool, minBytesToKeep);
}

PSYCHE_CUDA_STUB_API CUresult cuMemGetDefaultMemPool(
    CUmemoryPool *pool_out,
    CUmemLocation *location,
    CUmemAllocationType type) {
  CUresult result = CUDA_SUCCESS;
  if (pool_out == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    *pool_out = 0;
    return CUDA_ERROR_NO_DEVICE;
  }
  result = psyche_cuda_driver_validate_mem_pool_location_request(location, type);
  if (result != CUDA_SUCCESS) {
    *pool_out = 0;
    return result;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  *pool_out = psyche_cuda_driver_default_pool_locked(type)->handle;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuMemGetMemPool(
    CUmemoryPool *pool,
    CUmemLocation *location,
    CUmemAllocationType type) {
  CUresult result = CUDA_SUCCESS;
  if (pool == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    *pool = 0;
    return CUDA_ERROR_NO_DEVICE;
  }
  result = psyche_cuda_driver_validate_mem_pool_location_request(location, type);
  if (result != CUDA_SUCCESS) {
    *pool = 0;
    return result;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  *pool = psyche_cuda_driver_current_pool_locked(type)->handle;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuMemSetMemPool(
    CUmemLocation *location,
    CUmemAllocationType type,
    CUmemoryPool pool) {
  PsycheCudaDriverMemoryPool *record = 0;
  CUresult result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  result = psyche_cuda_driver_validate_mem_pool_location_request(location, type);
  if (result != CUDA_SUCCESS) {
    return result;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  record = psyche_cuda_driver_find_pool_locked(pool);
  if (record == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return pool == 0 ? CUDA_ERROR_INVALID_VALUE : CUDA_ERROR_INVALID_HANDLE;
  }
  if (
      record->alloc_type != type ||
      record->location.type != location->type) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (type == CU_MEM_ALLOCATION_TYPE_MANAGED) {
    psyche_cuda_driver_current_managed_pool = record->handle;
  } else {
    psyche_cuda_driver_current_host_pool = record->handle;
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuDeviceGetDefaultMemPool(
    CUmemoryPool *pool_out,
    CUdevice dev) {
  (void)dev;
  if (pool_out == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *pool_out = 0;
  return CUDA_ERROR_INVALID_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuDeviceGetMemPool(CUmemoryPool *pool, CUdevice dev) {
  (void)dev;
  if (pool == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *pool = 0;
  return CUDA_ERROR_INVALID_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuDeviceSetMemPool(CUdevice dev, CUmemoryPool pool) {
  (void)dev;
  (void)pool;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  return CUDA_ERROR_INVALID_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolGetAccess(
    CUmemAccess_flags *flags,
    CUmemoryPool memPool,
    CUmemLocation *location) {
  (void)memPool;
  (void)location;
  if (flags == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *flags = CU_MEM_ACCESS_FLAGS_PROT_NONE;
  return psyche_cuda_driver_simulated_memory_enabled() ?
      CUDA_ERROR_NOT_SUPPORTED :
      CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolSetAccess(
    CUmemoryPool memPool,
    const CUmemAccessDesc *descList,
    size_t count) {
  (void)memPool;
  (void)descList;
  (void)count;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  return psyche_cuda_driver_simulated_memory_enabled() ?
      CUDA_ERROR_NOT_SUPPORTED :
      CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolExportToShareableHandle(
    void *handle_out,
    CUmemoryPool pool,
    CUmemAllocationHandleType handleType,
    unsigned long long flags) {
  (void)pool;
  (void)handleType;
  (void)flags;
  if (handle_out == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  memset(handle_out, 0, sizeof(void *));
  return psyche_cuda_driver_simulated_memory_enabled() ?
      CUDA_ERROR_NOT_SUPPORTED :
      CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolImportFromShareableHandle(
    CUmemoryPool *pool_out,
    void *handle,
    CUmemAllocationHandleType handleType,
    unsigned long long flags) {
  (void)handle;
  (void)handleType;
  (void)flags;
  if (pool_out == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *pool_out = 0;
  return psyche_cuda_driver_simulated_memory_enabled() ?
      CUDA_ERROR_NOT_SUPPORTED :
      CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolExportPointer(
    CUmemPoolPtrExportData *shareData_out,
    CUdeviceptr ptr) {
  (void)ptr;
  if (shareData_out == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  memset(shareData_out, 0, sizeof(*shareData_out));
  return psyche_cuda_driver_simulated_memory_enabled() ?
      CUDA_ERROR_NOT_SUPPORTED :
      CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemPoolImportPointer(
    CUdeviceptr *ptr_out,
    CUmemoryPool pool,
    CUmemPoolPtrExportData *shareData) {
  (void)pool;
  (void)shareData;
  if (ptr_out == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *ptr_out = 0;
  return psyche_cuda_driver_simulated_memory_enabled() ?
      CUDA_ERROR_NOT_SUPPORTED :
      CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemAdvise(
    CUdeviceptr devPtr,
    size_t count,
    CUmem_advise advice,
    CUdevice device) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_mem_advise_simulated(
        devPtr,
        count,
        advice,
        device);
  }
  (void)devPtr;
  (void)count;
  (void)advice;
  (void)device;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemPrefetchAsync(
    CUdeviceptr devPtr,
    size_t count,
    CUdevice dstDevice,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    stream_result = psyche_cuda_driver_validate_async_stream(hStream);
    if (stream_result != CUDA_SUCCESS) {
      return stream_result;
    }
    return psyche_cuda_driver_mem_prefetch_simulated(devPtr, count, dstDevice);
  }
  (void)devPtr;
  (void)count;
  (void)dstDevice;
  (void)hStream;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemRangeGetAttribute(
    void *data,
    size_t dataSize,
    CUmem_range_attribute attribute,
    CUdeviceptr devPtr,
    size_t count) {
  CUresult result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  result = psyche_cuda_driver_mem_range_get_attribute_locked(
      data,
      dataSize,
      attribute,
      devPtr,
      count);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

PSYCHE_CUDA_STUB_API CUresult cuMemRangeGetAttributes(
    void **data,
    size_t *dataSizes,
    CUmem_range_attribute *attributes,
    size_t numAttributes,
    CUdeviceptr devPtr,
    size_t count) {
  CUresult result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  if (numAttributes == 0) {
    return CUDA_SUCCESS;
  }
  if (data == 0 || dataSizes == 0 || attributes == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  for (size_t index = 0; index < numAttributes; index++) {
    result = psyche_cuda_driver_mem_range_get_attribute_locked(
        data[index],
        dataSizes[index],
        attributes[index],
        devPtr,
        count);
    if (result != CUDA_SUCCESS) {
      break;
    }
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

PSYCHE_CUDA_STUB_API CUresult cuPointerGetAttribute(
    void *data,
    CUpointer_attribute attribute,
    CUdeviceptr ptr) {
  CUresult result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  result = psyche_cuda_driver_pointer_get_attribute_locked(data, attribute, ptr);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

PSYCHE_CUDA_STUB_API CUresult cuPointerGetAttributes(
    unsigned int numAttributes,
    CUpointer_attribute *attributes,
    void **data,
    CUdeviceptr ptr) {
  CUresult result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  if (numAttributes == 0) {
    return CUDA_SUCCESS;
  }
  if (attributes == 0 || data == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  for (unsigned int index = 0; index < numAttributes; index++) {
    result = psyche_cuda_driver_pointer_get_attribute_locked(
        data[index],
        attributes[index],
        ptr);
    if (result != CUDA_SUCCESS) {
      break;
    }
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

PSYCHE_CUDA_STUB_API CUresult cuPointerSetAttribute(
    const void *value,
    CUpointer_attribute attribute,
    CUdeviceptr ptr) {
  CUresult result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NO_DEVICE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  result = psyche_cuda_driver_pointer_set_attribute_locked(value, attribute, ptr);
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

PSYCHE_CUDA_STUB_API CUresult cuMemAllocHost(void **pp, size_t bytesize) {
  if (pp == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_host_alloc_simulated(pp, bytesize, 0, 1);
  }
  (void)bytesize;
  *pp = 0;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemHostAlloc(
    void **pp,
    size_t bytesize,
    unsigned int Flags) {
  if (pp == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_host_alloc_simulated(
        pp,
        bytesize,
        Flags,
        (Flags & CU_MEMHOSTALLOC_DEVICEMAP) != 0);
  }
  (void)bytesize;
  (void)Flags;
  *pp = 0;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemFreeHost(void *p) {
  CUresult cleanup_result = CUDA_ERROR_INVALID_VALUE;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  cleanup_result = psyche_cuda_driver_free_host_simulated(p, CUDA_ERROR_INVALID_VALUE);
  if (cleanup_result == CUDA_SUCCESS) {
    return CUDA_SUCCESS;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return cleanup_result;
  }
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemHostRegister(
    void *p,
    size_t bytesize,
    unsigned int Flags) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_host_register_simulated(p, bytesize, Flags);
  }
  (void)p;
  (void)bytesize;
  (void)Flags;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemHostUnregister(void *p) {
  CUresult cleanup_result = CUDA_ERROR_INVALID_VALUE;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  cleanup_result =
      psyche_cuda_driver_host_unregister_simulated(p, CUDA_ERROR_HOST_MEMORY_NOT_REGISTERED);
  if (cleanup_result == CUDA_SUCCESS) {
    return CUDA_SUCCESS;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return cleanup_result;
  }
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemHostGetFlags(unsigned int *pFlags, void *p) {
  if (pFlags == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_host_get_flags_simulated(pFlags, p);
  }
  *pFlags = 0;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemHostGetDevicePointer(
    CUdeviceptr *pdptr,
    void *p,
    unsigned int Flags) {
  if (pdptr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_host_get_device_pointer_simulated(pdptr, p, Flags);
  }
  *pdptr = 0;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyHtoD(
    CUdeviceptr dstDevice,
    const void *srcHost,
    size_t ByteCount) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_htod_simulated(dstDevice, srcHost, ByteCount);
  }
  (void)dstDevice;
  (void)srcHost;
  (void)ByteCount;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyHtoD_v2(
    CUdeviceptr dstDevice,
    const void *srcHost,
    size_t ByteCount) {
  return cuMemcpyHtoD(dstDevice, srcHost, ByteCount);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyDtoH(
    void *dstHost,
    CUdeviceptr srcDevice,
    size_t ByteCount) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_dtoh_simulated(dstHost, srcDevice, ByteCount);
  }
  (void)dstHost;
  (void)srcDevice;
  (void)ByteCount;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyDtoH_v2(
    void *dstHost,
    CUdeviceptr srcDevice,
    size_t ByteCount) {
  return cuMemcpyDtoH(dstHost, srcDevice, ByteCount);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyDtoD(
    CUdeviceptr dstDevice,
    CUdeviceptr srcDevice,
    size_t ByteCount) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_dtod_simulated(dstDevice, srcDevice, ByteCount);
  }
  (void)dstDevice;
  (void)srcDevice;
  (void)ByteCount;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyDtoD_v2(
    CUdeviceptr dstDevice,
    CUdeviceptr srcDevice,
    size_t ByteCount) {
  return cuMemcpyDtoD(dstDevice, srcDevice, ByteCount);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy(
    CUdeviceptr dst,
    CUdeviceptr src,
    size_t ByteCount) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_simulated(dst, src, ByteCount);
  }
  (void)dst;
  (void)src;
  (void)ByteCount;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy_v2(
    CUdeviceptr dst,
    CUdeviceptr src,
    size_t ByteCount) {
  return cuMemcpy(dst, src, ByteCount);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy2D(const CUDA_MEMCPY2D *pCopy) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_2d_simulated(pCopy);
  }
  (void)pCopy;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy2D_v2(const CUDA_MEMCPY2D *pCopy) {
  return cuMemcpy2D(pCopy);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy2DUnaligned(const CUDA_MEMCPY2D *pCopy) {
  return cuMemcpy2D(pCopy);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy2DUnaligned_v2(const CUDA_MEMCPY2D *pCopy) {
  return cuMemcpy2DUnaligned(pCopy);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy3D(const CUDA_MEMCPY3D *pCopy) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_3d_simulated(pCopy);
  }
  (void)pCopy;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy3D_v2(const CUDA_MEMCPY3D *pCopy) {
  return cuMemcpy3D(pCopy);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyHtoDAsync(
    CUdeviceptr dstDevice,
    const void *srcHost,
    size_t ByteCount,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_htod_simulated(dstDevice, srcHost, ByteCount);
  }
  (void)dstDevice;
  (void)srcHost;
  (void)ByteCount;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyHtoDAsync_v2(
    CUdeviceptr dstDevice,
    const void *srcHost,
    size_t ByteCount,
    CUstream hStream) {
  return cuMemcpyHtoDAsync(dstDevice, srcHost, ByteCount, hStream);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyDtoHAsync(
    void *dstHost,
    CUdeviceptr srcDevice,
    size_t ByteCount,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_dtoh_simulated(dstHost, srcDevice, ByteCount);
  }
  (void)dstHost;
  (void)srcDevice;
  (void)ByteCount;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyDtoHAsync_v2(
    void *dstHost,
    CUdeviceptr srcDevice,
    size_t ByteCount,
    CUstream hStream) {
  return cuMemcpyDtoHAsync(dstHost, srcDevice, ByteCount, hStream);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyDtoDAsync(
    CUdeviceptr dstDevice,
    CUdeviceptr srcDevice,
    size_t ByteCount,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_dtod_simulated(dstDevice, srcDevice, ByteCount);
  }
  (void)dstDevice;
  (void)srcDevice;
  (void)ByteCount;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyDtoDAsync_v2(
    CUdeviceptr dstDevice,
    CUdeviceptr srcDevice,
    size_t ByteCount,
    CUstream hStream) {
  return cuMemcpyDtoDAsync(dstDevice, srcDevice, ByteCount, hStream);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyAsync(
    CUdeviceptr dst,
    CUdeviceptr src,
    size_t ByteCount,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_simulated(dst, src, ByteCount);
  }
  (void)dst;
  (void)src;
  (void)ByteCount;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpyAsync_v2(
    CUdeviceptr dst,
    CUdeviceptr src,
    size_t ByteCount,
    CUstream hStream) {
  return cuMemcpyAsync(dst, src, ByteCount, hStream);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy2DAsync(
    const CUDA_MEMCPY2D *pCopy,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_2d_simulated(pCopy);
  }
  (void)pCopy;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy2DAsync_v2(
    const CUDA_MEMCPY2D *pCopy,
    CUstream hStream) {
  return cuMemcpy2DAsync(pCopy, hStream);
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy3DAsync(
    const CUDA_MEMCPY3D *pCopy,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memcpy_3d_simulated(pCopy);
  }
  (void)pCopy;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemcpy3DAsync_v2(
    const CUDA_MEMCPY3D *pCopy,
    CUstream hStream) {
  return cuMemcpy3DAsync(pCopy, hStream);
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD8(
    CUdeviceptr dstDevice,
    unsigned char uc,
    size_t N) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_d8_simulated(dstDevice, uc, N);
  }
  (void)dstDevice;
  (void)uc;
  (void)N;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD8Async(
    CUdeviceptr dstDevice,
    unsigned char uc,
    size_t N,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_d8_simulated(dstDevice, uc, N);
  }
  (void)dstDevice;
  (void)uc;
  (void)N;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD2D8(
    CUdeviceptr dstDevice,
    size_t dstPitch,
    unsigned char uc,
    size_t Width,
    size_t Height) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_2d_pattern_simulated(
        dstDevice,
        dstPitch,
        &uc,
        sizeof(uc),
        sizeof(uc),
        Width,
        Height);
  }
  (void)dstDevice;
  (void)dstPitch;
  (void)uc;
  (void)Width;
  (void)Height;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD2D8Async(
    CUdeviceptr dstDevice,
    size_t dstPitch,
    unsigned char uc,
    size_t Width,
    size_t Height,
    CUstream hStream) {
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_2d_pattern_simulated(
        dstDevice,
        dstPitch,
        &uc,
        sizeof(uc),
        sizeof(uc),
        Width,
        Height);
  }
  (void)dstDevice;
  (void)dstPitch;
  (void)uc;
  (void)Width;
  (void)Height;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD16(
    CUdeviceptr dstDevice,
    unsigned short us,
    size_t N) {
  uint16_t value = (uint16_t)us;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_pattern_simulated(
        dstDevice,
        &value,
        sizeof(value),
        sizeof(value),
        N);
  }
  (void)dstDevice;
  (void)value;
  (void)N;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD16Async(
    CUdeviceptr dstDevice,
    unsigned short us,
    size_t N,
    CUstream hStream) {
  uint16_t value = (uint16_t)us;
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_pattern_simulated(
        dstDevice,
        &value,
        sizeof(value),
        sizeof(value),
        N);
  }
  (void)dstDevice;
  (void)value;
  (void)N;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD2D16(
    CUdeviceptr dstDevice,
    size_t dstPitch,
    unsigned short us,
    size_t Width,
    size_t Height) {
  uint16_t value = (uint16_t)us;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_2d_pattern_simulated(
        dstDevice,
        dstPitch,
        &value,
        sizeof(value),
        sizeof(value),
        Width,
        Height);
  }
  (void)dstDevice;
  (void)dstPitch;
  (void)value;
  (void)Width;
  (void)Height;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD2D16Async(
    CUdeviceptr dstDevice,
    size_t dstPitch,
    unsigned short us,
    size_t Width,
    size_t Height,
    CUstream hStream) {
  uint16_t value = (uint16_t)us;
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_2d_pattern_simulated(
        dstDevice,
        dstPitch,
        &value,
        sizeof(value),
        sizeof(value),
        Width,
        Height);
  }
  (void)dstDevice;
  (void)dstPitch;
  (void)value;
  (void)Width;
  (void)Height;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD32(
    CUdeviceptr dstDevice,
    unsigned int ui,
    size_t N) {
  uint32_t value = (uint32_t)ui;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_pattern_simulated(
        dstDevice,
        &value,
        sizeof(value),
        sizeof(value),
        N);
  }
  (void)dstDevice;
  (void)value;
  (void)N;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD32Async(
    CUdeviceptr dstDevice,
    unsigned int ui,
    size_t N,
    CUstream hStream) {
  uint32_t value = (uint32_t)ui;
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_pattern_simulated(
        dstDevice,
        &value,
        sizeof(value),
        sizeof(value),
        N);
  }
  (void)dstDevice;
  (void)value;
  (void)N;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD2D32(
    CUdeviceptr dstDevice,
    size_t dstPitch,
    unsigned int ui,
    size_t Width,
    size_t Height) {
  uint32_t value = (uint32_t)ui;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_2d_pattern_simulated(
        dstDevice,
        dstPitch,
        &value,
        sizeof(value),
        sizeof(value),
        Width,
        Height);
  }
  (void)dstDevice;
  (void)dstPitch;
  (void)value;
  (void)Width;
  (void)Height;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuMemsetD2D32Async(
    CUdeviceptr dstDevice,
    size_t dstPitch,
    unsigned int ui,
    size_t Width,
    size_t Height,
    CUstream hStream) {
  uint32_t value = (uint32_t)ui;
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  stream_result = psyche_cuda_driver_validate_async_stream(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (psyche_cuda_driver_simulated_memory_enabled()) {
    return psyche_cuda_driver_memset_2d_pattern_simulated(
        dstDevice,
        dstPitch,
        &value,
        sizeof(value),
        sizeof(value),
        Width,
        Height);
  }
  (void)dstDevice;
  (void)dstPitch;
  (void)value;
  (void)Width;
  (void)Height;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuStreamCreate(CUstream *phStream, unsigned int flags) {
  if (phStream == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_sync_enabled()) {
    return psyche_cuda_driver_create_stream_simulated(phStream, flags, 0);
  }
  *phStream = 0;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuStreamDestroy(CUstream hStream) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_sync_enabled()) {
    return psyche_cuda_driver_destroy_stream_simulated(hStream);
  }
  (void)hStream;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuStreamDestroy_v2(CUstream hStream) {
  return cuStreamDestroy(hStream);
}

PSYCHE_CUDA_STUB_API CUresult cuStreamSynchronize(CUstream hStream) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_sync_enabled()) {
    return psyche_cuda_driver_validate_stream_simulated(hStream);
  }
  (void)hStream;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuStreamQuery(CUstream hStream) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_sync_enabled()) {
    return psyche_cuda_driver_validate_stream_simulated(hStream);
  }
  (void)hStream;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuEventCreate(CUevent *phEvent, unsigned int flags) {
  if (phEvent == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_sync_enabled()) {
    return psyche_cuda_driver_create_event_simulated(phEvent, flags);
  }
  *phEvent = 0;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuEventDestroy(CUevent hEvent) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_sync_enabled()) {
    return psyche_cuda_driver_destroy_event_simulated(hEvent);
  }
  (void)hEvent;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuEventDestroy_v2(CUevent hEvent) {
  return cuEventDestroy(hEvent);
}

PSYCHE_CUDA_STUB_API CUresult cuEventRecord(CUevent hEvent, CUstream hStream) {
  PsycheCudaDriverEvent *event = 0;
  CUresult stream_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_sync_enabled()) {
    (void)hEvent;
    (void)hStream;
    return CUDA_ERROR_NO_DEVICE;
  }
  stream_result = psyche_cuda_driver_validate_stream_simulated(hStream);
  if (stream_result != CUDA_SUCCESS) {
    return stream_result;
  }
  if (hEvent == 0) {
    return CUDA_ERROR_INVALID_HANDLE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  event = psyche_cuda_driver_find_event_locked(hEvent);
  if (event == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_HANDLE;
  }
  psyche_cuda_driver_now(&event->recorded_at);
  event->recorded = 1;
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuEventQuery(CUevent hEvent) {
  PsycheCudaDriverEvent *event = 0;
  CUresult result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_sync_enabled()) {
    (void)hEvent;
    return CUDA_ERROR_NO_DEVICE;
  }
  if (hEvent == 0) {
    return CUDA_ERROR_INVALID_HANDLE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  event = psyche_cuda_driver_find_event_locked(hEvent);
  if (event == 0) {
    result = CUDA_ERROR_INVALID_HANDLE;
  } else if (!event->recorded) {
    result = CUDA_ERROR_NOT_READY;
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

PSYCHE_CUDA_STUB_API CUresult cuEventElapsedTime(
    float *pMilliseconds,
    CUevent hStart,
    CUevent hEnd) {
  PsycheCudaDriverEvent *start = 0;
  PsycheCudaDriverEvent *end = 0;
  CUresult result = CUDA_SUCCESS;
  if (pMilliseconds == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *pMilliseconds = 0.0f;
  if (!psyche_cuda_driver_simulated_sync_enabled()) {
    (void)hStart;
    (void)hEnd;
    return CUDA_ERROR_NO_DEVICE;
  }
  if (hStart == 0 || hEnd == 0) {
    return CUDA_ERROR_INVALID_HANDLE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  start = psyche_cuda_driver_find_event_locked(hStart);
  end = psyche_cuda_driver_find_event_locked(hEnd);
  if (
      start == 0 ||
      end == 0 ||
      !start->recorded ||
      !end->recorded ||
      (start->flags & CU_EVENT_DISABLE_TIMING) != 0 ||
      (end->flags & CU_EVENT_DISABLE_TIMING) != 0) {
    result = CUDA_ERROR_INVALID_HANDLE;
  } else {
    *pMilliseconds = psyche_cuda_driver_elapsed_ms(&start->recorded_at, &end->recorded_at);
    if (*pMilliseconds < 0.0f) {
      *pMilliseconds = 0.0f;
    }
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

PSYCHE_CUDA_STUB_API CUresult cuEventSynchronize(CUevent hEvent) {
  CUresult query_result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_sync_enabled()) {
    (void)hEvent;
    return CUDA_ERROR_NO_DEVICE;
  }
  query_result = cuEventQuery(hEvent);
  if (query_result == CUDA_ERROR_NOT_READY) {
    return CUDA_ERROR_NOT_READY;
  }
  return query_result;
}

PSYCHE_CUDA_STUB_API CUresult cuEventRecordWithFlags(
    CUevent hEvent,
    CUstream hStream,
    unsigned int flags) {
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (flags != 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  return cuEventRecord(hEvent, hStream);
}

PSYCHE_CUDA_STUB_API CUresult cuStreamGetFlags(CUstream hStream, unsigned int *flags) {
  PsycheCudaDriverStream *stream = 0;
  CUresult result = CUDA_SUCCESS;
  if (flags == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *flags = 0;
  if (!psyche_cuda_driver_simulated_sync_enabled()) {
    (void)hStream;
    return CUDA_ERROR_NO_DEVICE;
  }
  if (hStream == 0) {
    return CUDA_SUCCESS;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  stream = psyche_cuda_driver_find_stream_locked(hStream);
  if (stream == 0) {
    result = CUDA_ERROR_INVALID_HANDLE;
  } else {
    *flags = stream->flags;
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  return result;
}

PSYCHE_CUDA_STUB_API CUresult cuStreamCreateWithPriority(
    CUstream *phStream,
    unsigned int flags,
    int priority) {
  if (phStream == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (psyche_cuda_driver_simulated_sync_enabled()) {
    return psyche_cuda_driver_create_stream_simulated(phStream, flags, priority);
  }
  *phStream = 0;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuStreamGetPriority(CUstream hStream, int *priority) {
  CUresult result = CUDA_SUCCESS;
  if (priority == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *priority = 0;
  if (!psyche_cuda_driver_simulated_sync_enabled()) {
    (void)hStream;
    return CUDA_ERROR_NO_DEVICE;
  }
  result = psyche_cuda_driver_validate_stream_simulated(hStream);
  if (result != CUDA_SUCCESS) {
    return result;
  }
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuCtxGetStreamPriorityRange(
    int *leastPriority,
    int *greatestPriority) {
  if (leastPriority == 0 || greatestPriority == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *leastPriority = 0;
  *greatestPriority = 0;
  return CUDA_ERROR_NO_DEVICE;
}

PSYCHE_CUDA_STUB_API CUresult cuModuleLoad(CUmodule *module, const char *fname) {
  (void)fname;
  if (module == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  *module = 0;
  return CUDA_ERROR_NOT_SUPPORTED;
}

PSYCHE_CUDA_STUB_API CUresult cuModuleLoadData(CUmodule *module, const void *image) {
  if (module == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    *module = 0;
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  return psyche_cuda_driver_create_psyche_module_from_blob(module, (const char *)image);
}

PSYCHE_CUDA_STUB_API CUresult cuModuleGetFunction(
    CUfunction *hfunc,
    CUmodule hmod,
    const char *name) {
  PsycheCudaDriverModule *module = 0;
  if (hfunc == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (hmod == 0 || name == 0) {
    *hfunc = 0;
    return CUDA_ERROR_INVALID_VALUE;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  module = psyche_cuda_driver_find_module_locked(hmod);
  if (module == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    *hfunc = 0;
    return CUDA_ERROR_INVALID_HANDLE;
  }
  {
    unsigned int i = 0;
    for (i = 0; i < PSYCHE_CUDA_DRIVER_MODULE_FUNCTION_COUNT; i++) {
      PsycheCudaDriverFunction *candidate = psyche_cuda_driver_module_function_at(module, i);
      if (
          candidate != 0 &&
          candidate->magic == PSYCHE_CUDA_FUNCTION_MAGIC &&
          strcmp(name, candidate->name) == 0) {
        *hfunc = candidate->handle;
        pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
        return CUDA_SUCCESS;
      }
    }
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  *hfunc = 0;
  return CUDA_ERROR_NOT_SUPPORTED;
}

PSYCHE_CUDA_STUB_API CUresult cuLaunchKernel(
    CUfunction f,
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    unsigned int sharedMemBytes,
    void *hStream,
    void **kernelParams,
    void **extra) {
  PsycheCudaDriverFunction *function = 0;
  PsycheCudaDriverKernelLaunchFn launch = 0;
  CUresult result = CUDA_SUCCESS;
  if (!atomic_load(&psyche_cuda_driver_initialized)) {
    return CUDA_ERROR_NOT_INITIALIZED;
  }
  if (f == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (!psyche_cuda_driver_simulated_memory_enabled()) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  if (sharedMemBytes != 0 || extra != 0) {
    return CUDA_ERROR_NOT_SUPPORTED;
  }
  result = psyche_cuda_driver_validate_stream_simulated((CUstream)hStream);
  if (result != CUDA_SUCCESS) {
    return result;
  }
  pthread_mutex_lock(&psyche_cuda_driver_allocation_mutex);
  function = psyche_cuda_driver_find_function_locked(f);
  if (function == 0) {
    pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
    return CUDA_ERROR_INVALID_HANDLE;
  }
  if (function->descriptor != 0) {
    launch = function->descriptor->launch;
  }
  pthread_mutex_unlock(&psyche_cuda_driver_allocation_mutex);
  if (launch != 0) {
    return launch(
        gridDimX,
        gridDimY,
        gridDimZ,
        blockDimX,
        blockDimY,
        blockDimZ,
        kernelParams);
  }
  return CUDA_ERROR_NOT_SUPPORTED;
}

PSYCHE_CUDA_STUB_API CUresult cuGetProcAddress(
    const char *symbol,
    void **pfn,
    int cudaVersion,
    cuuint64_t flags,
    CUdriverProcAddressQueryResult *symbolStatus) {
  (void)cudaVersion;
  (void)flags;
  struct Symbol {
    const char *name;
    void *address;
  };
  /*
   * Direct symbols for module loading and kernel launch are exported so linkers
   * can resolve CUDA binaries, but cuGetProcAddress keeps execution symbols
   * unavailable until a real Metal-backed runtime exists.
   */
  static const struct Symbol symbols[] = {
      {"cuInit", (void *)&cuInit},
      {"cuDriverGetVersion", (void *)&cuDriverGetVersion},
      {"cuDeviceGetCount", (void *)&cuDeviceGetCount},
      {"cuDeviceGet", (void *)&cuDeviceGet},
      {"cuDeviceGetName", (void *)&cuDeviceGetName},
      {"cuDeviceTotalMem", (void *)&cuDeviceTotalMem},
      {"cuDeviceGetAttribute", (void *)&cuDeviceGetAttribute},
      {"cuCtxGetCurrent", (void *)&cuCtxGetCurrent},
      {"cuMemGetInfo", (void *)&cuMemGetInfo},
      {"cuGetProcAddress", (void *)&cuGetProcAddress},
      {"cuGetErrorName", (void *)&cuGetErrorName},
      {"cuGetErrorString", (void *)&cuGetErrorString},
      {"psyche_cuda_compat_stub_version", (void *)&psyche_cuda_compat_stub_version},
      {"psyche_cuda_compat_stub_is_stub", (void *)&psyche_cuda_compat_stub_is_stub},
  };
  if (pfn == 0) {
    if (symbolStatus != 0) {
      *symbolStatus = CU_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
    }
    return CUDA_ERROR_INVALID_VALUE;
  }
  if (symbol == 0) {
    *pfn = 0;
    if (symbolStatus != 0) {
      *symbolStatus = CU_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
    }
    return CUDA_ERROR_INVALID_VALUE;
  }
  for (size_t index = 0; index < sizeof(symbols) / sizeof(symbols[0]); index++) {
    if (strcmp(symbol, symbols[index].name) == 0) {
      *pfn = symbols[index].address;
      if (symbolStatus != 0) {
        *symbolStatus = CU_GET_PROC_ADDRESS_SUCCESS;
      }
      return CUDA_SUCCESS;
    }
  }
  *pfn = 0;
  if (symbolStatus != 0) {
    *symbolStatus = CU_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
  }
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuGetErrorName(CUresult error, const char **pStr) {
  if (pStr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  *pStr = psyche_cuda_stub_error_name(error);
  return CUDA_SUCCESS;
}

PSYCHE_CUDA_STUB_API CUresult cuGetErrorString(CUresult error, const char **pStr) {
  if (pStr == 0) {
    return CUDA_ERROR_INVALID_VALUE;
  }
  *pStr = psyche_cuda_stub_error_string(error);
  return CUDA_SUCCESS;
}
