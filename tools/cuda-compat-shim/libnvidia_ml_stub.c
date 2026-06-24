#include "cuda_compat_stub.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

static _Atomic int psyche_nvml_refcount = 0;

static const char *const PSYCHE_NVML_DRIVER_VERSION = "000.00.00-psyche-stub";
static const char *const PSYCHE_NVML_LIBRARY_VERSION = "0.0.0-psyche-stub";

nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int *deviceCount);
nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t *device);
nvmlReturn_t nvmlDeviceGetHandleByPciBusId_v2(
    const char *pciBusId,
    nvmlDevice_t *device);

static int psyche_nvml_is_initialized(void) {
  return atomic_load_explicit(&psyche_nvml_refcount, memory_order_acquire) > 0;
}

static nvmlReturn_t psyche_nvml_increment_refcount(void) {
  atomic_fetch_add_explicit(&psyche_nvml_refcount, 1, memory_order_acq_rel);
  return NVML_SUCCESS;
}

static nvmlReturn_t psyche_nvml_decrement_refcount(void) {
  int current = atomic_load_explicit(&psyche_nvml_refcount, memory_order_acquire);
  while (current > 0) {
    if (atomic_compare_exchange_weak_explicit(
            &psyche_nvml_refcount,
            &current,
            current - 1,
            memory_order_acq_rel,
            memory_order_acquire)) {
      return NVML_SUCCESS;
    }
  }
  return NVML_ERROR_UNINITIALIZED;
}

static nvmlReturn_t psyche_nvml_copy_string(
    char *dst,
    unsigned int length,
    const char *src) {
  size_t required;
  if (dst == 0) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  required = strlen(src) + 1;
  if ((size_t)length < required) {
    return NVML_ERROR_INSUFFICIENT_SIZE;
  }
  memcpy(dst, src, required);
  return NVML_SUCCESS;
}

static nvmlReturn_t psyche_nvml_check_initialized(void) {
  return psyche_nvml_is_initialized() ? NVML_SUCCESS : NVML_ERROR_UNINITIALIZED;
}

static nvmlReturn_t psyche_nvml_check_device_output(
    nvmlDevice_t device,
    const void *output) {
  nvmlReturn_t status = psyche_nvml_check_initialized();
  if (status != NVML_SUCCESS) {
    return status;
  }
  if (device == 0 || output == 0) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  return NVML_ERROR_INVALID_ARGUMENT;
}

PSYCHE_CUDA_STUB_API int psyche_cuda_compat_stub_is_stub(void) {
  return 1;
}

PSYCHE_CUDA_STUB_API const char *psyche_cuda_compat_stub_version(void) {
  return "psyche-nvml-compat-stub/0.1";
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlInit(void) {
  return psyche_nvml_increment_refcount();
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlInit_v2(void) {
  return psyche_nvml_increment_refcount();
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlInitWithFlags(unsigned int flags) {
  /*
   * No NVIDIA driver or device attach happens in this shim, so known and future
   * init flags are intentionally no-op metadata.
   */
  (void)flags;
  return psyche_nvml_increment_refcount();
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlShutdown(void) {
  return psyche_nvml_decrement_refcount();
}

PSYCHE_CUDA_STUB_API const char *nvmlErrorString(nvmlReturn_t result) {
  switch (result) {
  case NVML_SUCCESS:
    return "Success";
  case NVML_ERROR_UNINITIALIZED:
    return "Uninitialized";
  case NVML_ERROR_INVALID_ARGUMENT:
    return "Invalid Argument";
  case NVML_ERROR_NOT_SUPPORTED:
    return "Not Supported";
  case NVML_ERROR_NO_PERMISSION:
    return "Insufficient Permissions";
  case NVML_ERROR_ALREADY_INITIALIZED:
    return "Already Initialized";
  case NVML_ERROR_NOT_FOUND:
    return "Not Found";
  case NVML_ERROR_INSUFFICIENT_SIZE:
    return "Insufficient Size";
  case NVML_ERROR_INSUFFICIENT_POWER:
    return "Insufficient External Power";
  case NVML_ERROR_DRIVER_NOT_LOADED:
    return "Driver Not Loaded";
  case NVML_ERROR_TIMEOUT:
    return "Timeout";
  case NVML_ERROR_IRQ_ISSUE:
    return "IRQ Issue";
  case NVML_ERROR_LIBRARY_NOT_FOUND:
    return "Library Not Found";
  case NVML_ERROR_FUNCTION_NOT_FOUND:
    return "Function Not Found";
  case NVML_ERROR_CORRUPTED_INFOROM:
    return "Corrupted InfoROM";
  case NVML_ERROR_GPU_IS_LOST:
    return "GPU Is Lost";
  case NVML_ERROR_RESET_REQUIRED:
    return "Reset Required";
  case NVML_ERROR_OPERATING_SYSTEM:
    return "Operating System Error";
  case NVML_ERROR_LIB_RM_VERSION_MISMATCH:
    return "Driver/library Version Mismatch";
  case NVML_ERROR_IN_USE:
    return "In Use";
  case NVML_ERROR_MEMORY:
    return "Memory Error";
  case NVML_ERROR_NO_DATA:
    return "No Data";
  case NVML_ERROR_VGPU_ECC_NOT_SUPPORTED:
    return "vGPU ECC Not Supported";
  case NVML_ERROR_INSUFFICIENT_RESOURCES:
    return "Insufficient Resources";
  case NVML_ERROR_FREQ_NOT_SUPPORTED:
    return "Frequency Not Supported";
  case NVML_ERROR_ARGUMENT_VERSION_MISMATCH:
    return "Argument Version Mismatch";
  case NVML_ERROR_DEPRECATED:
    return "Deprecated";
  case NVML_ERROR_NOT_READY:
    return "Not Ready";
  case NVML_ERROR_GPU_NOT_FOUND:
    return "GPU Not Found";
  case NVML_ERROR_INVALID_STATE:
    return "Invalid State";
  case NVML_ERROR_UNKNOWN:
  default:
    return "Unknown Error";
  }
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlSystemGetDriverVersion(
    char *version,
    unsigned int length) {
  nvmlReturn_t status = psyche_nvml_check_initialized();
  if (status != NVML_SUCCESS) {
    return status;
  }
  return psyche_nvml_copy_string(version, length, PSYCHE_NVML_DRIVER_VERSION);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlSystemGetNVMLVersion(
    char *version,
    unsigned int length) {
  return psyche_nvml_copy_string(version, length, PSYCHE_NVML_LIBRARY_VERSION);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlSystemGetCudaDriverVersion(
    int *cudaDriverVersion) {
  if (cudaDriverVersion == 0) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  /*
   * Match the libcuda/libcudart discovery contract: version queries succeed but
   * report 0 so callers can cross-check that no CUDA driver is present.
   */
  *cudaDriverVersion = 0;
  return NVML_SUCCESS;
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlSystemGetCudaDriverVersion_v2(
    int *cudaDriverVersion) {
  return nvmlSystemGetCudaDriverVersion(cudaDriverVersion);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetCount(
    unsigned int *deviceCount) {
  return nvmlDeviceGetCount_v2(deviceCount);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetCount_v2(
    unsigned int *deviceCount) {
  nvmlReturn_t status = psyche_nvml_check_initialized();
  if (status != NVML_SUCCESS) {
    return status;
  }
  if (deviceCount == 0) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  *deviceCount = 0;
  return NVML_SUCCESS;
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetHandleByIndex(
    unsigned int index,
    nvmlDevice_t *device) {
  return nvmlDeviceGetHandleByIndex_v2(index, device);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(
    unsigned int index,
    nvmlDevice_t *device) {
  nvmlReturn_t status = psyche_nvml_check_initialized();
  (void)index;
  if (status != NVML_SUCCESS) {
    return status;
  }
  if (device == 0) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  return NVML_ERROR_INVALID_ARGUMENT;
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetHandleByUUID(
    const char *uuid,
    nvmlDevice_t *device) {
  nvmlReturn_t status = psyche_nvml_check_initialized();
  if (status != NVML_SUCCESS) {
    return status;
  }
  if (uuid == 0 || device == 0 || uuid[0] == '\0') {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  return NVML_ERROR_NOT_FOUND;
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetHandleByPciBusId(
    const char *pciBusId,
    nvmlDevice_t *device) {
  return nvmlDeviceGetHandleByPciBusId_v2(pciBusId, device);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetHandleByPciBusId_v2(
    const char *pciBusId,
    nvmlDevice_t *device) {
  nvmlReturn_t status = psyche_nvml_check_initialized();
  if (status != NVML_SUCCESS) {
    return status;
  }
  if (pciBusId == 0 || device == 0 || pciBusId[0] == '\0') {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  return NVML_ERROR_NOT_FOUND;
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetName(
    nvmlDevice_t device,
    char *name,
    unsigned int length) {
  (void)length;
  return psyche_nvml_check_device_output(device, name);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetUUID(
    nvmlDevice_t device,
    char *uuid,
    unsigned int length) {
  (void)length;
  return psyche_nvml_check_device_output(device, uuid);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetMemoryInfo(
    nvmlDevice_t device,
    nvmlMemory_t *memory) {
  return psyche_nvml_check_device_output(device, memory);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetUtilizationRates(
    nvmlDevice_t device,
    nvmlUtilization_t *utilization) {
  return psyche_nvml_check_device_output(device, utilization);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetTemperature(
    nvmlDevice_t device,
    unsigned int sensorType,
    unsigned int *temp) {
  (void)sensorType;
  return psyche_nvml_check_device_output(device, temp);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetPowerUsage(
    nvmlDevice_t device,
    unsigned int *power) {
  return psyche_nvml_check_device_output(device, power);
}

PSYCHE_CUDA_STUB_API nvmlReturn_t nvmlDeviceGetCudaComputeCapability(
    nvmlDevice_t device,
    int *major,
    int *minor) {
  nvmlReturn_t status = psyche_nvml_check_initialized();
  if (status != NVML_SUCCESS) {
    return status;
  }
  if (device == 0 || major == 0 || minor == 0) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  return NVML_ERROR_INVALID_ARGUMENT;
}
