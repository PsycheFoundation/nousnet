# Psyche CUDA Compatibility Stubs

This directory contains deliberately small `libcuda`, `libcudart`,
`libcublas`, `libcublasLt`, `libcusparse`, `libcusolver`,
`libnvidia-ml`, and `libcudnn` shims for Apple Silicon development on macOS.

The shim is not a CUDA implementation. It exists to make CUDA discovery and
linkage fail honestly on machines where the real compute path is MPS, Metal,
or MLX:

- driver/runtime version queries return `0`;
- driver initialization reports `CUDA_ERROR_NO_DEVICE` instead of pretending a
  working CUDA driver exists;
- device-count queries report zero CUDA-capable devices;
- memory-accounting queries may return success with `0` free and `0` total
  bytes, which is a no-device discovery signal, not a usable allocation path;
- `libnvidia-ml` initializes as a compatibility library, reports zero NVIDIA
  devices through `nvmlDeviceGetCount[_v2]`, returns parseable stub driver/NVML
  versions, exposes `nvmlErrorString`, and rejects handle lookup or device
  telemetry without synthesizing Apple GPU identity, clocks, power, process
  accounting, utilization, or memory as NVIDIA telemetry;
- `libcudnn` exposes version/property/error-string discovery helpers as
  truthful zero/stub values. By default, `cudnnCreate` clears the output handle
  and returns not-initialized because there is no compatible NVIDIA CUDA
  backend. Under `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1`, it can create a
  metadata handle plus tensor, activation, and pooling descriptors for bounded
  simulated cuDNN operation subsets. `cudnnActivationForward` and
	  `cudnnActivationBackward` support only contiguous 4D NCHW FP32
	  ReLU/Sigmoid/Tanh/Identity with host alpha/beta scalars and value-identical
	  tensor descriptors. Activation coefficients are ignored for those supported
	  modes, matching cuDNN's descriptor semantics. Forward supports exact `x == y`;
	  backward validates `y` but
  computes derivatives from `x`, supports exact `dy == dx`, avoids reading old
  dx when beta is zero, sanitizes NaN x to zero under `CUDNN_NOT_PROPAGATE_NAN`,
  lets upstream dy NaNs propagate, and rejects backward x/dx or y/dx overlap.
  The backward path is a bounded compatibility bridge, not bitwise cuDNN parity
  for implementations that derive sigmoid/tanh gradients from saved raw
  activation output y. `cudnnAddTensor` supports same-shape and broadcasted
  FP32 dense 4D NCHW adds, including the common `1xCx1x1` bias into
  `NxCxHxW`: `C = alpha * A_broadcast + beta * prior_C`. The AddTensor path
  rejects any A/C overlap, avoids reading prior C when beta is zero, still
  evaluates `alpha * A` so alpha-zero NaN source values propagate, and uses the
  same required/preferred Metal route. 5D tensors, NHWC, non-FP32,
  non-contiguous/custom strides, and aliased A/C storage remain unsupported.
  `cudnnBatchNormalizationForwardInference` supports the deprecated cuDNN
  legacy inference API for contiguous 4D NCHW FP32 tensors with matching x/y
  descriptors, `CUDNN_BATCHNORM_SPATIAL` parameters shaped `1xCx1x1`, and
  `CUDNN_BATCHNORM_PER_ACTIVATION` parameters shaped `1xCxHxW`. It computes
  `y = beta * prior_y + alpha * (bias + scale * (x - mean) / sqrt(epsilon + variance))`,
  rejects epsilon below `CUDNN_BN_MIN_EPSILON`, avoids reading prior y when beta
  is zero, allows exact `x == y` in-place inference, rejects partial x/y overlap
  and parameter/stat buffer overlap, leaves negative estimated variance to
  produce the formula's natural NaN-domain result, and uses the same
  required/preferred Metal route. Training forward, backward,
  `CUDNN_BATCHNORM_SPATIAL_PERSISTENT`, 5D, NHWC, non-FP32, and custom strides
  remain unsupported.
  `cudnnConvolutionForward` supports a bounded legacy 2D convolution-forward
  subset with cuDNN filter and convolution descriptors: contiguous 4D NCHW FP32
  x/y tensors, contiguous FP32 NCHW/KCRS filters using cuDNN grouped semantics
  (`groupCount > 0`, full x/y descriptors, filter `C = input_C/groupCount`,
  `input_C % groupCount == 0`, and `K % groupCount == 0`),
  `CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM`, `CUDNN_CROSS_CORRELATION`, and
  `CUDNN_CONVOLUTION` with spatial R/S filter flipping. Depthwise and
  depthwise-multiplier cases are covered when they satisfy the same grouped
  descriptor rules. It uses cuDNN's 2D padding/stride/dilation output formula.
  `cudnnGetConvolutionForwardAlgorithm`,
  `cudnnGetConvolutionForwardAlgorithm_v7`,
  `cudnnFindConvolutionForwardAlgorithm`,
  `cudnnGetConvolutionForwardAlgorithmMaxCount`, and
  `cudnnGetConvolutionForwardWorkspaceSize` validate the same bounded descriptor
  configuration, report exactly one deterministic zero-workspace IMPLICIT_GEMM
  algorithm, and do not claim alternates. `cudnnConvolutionForward` accepts but
  ignores workspace for that supported algorithm, rejects unsupported algorithms
  and any x/y/w byte-range overlap, avoids reading prior y when beta is zero,
  follows the shim's literal `alpha * result + beta * prior_y` formula so
  alpha-zero source/filter NaNs can propagate, and uses the same
  required/preferred Metal route for the same FP32 contiguous 4D NCHW/KCRS
  grouped subset; non-contiguous tensors, FP16, and non-NCHW layouts remain
  unsupported rather than being silently reshaped. When
  `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required` is also set, the Metal route
  tries an MPSGraph `convolution2D` fast path for the same NCHW/OIHW grouped
  FP32 subset. A small MSL prepare kernel copies cross-correlation weights or
  flips only the logical R/S axes for true convolution, leaving dilation to the
  MPSGraph descriptor; MPSGraph writes raw y into an internal shared buffer, and
  an MSL epilogue applies alpha/beta with beta-zero no-read behavior before CPU
	  copy-back. `required` mode leaves caller y unchanged on prepare, MPSGraph, or
	  epilogue failure; preferred mode falls back to the deterministic MSL path.
	  `cudnnConvolutionBiasActivationForward` supports a bounded legacy fused
	  forward subset for the same contiguous 4D NCHW FP32 x/z/y tensors and
	  contiguous FP32 KCRS filters, strict FP32 `1xKx1x1` bias descriptors,
	  value-identical z/y descriptors, ReLU with `IMPLICIT_GEMM`, and identity with
	  `IMPLICIT_PRECOMP_GEMM`. It computes
	  `y = act(alpha1 * conv_or_correlation(x, w) + alpha2 * z + bias[k])`,
	  supports exact `z == y`, rejects partial z/y overlap and broader
	  x/w/z/bias/y overlap, avoids reading z when alpha2 is zero, accepts but
	  ignores workspace, and defaults to CPU reference execution. `required`
	  cuDNN Metal mode verifies a real fused MSL route that stages x, w, z, and
	  bias into shared buffers, writes into a separate output buffer, copies y back
	  only after command-buffer completion, and leaves y unchanged on backend
	  failure; preferred Metal mode falls back only for backend-availability
	  failures. With `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required`, the Metal
	  route can prefer an MPSGraph `convolution2D` raw-convolution fast path plus
	  an MSL epilogue for alpha1, alpha2/z, bias, and ReLU/identity activation;
	  required-mode prepare, graph, or epilogue failures leave y unchanged, while
	  preferred mode falls back to deterministic fused MSL. No fused MLX route is
	  claimed yet.
	  `cudnnConvolutionBackwardData`
  supports the matching bounded legacy 2D data-gradient subset for contiguous
  4D NCHW FP32 dy/dx tensors and contiguous FP32 NCHW/KCRS filters using the
  same grouped descriptor rules, including depthwise and depthwise-multiplier
  cases. It supports only `CUDNN_CONVOLUTION_BWD_DATA_ALGO_1` for the FP32 NCHW
  path, reports exactly one deterministic zero-workspace backward-data
  algorithm through the legacy query helpers, returns zero workspace for that
  path, rejects `ALGO_0`/FFT/Winograd alternates, and computes
  `dx = alpha * dconv_data(w, dy) + beta * prior_dx` while avoiding prior-dx
  reads when beta is zero. The CPU and Metal routes use the same FP32 contiguous
  4D NCHW/KCRS grouped subset and copy dx back only after successful Metal
  completion in required/preferred Metal mode. With
  `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required`, that same Metal route can
  prefer an MPSGraph `convolution2DDataGradient` fast path. It reuses the MSL
  weight-prepare kernel described above, lets MPSGraph write raw dx internally,
  and uses the MSL alpha/beta epilogue before copy-back; required mode leaves
  caller dx unchanged on prepare, MPSGraph, or epilogue failure, while preferred
  mode falls back to deterministic MSL. `cudnnConvolutionBackwardFilter`
  supports the matching bounded legacy 2D filter-gradient subset for contiguous
  4D NCHW FP32 x/dy tensors and contiguous FP32 NCHW/KCRS dw filters using the
  same grouped descriptor rules, including depthwise and depthwise-multiplier
  cases. It supports only `CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1` for the FP32
  NCHW path, reports exactly one deterministic zero-workspace backward-filter
  algorithm through the legacy query helpers, returns zero workspace for that
  path, rejects `ALGO_0`/`ALGO_3`/FFT/Winograd alternates, and computes
  `dw = alpha * dconv_filter(x, dy) + beta * prior_dw` while avoiding prior-dw
  reads when beta is zero. For true convolution, the physical KCRS dw write slot
  is unchanged while the sampled input tap is spatially flipped to match the
  forward/backward-data convention. When
  `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required` is also set, the Metal route
  tries an MPSGraph `convolution2DWeightsGradient` fast path for the same
  NCHW/OIHW grouped FP32 subset. MPSGraph writes raw dW into an internal shared
  buffer, then a small MSL epilogue applies alpha/beta, beta-zero no-read
  behavior, and the true-convolution spatial R/S flip into a second internal
  buffer before CPU copy-back. `required` mode fails without modifying caller
  `dw` if MPSGraph or the epilogue fails; preferred mode falls back to the
  deterministic MSL path. The baseline Metal route keeps the deterministic
  one-thread-per-dw serial kernel as a fallback/oracle, and uses a
  private-scratch two-pass tiled reduction for larger `N*outH*outW` spans:
  fixed-order threadgroup partial sums first, fixed chunk-order final reduction
  second, then CPU `memcpy` copy-back only after successful Metal completion.
  MPSGraph numerical checks use tolerance-based comparison because Apple may
  choose a different FP32 reduction order; the MSL path remains the bitwise
  deterministic oracle. The private scratch is not caller workspace, so the
  public workspace query remains zero.
  Fused/bias convolution,
  alternate algorithms, `cudnnFindConvolutionForwardAlgorithmEx`,
  5D/Nd, NHWC, non-FP32, custom strides, broader MPSGraph layouts/dtypes and
  descriptor semantics, MLX rewrites, and full CUDA async semantics remain outside this bounded slice;
  those Apple-side rewrites are next implementation targets, not dead-end
  incompatibility claims.
  `cudnnTransformTensor` supports the scaled-copy subset
  for the same contiguous 4D NCHW FP32 descriptors: `y = alpha * x + beta * prior_y`,
  with no source/destination overlap, beta-zero no-read behavior, and the same
  required/preferred Metal route. Arbitrary-stride or layout conversion remains
  unsupported until the descriptor layer models those layouts. `cudnnPoolingForward` and
  `cudnnPoolingBackward` support contiguous 4D NCHW FP32 2D max pooling for
  `CUDNN_POOLING_MAX` and
  `CUDNN_POOLING_MAX_DETERMINISTIC`, plus average pooling for
  `CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING` and
  `CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING`, including
  `cudnnGetPooling2dDescriptor`, `cudnnGetPooling2dForwardOutputDim`, host
  alpha/beta scalars, and `CUDNN_PROPAGATE_NAN` /
  `CUDNN_NOT_PROPAGATE_NAN`. Average include-padding uses the full window
  denominator and zero-valued padding; average exclude-padding uses the
  per-output in-bounds denominator. Average backward is geometry-based and
  permits null x/y descriptors and data; max backward requires x/y descriptors
  and data, validates them, then recomputes deterministic first-max selection
  from x using the same scan and NaN policy as forward. Backward supports
  beta-zero no-read behavior and exact `dy == dx` only when descriptors match.
  The Metal backward path computes one dx element per thread and avoids FP32
  atomics for Apple-family portability, so it favors deterministic correctness
  over broad performance claims. `cudnnSoftmaxForward` and
  `cudnnSoftmaxBackward` support contiguous 4D NCHW FP32
  `CUDNN_SOFTMAX_FAST`, `CUDNN_SOFTMAX_ACCURATE`, and `CUDNN_SOFTMAX_LOG` for
  `CUDNN_SOFTMAX_MODE_CHANNEL` and `CUDNN_SOFTMAX_MODE_INSTANCE`, with host
  alpha/beta scalars, deterministic whole-vector NaN propagation, stable
  forward positive-infinity handling for ACCURATE/LOG, exact `x == y` forward
  staging, exact `dy == dx` backward staging, and partial-overlap rejection.
  Backward uses `y * (dy - sum(y * dy))` for FAST/ACCURATE and
  `dy - exp(y) * sum(dy)` for LOG. The
  default operation path is a CPU reference loop; on Apple Silicon,
  `PSYCHE_CUDA_COMPAT_CUDNN_METAL=required` verifies a real Metal shared-buffer
  dispatch for those same subsets, while `PSYCHE_CUDA_COMPAT_CUDNN_METAL=1`
  prefers Metal and falls back only for backend-availability failures. The
  Metal path stages inputs and prior outputs separately, computes into a
  separate output buffer, and copies back only after command completion.
  Partial transform x/y overlap, partial x/y forward overlap, partial dy/dx backward overlap,
  activation-backward x/dx or y/dx overlap, max-backward x/dx or y/dx overlap,
	  NHWC, non-FP32, custom strides, arbitrary tensor layout conversion,
	  clipped ReLU, ELU, swish, 5D softmax,
	  broader convolution, normalization, graph, and general cuDNN kernel execution remain
  unsupported;
- `cudaMemcpy(..., cudaMemcpyHostToHost)`,
  `cudaMemcpy2D(..., cudaMemcpyHostToHost)`, and linear
  `cudaMemcpy3D(..., cudaMemcpyHostToHost)` perform real host memory copies
  because they do not require a CUDA-capable device. Linear copies use
  `memmove`, 2D copies copy bounded rows, and 3D copies stage through a
  temporary CPU buffer so overlapping volumes are well-defined;
- by default, `cudaMalloc`, `cudaMallocPitch`, `cudaFree`, `cudaMemcpy`
  host-device/device-host/device-device/default-UVA directions, `cudaMemcpy2D`
  device directions, `cudaMemcpy3D` device directions, `cudaMemcpyAsync`
  device directions, `cudaMemcpy2DAsync` device directions,
  `cudaMemcpy3DAsync` device directions, `cudaMemset`, `cudaMemset2D`,
  `cudaMemsetAsync`, and `cudaMemset2DAsync` still report no CUDA device;
- setting `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY` to `1`, `true`, `yes`, or `on`
  opts the native shims into CPU-backed simulated allocation, async allocation,
  managed allocation, advice, prefetch, range-query, pointer-query, copy,
  2D-copy, 3D-copy, fill, pinned-host-memory probes, and host/no-location
  memory-pool metadata, plus host-side simulated stream/event synchronization.
  Values such as `0`, `false`, `no`, `off`, empty string, or
  an unset variable keep simulated memory disabled.
  `libcuda` covers `cuMemAlloc`, `cuMemAllocAsync`,
  `cuMemAllocFromPoolAsync`, `cuMemAllocManaged`, `cuMemAllocPitch`,
  `cuMemFree`, `cuMemFreeAsync`, `cuMemAdvise`, `cuMemPrefetchAsync`,
  `cuMemRangeGetAttribute`, `cuMemRangeGetAttributes`,
  `cuPointerGetAttribute`, `cuPointerGetAttributes`, `cuPointerSetAttribute`,
  `cuMemPoolCreate`, `cuMemPoolDestroy`, `cuMemPoolGetAttribute`,
  `cuMemPoolSetAttribute`, `cuMemPoolTrimTo`, `cuMemGetDefaultMemPool`,
  `cuMemGetMemPool`, `cuMemSetMemPool`, and the device-default/access/export/
  import pool APIs as honest no-device or unsupported stubs,
  `cuMemcpyHtoD`, `cuMemcpyDtoH`, `cuMemcpyDtoD`, `cuMemcpy`, `cuMemcpy2D`,
  `cuMemcpy2DUnaligned`, linear `cuMemcpy3D`, their null-stream or
  shim-created-stream `*Async` forms, `_v2` aliases for the allocation/copy
  forms commonly requested by CUDA-header consumers, `cuMemsetD8`,
  `cuMemsetD16`, `cuMemsetD32`, `cuMemsetD2D8`, `cuMemsetD2D16`,
  `cuMemsetD2D32`, and their null-stream or shim-created-stream async forms,
  plus `cuMemAllocHost`, `cuMemHostAlloc`, `cuMemFreeHost`,
  `cuMemHostRegister`, `cuMemHostUnregister`, `cuMemHostGetFlags`, and
  `cuMemHostGetDevicePointer`; `libcudart` covers `cudaMalloc`,
  `cudaMallocAsync`, `cudaMallocFromPoolAsync`, `cudaMallocManaged`,
  `cudaMallocPitch`, `cudaFree`, `cudaFreeAsync`, `cudaMemAdvise`,
  `cudaMemPrefetchAsync`, `cudaMemRangeGetAttribute`,
  `cudaMemRangeGetAttributes`, `cudaPointerGetAttributes`,
  `cudaMemPoolCreate`, `cudaMemPoolDestroy`, `cudaMemPoolGetAttribute`,
  `cudaMemPoolSetAttribute`, `cudaMemPoolTrimTo`,
  `cudaMemGetDefaultMemPool`, `cudaMemGetMemPool`, `cudaMemSetMemPool`, and the
  device-default/access/export/import pool APIs as honest no-device or
  unsupported stubs, `cudaMemcpy`,
  `cudaMemcpy2D`, linear `cudaMemcpy3D`, `cudaMemcpyAsync`,
  `cudaMemcpy2DAsync`, `cudaMemcpy3DAsync`, `cudaMemset`, `cudaMemset2D`,
  `cudaMemsetAsync`, `cudaMemset2DAsync`, `cudaMallocHost`, `cudaHostAlloc`,
  `cudaFreeHost`, `cudaHostRegister`, `cudaHostUnregister`,
  `cudaHostGetFlags`, and `cudaHostGetDevicePointer` for host-device,
  device-host, device-device, and default/UVA directions.
  This is CPU memory, not Apple GPU memory. Simulated copies, fills, host
  allocations, and host registrations are serialized by the shim allocation
  mutexes, and non-registered non-null pointers are treated as host pointers by
  convention, not proven-valid host memory. Linear simulated copies use
  `memmove`, pitched 2D copies copy bounded rows, and linear 3D copies stage
  through a temporary CPU buffer so overlapping volumes are well-defined. Async
  forms run synchronously and accept either the null/zero stream or a live
  stream created by the same shim family.
  Managed allocations are CPU-backed allocations with CUDA-shaped metadata:
  pointer attributes, read-mostly/preferred-location state, accessed-by range
  queries, and last-prefetch state are queryable. CPU/host advice and prefetch
  destinations are accepted; GPU destinations fail with invalid-device errors.
  The exported advice/prefetch APIs use fixed CUDA-shaped device-ordinal
  prototypes, and prefetch stream arguments are accepted only for the null/zero
  stream or a live stream created by the same shim family. Simulated device,
  async, pool-backed, and managed allocations are at least 256-byte aligned.
  Stream-ordered allocation/free calls complete synchronously and update
  host-side pool counters. The public pool-props structs follow the CUDA 13
  `maxSize` / `reserved[54]` / `usage` layout. Explicit pool creation is
  pinned/host-only; CUDA 13 location-default pools are exposed only for host
  pinned and managed/no-location requests. Default managed/no-location pools are
  metadata for CPU-backed managed allocations only; explicit managed pool
  creation remains unsupported because residency, migration, and page-fault
  behavior are not modeled. `cuMemPoolTrimTo` / `cudaMemPoolTrimTo` validate
  handles but do not model a retained backing-store cache: reserved bytes track
  live simulated allocations, so trim does not synthesize retained memory.
  Runtime `cudaDeviceReset` returns no-device when there is no CUDA device and
  no shim state, but succeeds when it is clearing previously-created simulated
  shim state. Device-default pool APIs still reject absent CUDA devices, and
  IPC, access-control, imported/exported pools, graph allocator ownership, GPU
  residency, managed-memory migration, page faults, and access counters are not
  modeled.
  Mapped host-pointer queries return the same CPU pointer only for records that
  were explicitly mapped by the relevant API; those aliases are accepted by the
  shim's simulated copy/fill operations. Linear 3D copies are modeled as bounded
  pitched row copies across depth; CUDA arrays, textures, surfaces, and
  unified-memory 3D operands are unsupported. Default host registrations are
  tracked but not device-mapped. `libcuda` generic `cuMemcpy` only accepts known
  simulated device allocations or mapped host records, not arbitrary numeric
  addresses. Runtime and driver simulated host ownership are intentionally
  separate; cross-family free/unregister calls fail safely instead of freeing an
  allocation owned by the other shim.
  The same opt-in enables host-side stream/event handles for discovery-oriented
  libraries that create streams, record events, query completion, or synchronize
  before deciding whether deeper CUDA work is possible. Driver support includes
  `cuStreamCreate`, `cuStreamCreateWithPriority`, `cuStreamDestroy`,
  `cuStreamQuery`, `cuStreamSynchronize`, `cuStreamGetFlags`,
  `cuStreamGetPriority`, `cuEventCreate`, `cuEventDestroy`, `cuEventRecord`,
  `cuEventRecordWithFlags`, `cuEventQuery`, `cuEventSynchronize`, and
  `cuEventElapsedTime`. Runtime support includes
  `cudaStreamCreate`, `cudaStreamCreateWithFlags`,
  `cudaStreamCreateWithPriority`, `cudaStreamDestroy`, `cudaStreamQuery`,
  `cudaStreamSynchronize`, `cudaStreamGetFlags`, `cudaStreamGetPriority`,
  `cudaEventCreate`, `cudaEventCreateWithFlags`, `cudaEventDestroy`,
  `cudaEventRecord`, `cudaEventRecordWithFlags`, `cudaEventQuery`,
  `cudaEventSynchronize`, and `cudaEventElapsedTime`.
  These handles are registry-validated metadata objects, not command queues.
  Runtime and driver handle domains are intentionally separate. Event timestamps
  use CPU monotonic time, not GPU timing. Priorities are accepted as metadata
  only and are normalized to `0`; context/device-wide synchronization and
  priority-range queries still report no CUDA device. Interprocess events are
  unsupported. Destroyed or foreign handles fail with invalid-handle errors. The
  mode does not change device count, properties, or real context creation;
- device selection still returns `CUDA_ERROR_NO_DEVICE`, `cudaErrorNoDevice`,
  or `*_NOT_SUPPORTED`;
- under `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1`, `cuModuleLoadData` accepts a
  Psyche-native `PSYCHE_CUDA_MODULE_V1` blob declaring `vector_add_f32` and/or
  `saxpy_f32` and/or `scale_f32` and/or `axpby_f32`, `cuModuleGetFunction` resolves those
  registered functions, and `cuLaunchKernel` runs the fixed 1D parameter schemas
  over simulated driver allocations. `vector_add_f32` computes
  `out[i] = a[i] + b[i]`; `saxpy_f32` computes in-place
  `y[i] = alpha * x[i] + y[i]`; `scale_f32` computes in-place
  `x[i] = alpha * x[i]`; `axpby_f32` computes in-place
  `x[i] = alpha * x[i] + beta * y[i]`. The default path is a CPU
  reference kernel; on Apple Silicon, `PSYCHE_CUDA_COMPAT_METAL_KERNELS=required`
  verifies a real Metal shared-buffer dispatch for those same registered kernels.
  `PSYCHE_CUDA_COMPAT_METAL_KERNELS=1` prefers Metal and falls back to the CPU
  reference path if the private Metal backend is unavailable. The private Metal
  backend is synchronous: it copies simulated allocation spans into Metal-owned
  shared buffers, waits for command-buffer completion, then copies mutated output
  spans back only after a completed command. Exact aliases are supported for the
  tested in-place/output cases (`saxpy_f32` `x == y`, `vector_add_f32`
  `out == a` / `out == b`, and `axpby_f32` `x == y`). Required Metal mode
  rejects partial overlaps involving a mutated span, while preferred Metal mode
  falls back to the CPU reference kernel for those overlap shapes.
  Raw PTX/CUBIN, `cuModuleLoad` files, arbitrary kernels, dynamic shared memory,
  extra launch config, multidimensional launches, and general CUDA execution
  remain unsupported;
- under the same explicit `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY` opt-in,
  `libcudart` exports `psyche_cuda_runtime_kernel_vector_add_f32`,
  `psyche_cuda_runtime_kernel_saxpy_f32`,
  `psyche_cuda_runtime_kernel_scale_f32`, and
  `psyche_cuda_runtime_kernel_axpby_f32` as function-address tokens.
  `cudaLaunchKernel` accepts only those tokens and runs the same fixed 1D
  parameter schemas over simulated runtime allocations. The runtime path is a
  CPU reference executor, accepts the null/default stream, validates non-null
  runtime-owned stream handles, requires a CUDA-shaped `void **args` array with
  the expected parameter slots, stages mutating multi-input outputs so exact and
  partial allocation overlaps use original inputs deterministically, rejects
  dynamic shared memory and multidimensional launch geometry, and keeps runtime
  allocation/stream ownership separate from `libcuda`. `scale_f32` is a
  single-buffer element-local update and intentionally stays in-place;
- no function reports an Apple GPU as a CUDA-capable device.
- under the same explicit `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY` opt-in,
  `libcublas` can create metadata handles; run bounded FP32/FP64 real Level-1
  `cublas<t>axpy`, `cublas<t>copy`, `cublas<t>dot`, `cublas<t>scal`,
  `cublas<t>rot`, `cublas<t>rotg`, `cublas<t>rotm`, `cublas<t>rotmg`, and `cublas<t>swap`, plus `cublas<t>asum`, `cublas<t>nrm2`,
  `cublasI<t>amax`, and `cublasI<t>amin`; run bounded FP32/FP64 complex Level-1
  `cublasC/Zaxpy`, `cublasC/Zcopy`, `cublasC/Zdotu`,
  `cublasC/Zdotc`, `cublasC/Zscal`, `cublasC/Zswap`, plus
  `cublasCsscal` and `cublasZdscal`; run FP32/FP64 complex
  `cublasC/Zgemv`, `cublasC/Zgeru`, `cublasC/Zgerc`,
  `cublasC/Zhemv`, `cublasC/Zher`, `cublasC/Zher2`,
  `cublasC/Zherk`, `cublasC/Zher2k`,
  `cublasC/Ztrmv`, and `cublasC/Ztrsv`; run FP32/FP64 `cublas<t>gemv`,
  `cublas<t>ger`, `cublas<t>symv`, `cublas<t>syr`, `cublas<t>syr2`,
  `cublas<t>trmv`, and `cublas<t>trsv`; run FP32/FP64
  `cublas<t>trmm`, `cublas<t>trsm`, `cublas<t>symm`, `cublas<t>syrk`,
  and `cublas<t>syr2k`; and run
  `cublasS/D/C/Zgemm[_v2]`,
  `cublasS/D/C/ZgemmBatched`, and
  `cublasS/D/C/ZgemmStridedBatched`
  on host-accessible CPU pointers, including pointers
  allocated by the simulated runtime shim. On Darwin, real/complex
  `cublasS/D/C/Zgemm[_v2]`, `cublasS/D/C/ZgemmBatched` and
  `cublasS/D/C/ZgemmStridedBatched` batch entries, and real/complex TRMM/TRSM
  route through Accelerate/vecLib CBLAS after the shim's cuBLAS-shaped
  validation. GEMM keeps the shim's temporary output staging and alpha-zero /
  `k == 0` no-read fallbacks; TRMM keeps alpha-zero no-read guards and
  `B`-to-`C` staging. Non-Darwin builds keep the reference-loop implementation. On Apple Silicon,
  `PSYCHE_CUDA_COMPAT_CUBLAS_METAL=required` verifies a real Metal
  shared-buffer dispatch for contiguous FP32 `cublasSaxpy[_v2]` and
  `cublasSscal[_v2]`, plus contiguous FP32 `cublasScopy[_v2]` and
  `cublasSdot[_v2]`, `cublasSasum[_v2]`, `cublasSnrm2[_v2]`, plus
  nonzero signed-stride FP32 `cublasSgemv[_v2]` and
  `cublasSger[_v2]`;
  `PSYCHE_CUDA_COMPAT_CUBLAS_METAL=1` prefers that Metal path and falls
  back to the CPU reference path for fallback-eligible backend errors;
  `PSYCHE_CUDA_COMPAT_CUBLAS_METAL=required` returns the Metal-derived status
  instead of falling back.
  For these Metal routes, contiguous means `incx == 1 && incy == 1` for
  SAXPY/SCOPY/SDOT and `incx == 1` for SSCAL/SASUM/SNRM2; SGEMV and SGER
  stage signed-strided `x` and `y` into compact host buffers before dispatch,
  use Netlib/cuBLAS negative-increment logical indexing for nonzero signed
  increments, reject zero increments,
  and SGEMV scatters compact Metal output back to strided `y` only after a
  successful command-buffer completion.
  `cublasSaxpy[_v2]` and `cublasScopy[_v2]` allow exact `x == y` aliasing in
  the contiguous Metal shim path, but required Metal mode rejects partially
  overlapping source/destination ranges instead of promising CPU-loop overlap
  behavior. The Metal `cublasSgemv[_v2]` route stages `A`, compact `x`, and
  compact old `y` into Metal buffers, computes into a separate compact output
  buffer, and then copies or scatters `y` back after completion, so tested exact
  and partial host overlaps follow the CPU temp-output semantics.
  Strided FP32 AXPY, SSCAL, SCOPY, SDOT, SASUM, and SNRM2 remain CPU-backed, and required Metal mode
  returns `CUBLAS_STATUS_NOT_SUPPORTED` for those shapes instead of falling
  back. GEMV, GER, HEMV, HER, HER2, SYMV, SYR, and SYR2 reject zero increments, but nonzero
  negative increments use signed logical vector order on their CPU-backed paths.
  The Metal-backed cuBLAS path launches a real Metal compute kernel over
  shared Apple Silicon memory; it does not implement CUDA device memory, CUDA
  UVA pointer provenance, CUDA stream semantics, or `torch.cuda` availability.
  The cuBLAS Metal routes copy host-accessible inputs and mutable spans into
  Metal shared buffers, then copy mutated outputs or scalar results back after
  command-buffer completion. The Metal SGEMV route is a baseline one-thread-per-output
  kernel with serial accumulation over each output element, and the Metal SGER
  route is a baseline one-thread-per-matrix-entry rank-1 update bounded to
  32-bit logical update and staged-A element counts. Neither is a tuned/tiled
  cuBLAS implementation. The Metal SDOT, SASUM, and SNRM2 routes use
  parallel reduction order, so low-order floating-point bits can differ from
  the CPU fallback. The Metal SNRM2 route uses a stable `scale`/`ssq` pair
  reduction to avoid naive square overflow/underflow, but it is not bitwise
  NVIDIA cuBLAS parity.
  It also exposes
  `cublasSetVector`, `cublasGetVector`, `cublasSetMatrix`, `cublasGetMatrix`,
  and their async variants for host-accessible byte-span copies, plus status,
  version/property, stream, pointer-mode, math-mode, and atomics-mode helpers.
  Version/property helpers return `0`, and a successful shim handle does not
  mean NVIDIA cuBLAS is present. Stream handles are stored and returned as
  metadata only; `libcublas` does not validate them against the driver/runtime
  stream registries or synchronize work through them. The shim does not claim
  same-handle thread safety; callers must serialize concurrent handle mutation,
  destruction, and operation calls themselves. Device pointer-mode
  scalars/results and tensor/TF32 math modes can be stored as metadata, but
  operations that require host scalar/result pointers, including AXPY, DOT/DOTU/DOTC,
  SCAL, ROT, ROTG, ROTM, ROTMG, reductions, GEMV, GER, HEMV, HER, HER2, HERK, HER2K,
  SYMV, SYR, SYR2, TRMM, TRSM, SYMM, SYRK, SYR2K, GEMM, and GEMM batched/strided-batched, return
  `CUBLAS_STATUS_NOT_SUPPORTED` for device pointer mode; scalar-free COPY,
  SWAP, TRMV, and TRSV are not blocked by pointer mode. Mutating Level-1
  vector ops, DOT/DOTU/DOTC, ROT, and ROTM model positive strides
  only when work is required; GEMV, GER, HEMV, HER, HER2, SYMV, SYR, SYR2, TRMV, and TRSV support nonzero signed strides;
  DOT/DOTU/DOTC write zero for `n <= 0` after result-pointer validation;
  DOTC conjugates the first complex vector, Csscal/Zdscal apply real
  scalar factors to complex vectors, complex GEMV/TRMV/TRSV honor `CUBLAS_OP_C`
  as conjugate transpose, complex GERU uses `y` as-is, complex GERC
  conjugates `y`, complex HEMV reads Hermitian diagonals as real without mutating
  `A`, complex HER/HER2 update only the stored triangle and force updated
  diagonal imaginary parts to zero, complex HERK/HER2K update only the stored
  triangle, force updated diagonal imaginary parts to zero, accept
  `CUBLAS_OP_T` as non-conjugate transpose, and avoid reading `C` when
  `beta == 0` and product inputs when `alpha == 0` or `k == 0`, and complex
  GEMM honors `CUBLAS_OP_C` as conjugate transpose;
  ROT stages original `x`/`y` input values before writes but does not guarantee
  arbitrary overlapping output vector semantics; ROTM applies FP32/FP64 host-param
  modified Givens transforms for flags `-2`, `-1`, `0`, and `1`, stages original
  `x`/`y` input values before writes, rejects undefined flags, and does not guarantee
  arbitrary overlapping output vector semantics; ROTMG constructs FP32/FP64 host scalar
  modified Givens parameters using Netlib scaling rules, writes the flag plus relevant
  parameter entries, leaves flag-implied entries unchanged, and does not guarantee
  arbitrary scalar aliasing; ROTG constructs FP32/FP64
  host scalar Givens parameters and overwrites `a`/`b`/`c`/`s` as `r`/`z`/`c`/`s`
  using Netlib-compatible rules but does not guarantee arbitrary aliasing among
  the scalar output pointers;
  TRMM supports the cuBLAS `C == B` in-place idiom but no other overlap guarantee;
  TRSM overwrites `B` and does not pre-test triangular singularity;
  SYRK/SYR2K and complex HERK/HER2K update only the requested stored `C` triangle and leave the
  opposite triangle untouched; SYMM/SYRK/SYR2K/HERK/HER2K do not guarantee arbitrary
  overlap handling; `beta == 0` avoids reading `C` input storage, and
  `alpha == 0` or `k == 0` avoids reading product inputs where cuBLAS permits;
  GEMM strided-batched permits zero A/B strides to broadcast a shared input matrix across
  batches, rejects negative batch strides, and rejects zero C stride when multiple output
  batches would overlap;
  zero-batch GEMM batched/strided-batched calls do not require matrices but still validate scalar pointers;
  ASUM/NRM2/IAMAX/IAMIN return zero for `n <= 0` or `incx <= 0`. The
  transfer helpers require positive element sizes, vector strides, and matrix
  leading dimensions; zero-work transfer calls may omit source/destination
  pointers; async forms run synchronously and treat streams as metadata only;
  and source bytes are staged so host-overlapping copies are deterministic.
  They do not model real device transfers, CUDA stream synchronization, or
  device-resident pointer provenance. Matrix transfer leading dimensions must
  be at least `rows` when `rows > 0`. When the simulated-memory opt-in is
  disabled, these helpers return `CUBLAS_STATUS_NOT_INITIALIZED` without
  mutating destination buffers. Except for the opt-in contiguous FP32
  `cublasSaxpy[_v2]`, `cublasSscal[_v2]`, `cublasScopy[_v2]`, `cublasSdot[_v2]`,
  `cublasSasum[_v2]`, `cublasSnrm2[_v2]`, signed-stride `cublasSgemv[_v2]`, and signed-stride `cublasSger[_v2]` Metal routes, plus the
  Darwin Accelerate-backed real/complex GEMM/TRMM/TRSM paths and
  real/complex GEMM batched/strided-batched batch entries, real
  ROT/ROTG/ROTM/ROTMG/GEMV/GER/SYMV/SYR/SYR2/TRMV/TRSV/SYMM/SYRK/SYR2K, complex Level-1,
  complex GEMV/GERU/GERC/HEMV/HER/HER2/HERK/HER2K/TRMV/TRSV paths use straightforward CPU accumulation for
  FP32/FP64 buffers and are not bitwise-equivalent to NVIDIA cuBLAS. The shim
  does not validate arbitrary pointer provenance; callers must pass valid
  host-accessible CPU buffers large enough for the requested shapes, strides, and batches.
  For pointer-array batched GEMM, the pointer arrays themselves and every
  pointed-to matrix must be CPU-addressable; device-resident pointer arrays are
  not modeled. Batched GEMM callers must also keep `C[i]` matrices
  independently computable and non-overlapping; the shim does not enforce that
  cuBLAS precondition.
  Non-`_v2` cuBLAS symbols use the current handle-based cuBLAS ABI shape as
  aliases for the matching `_v2` implementations; legacy cuBLAS v1 by-value /
  no-handle ABI is not modeled. Arbitrary real CUDA/UVA/foreign
  pointers, tensor cores, CUDA kernels, cuBLASLt advanced layouts/epilogues/tensor
  modes/low-precision paths, complex Level-2 outside GEMV/GERU/GERC/HEMV/HER/HER2/TRMV/TRSV and complex Level-3 outside GEMM/HERK/HER2K,
  half/TF32 operations, or Apple GPU execution outside the opt-in contiguous
  FP32 `cublasSaxpy[_v2]`, `cublasSscal[_v2]`, `cublasScopy[_v2]`, and
  `cublasSdot[_v2]`, `cublasSasum[_v2]`, `cublasSnrm2[_v2]`, and
  signed-stride `cublasSgemv[_v2]`, and signed-stride `cublasSger[_v2]` Metal routes are not modeled.
- under the same explicit `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY` opt-in,
  `libcublasLt` can create/destroy handles, initialize and allocate opaque
  matmul descriptors, matrix layouts, preferences, and algorithm records, store
  and retrieve the supported descriptor attributes, return one zero-workspace
	  heuristic for supported cases, and run real FP32/FP64 `cublasLtMatmul` with
	  `CUBLASLT_ORDER_COL` and `CUBLASLT_ORDER_ROW` matrix layouts over
	  host-accessible CPU pointers. The matmul path supports `CUBLAS_OP_N`,
	  `CUBLAS_OP_T`, and real-valued `CUBLAS_OP_C` as transpose, distinct `D` and
	  `C` buffers, `beta == 0` without reading `C`, null `C` data for that
	  beta-zero case when the descriptor is valid, compatible strided-batch layouts,
	  and `CUBLASLT_EPILOGUE_DEFAULT` / `CUBLASLT_EPILOGUE_RELU` /
	  `CUBLASLT_EPILOGUE_RELU_AUX` / `CUBLASLT_EPILOGUE_BIAS` /
	  `CUBLASLT_EPILOGUE_RELU_BIAS` / `CUBLASLT_EPILOGUE_RELU_AUX_BIAS` /
	  `CUBLASLT_EPILOGUE_DRELU` / `CUBLASLT_EPILOGUE_DRELU_BGRAD` /
	  `CUBLASLT_EPILOGUE_GELU` / `CUBLASLT_EPILOGUE_GELU_BIAS` /
	  `CUBLASLT_EPILOGUE_GELU_AUX` / `CUBLASLT_EPILOGUE_GELU_AUX_BIAS` /
	  `CUBLASLT_EPILOGUE_DGELU` / `CUBLASLT_EPILOGUE_DGELU_BGRAD` /
	  `CUBLASLT_EPILOGUE_BGRADA` / `CUBLASLT_EPILOGUE_BGRADB`.
	  BIAS-bearing epilogues read a caller-owned CPU-resident host-accessible bias
	  vector with dtype unset/default or matching `D`, length equal to `D.rows`,
	  and optional `CUBLASLT_MATMUL_DESC_BIAS_BATCH_STRIDE` in elements; stride zero
	  broadcasts the same bias vector across batches, while positive strides select
	  a per-batch vector and must be at least `D.rows`. Bias is applied as
	  `bias[row]` after `alpha*A@B + beta*C` and before optional ReLU or GELU.
	  `RELU_AUX` and `RELU_AUX_BIAS` write a ReLU bit-mask AUX buffer using
	  logical bit index `row + col * AUX_LD`; NVIDIA documents `AUX_LD` and
	  positive `AUX_BATCH_STRIDE` values for ReLU masks as bits, divisible by 128,
	  with `AUX_LD >= D.rows`, so the shim's overlap span covers
	  `AUX_LD * D.cols` bits. Within each byte the host bridge uses an LSB-first
	  convention that is covered by fixed-byte tests but still needs a real
	  NVIDIA AUX-buffer diff before claiming bit-for-bit interchange with
	  cuBLASLt. TODO: replace this convention note with a real NVIDIA AUX-buffer
	  byte diff once one is available. `DRELU` and `DRELU_BGRAD` read that same
	  bit-mask and write
	  `raw_dy` where the mask bit is set, otherwise zero. Following cuBLASLt's
	  "apply independently ReLu and Bias gradient to matmul output" wording,
	  `DRELU_BGRAD` writes an independent raw-dy row-wise bias-gradient output,
	  with FP32 reductions accumulated in FP64. ReLU mask epilogues reject
	  non-default AUX data types because the AUX buffer is a bit mask, not a typed
	  matrix.
	  `GELU_AUX` and `GELU_AUX_BIAS` write the pre-GELU logical output matrix to a
	  caller-owned CPU-resident host-accessible AUX buffer after optional bias and
	  before GELU, using column-major AUX indexing with `AUX_LD` in elements.
	  `DGELU` and `DGELU_BGRAD` read the same logical column-major AUX matrix as
	  the saved GELU preactivation input, apply the derivative of the documented
	  tanh GELU approximation to `alpha*A@B + beta*C`, and write the result to
	  `D`. `DGELU_BGRAD` also writes a bias-gradient vector of length `D.rows`
	  where `bias_gradient[row] = sum_col raw_dy[row,col]`, before the DGELU
	  multiply; FP32 bias-gradient reductions accumulate in FP64 before storing
	  the FP32 output. Multi-batch DGELU_BGRAD requires a positive bias stride at
	  least `D.rows`; stride-zero broadcast is only accepted for bias input
	  epilogues. The DGELU derivative is gradient-consistent with this shim's
	  tanh-approximation GELU/GELU_AUX paths, not with an exact-erf GELU
	  implementation such as PyTorch's default eager GELU.
	  `BGRADA` and `BGRADB` write operand-source bias-gradient vectors without
	  alpha/beta scaling: `BGRADA` writes length `D.rows` with
	  `bias_gradient[row] = sum_k op(A)[row,k]`, and `BGRADB` writes length
	  `D.cols` with `bias_gradient[col] = sum_k op(B)[k,col]`. The reduced
	  source operand is required even when alpha is zero; the other source operand
	  keeps the alpha-zero no-read behavior. Positive bias stride must cover the
	  selected gradient length, and multi-batch BGRADA/BGRADB rejects stride-zero
	  broadcast. FP32 operand-gradient reductions accumulate in FP64 before
	  storing FP32 output. `BGRADA`/`BGRADB` currently leave `D` as the raw
	  DEFAULT matmul output on the host bridge and emit a one-time runtime warning
	  for that unverified D-output behavior; the D-output behavior and reduction
	  order still need a real NVIDIA hardware byte diff before claiming bitwise
	  parity.
	  AUX dtype must be unset/default or match `D`, `AUX_LD` must be divisible by
	  8 elements, at least `D.rows`, and within the backend indexing ceiling,
	  positive `AUX_BATCH_STRIDE` values are in elements and must cover
	  `AUX_LD * D.cols`, and zero AUX stride is rejected for multi-batch AUX writes
	  or reads that would alias per-batch state. Execution rejects runtime AUX/D,
	  D/bias-gradient, AUX/bias-gradient for DRELU_BGRAD/DGELU_BGRAD, and
	  reduced-source/bias-gradient range overlap before writing output buffers.
	  Row-major `D` support for these epilogues is an intentional shim
	  compatibility extension beyond NVIDIA's documented row-major restriction.
	  ReLU clamps each logical
	  output with CUDA-style `value > 0 ? value : 0` semantics, while GELU applies
	  NVIDIA's documented tanh approximation before the `D` write. GELU propagates
	  NaN, maps `+Inf` to `+Inf`, and maps `-Inf` to `0` in this bounded reference
	  path. Layout order, epilogue, bias-data-type, and aux-data-type changes
	  validate before mutating descriptor state. BIAS/AUX pointers are validated
	  for non-null state and element-size alignment, but not lifetime, bounds, or
	  arbitrary CUDA/UVA pointer provenance beyond the AUX/D overlap check. On Darwin, all-column-major supported cases use
	  Accelerate/vecLib CBLAS for the raw GEMM core after cuBLASLt-shaped
	  validation; cuBLASLt epilogues are applied afterward by CPU postprocessing.
	  Row-major or mixed-order layouts use the CPU reference-loop GEMM core.
	  Non-Darwin builds keep the reference-loop implementation. Streams and
	  workspaces are accepted as metadata only. Pointer-array batch layout mode is
	  supported for DEFAULT-only matmuls when A/B/C/D descriptors all use
	  `CUBLASLT_BATCH_MODE_POINTER_ARRAY` with matching batch counts; the A/B/C/D
	  call arguments are CPU-addressable arrays of host-accessible matrix
	  pointers, strided batch offsets are ignored in that mode, required null
	  entries are rejected before any `D` batch is written, and non-DEFAULT
	  pointer-array epilogues return no heuristic and fail execution as
	  not-supported.
	  `cublasLtMatrixTransform` and its transform descriptor APIs support the
	  bounded host bridge for real FP32/FP64 `CUBLASLT_ORDER_COL` and
	  `CUBLASLT_ORDER_ROW` layouts: host pointer-mode alpha/beta scalars, FP32 or
	  FP64 scale type with input conversion to scale type and output conversion to
	  C's data type, `CUBLAS_OP_N`, `CUBLAS_OP_T`, and real `CUBLAS_OP_C` as
	  transpose, alpha-zero and beta-zero no-read behavior, strided batches with
	  input batch-count-one broadcast to C's batch count, and pointer-array
	  batches when participating A/B/C descriptors all use pointer-array mode with
	  exact batch counts. Pointer-array entries required by nonzero alpha/beta and
	  output writes are preflighted before any C batch is mutated. Unsafe
	  source/C byte-range overlap is rejected, while exact same-layout
	  no-transpose in-place sources are allowed. Validation and preflight
	  rejection paths leave C unchanged; successful CPU arithmetic writes C
	  elementwise and does not promise rollback for ordinary floating-point
	  NaN/Inf/overflow results. Device pointer-mode transform
	  descriptors preserve the attribute for ABI/config compatibility only, but
	  execution returns not-supported.
	  Tiled layouts,
	  half/BF16/FP8/complex/int data types, tensor-core and TF32 modes,
	  device/vector pointer modes, AUX scale/amax outputs, grouped batches,
	  real CUDA async semantics, and device-resident pointer
	  provenance remain unsupported.
	  Any future complex MatrixTransform bridge must implement `CUBLAS_OP_C` as a
	  true conjugating transpose rather than reusing the current real-type
	  transpose equivalence.
- under the same explicit `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY` opt-in,
  `libcusparse` can create/destroy handles, store stream and pointer-mode
  metadata, create/destroy CSR sparse-matrix descriptors and dense-vector
  descriptors, create/destroy dense-matrix descriptors, return
  status/version/property helpers, and run bounded `cusparseSpMV_bufferSize` /
  `cusparseSpMV` and `cusparseSpMM_bufferSize` / `cusparseSpMM` subsets. The
  SpMV executable subset is FP32 CSR SpMV over host-accessible CPU pointers:
  `CUSPARSE_OPERATION_NON_TRANSPOSE`,
  `CUDA_R_32F` matrix/vector/compute types, matching 32-bit or 64-bit row and
  column indices on the CPU route, zero-based or one-based CSR, host alpha/beta
  scalars, and
  `CUSPARSE_SPMV_ALG_DEFAULT`, `CUSPARSE_SPMV_CSR_ALG1`, or
  `CUSPARSE_SPMV_CSR_ALG2`. The workspace query returns zero bytes because the
  shim uses internal staging. The operation validates descriptor lifetimes,
  dimensions, row-offset monotonicity and endpoints, column ranges, alignment,
  and output/source overlap before mutating `y`; the CPU path computes into a
  temporary output buffer and copies back only after validation and execution
  succeed. On Apple Silicon, `PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=required`
  verifies a real Metal shared-buffer CSR SpMV route for that same subset, and
  `PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=1` prefers Metal with CPU fallback only
  for backend-availability failures. The Metal route currently requires 32-bit
  row and column indices, stages CSR, `x`, and prior `y` into shared buffers,
  computes one output row per thread into a separate output buffer, and copies
  `y` back only after command-buffer completion. In required-Metal mode,
  64-bit CSR indices return not-supported without CPU fallback until the local
  Metal toolchain can prove an MSL 64-bit-index kernel. The SpMM executable
  subset is FP32 CSR SpMM with `opA == opB == CUSPARSE_OPERATION_NON_TRANSPOSE`,
  `CUDA_R_32F` sparse/dense/compute types, matching 32-bit or 64-bit row and
  column indices, zero-based or one-based CSR, host alpha/beta scalars,
  `CUSPARSE_ORDER_COL` or `CUSPARSE_ORDER_ROW` dense B/C matrices with
  leading-dimension validation, and `CUSPARSE_SPMM_ALG_DEFAULT`,
  `CUSPARSE_SPMM_CSR_ALG1`, `CUSPARSE_SPMM_CSR_ALG2`, or
  `CUSPARSE_SPMM_CSR_ALG3`. Its workspace query also returns zero bytes and
  validates descriptor/contract metadata; in required-Metal mode it also applies
  the 32-bit-index/uint-limit Metal supportability preflight, but it
  intentionally does not read CSR row/column contents. The CPU SpMM path validates CSR indices, dense
  layouts, dimensions, B/C aliasing, and C overlap with CSR storage before
  writing; it computes into a temporary logical C buffer, copies back only after
  success, and does not read prior C when beta is zero. On Apple Silicon,
  `PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=required` verifies a real Metal
  shared-buffer CSR SpMM route for the 32-bit-index subset, stages CSR, `B`, and
  prior `C` when beta is nonzero, computes compact logical `C` into a separate
  shared buffer, and copies `C` back only after command-buffer completion.
  `PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=1` prefers Metal and falls back to the CPU
  reference path only for backend-availability failures. In required-Metal mode,
  64-bit CSR indices return not-supported without CPU fallback until the local
  Metal toolchain can prove an MSL 64-bit-index kernel.
  Device pointer-mode scalars, transpose/conjugate transpose, non-CSR formats,
  16-bit indices, non-FP32/complex/low-precision values, preprocess and
  update APIs, broader SpMM formats/algorithms, SpSV/SpSM, sparse/dense conversions, batched sparse APIs,
  external workspace semantics, CUDA streams, CUDA graphs, and asynchronous
  behavior are not modeled.
  Under the same explicit simulated-memory opt-in, the `libcusolver` shim can
  create/destroy cuSolverDN handles, store stream metadata, report
  version/property/status helpers, and run bounded `cusolverDnSgetrf` /
  `cusolverDnDgetrf` plus `cusolverDnSgetrs` / `cusolverDnDgetrs` dense
  FP32/FP64 LU factorization and solve paths over host-accessible
  column-major pointers. On Darwin, pivoted `getrf`/`getrs` routes through
  Accelerate/LAPACK and preserves 1-based pivot arrays; singular factors return
  success with positive `devInfo`. Singularity detection follows exact-zero
  pivot signaling; this shim does not estimate conditioning or warn on
  near-singular matrices. Non-Darwin builds use a deterministic CPU reference
  partial-pivot LU/solve for the same bounded subset. `devIpiv == NULL`
  uses a deterministic no-pivot LU/triangular-solve bridge, matching cuSOLVER's
  documented no-pivot mode rather than rejecting the surface. If the no-pivot `getrs`
  path encounters a zero diagonal, it returns `CUSOLVER_STATUS_EXECUTION_FAILED`
  with positive `devInfo` and leaves B unchanged; `getrs` does not reuse
  `getrf`'s positive-success singular contract. `getrf_bufferSize` returns
  `m * n` elements, and `getrf` validates a non-null workspace when work is
  required even though Accelerate uses internal storage. Mutable A/B buffers
  are staged so validation, allocation, no-pivot solve failure, and required backend failures leave
  caller data unchanged.
  The same opt-in path now covers bounded dense FP32/FP64 Cholesky
  `cusolverDnS/Dpotrf_bufferSize`, `cusolverDnS/Dpotrf`,
  `cusolverDnS/Dpotri_bufferSize`, `cusolverDnS/Dpotri`, and
  `cusolverDnS/Dpotrs` over host-accessible column-major pointers.
  `potrf_bufferSize` and `potri_bufferSize` return a conservative `n * n`
  element workspace; execution validates that workspace even though Accelerate
  uses internal storage. On Darwin, `potrf`/`potri`/`potrs` route through
  Accelerate/LAPACK after explicit lower/upper translation; non-Darwin builds
  use deterministic CPU reference Cholesky, inverse-from-factor, and triangular
  solve routes. Only the requested lower or upper triangle is referenced, the
  opposite triangle is left untouched, and positive non-positive-definite
  `potrf` `devInfo` leaves caller A unchanged because neither cuSOLVER nor
  LAPACK guarantees useful partial factors. `potri` consumes an existing
  Cholesky factor rather than refactorizing; it copies back only the requested
  inverse triangle on success, returns success with positive `devInfo` for an
  exact-zero factor diagonal, and leaves A unchanged on validation, allocation,
  or singular-factor failures. `potrs` stages B so failed solves leave it
  unchanged; the exact-zero diagonal `potrs` precheck is a shim-safety behavior,
  not a real cuSOLVER guarantee.
  Device pointers, async execution, CUDA streams beyond metadata, CUDA graphs,
  batched dense solvers, sparse cuSolverSP/cuDSS-style solves, QR, eigen, SVD,
  IRS, RF/Mg, complex and low-precision data types, real GPU residency, and
  bitwise NVIDIA parity are not modeled.

That makes it useful for dependency probes that only need a CUDA library to be
present, while keeping actual CUDA execution paths explicit and blocked until a
broader Metal-backed compiler/runtime bridge exists beyond the registered
`vector_add_f32` / `saxpy_f32` / `scale_f32` / `axpby_f32` driver-kernel and
runtime-token subsets.

Some execution, simulated-memory, and simulated-sync symbols are exported
directly so linkers and `ctypes` probes can resolve them, but dynamic lookup
through `cuGetProcAddress` deliberately withholds module-load, kernel-launch,
simulated-memory, simulated-mempool, and simulated-sync symbols by returning a
null function pointer and `CU_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND` status.
That split is intentional: linkability is not execution support.

Do not add the output directory to the library search path of a real training
process. These libraries use canonical CUDA names and are discovery stubs only;
they must not shadow a real CUDA installation.

Build and check it with:

```bash
python3 scripts/check-cuda-driver-stubs.py
PSYCHE_CUDA_COMPAT=1 scripts/check-cuda-compat.py
```

The check builds both `libnvidia-ml.dylib` and a local `libnvidia-ml.so.1`
alias so PyTorch's NVML-based CUDA availability probe can load the shim and
stay on the false/no-device path. It also builds `libcudnn.dylib`, verifies the
default no-fake-handle path, and exercises the opt-in cuDNN activation,
pooling, and softmax bridges including required-Metal behavior. It builds
`libcusparse.dylib` and exercises the opt-in FP32 CSR SpMV/SpMM bridges through
CPU, preferred Metal fallback, required Metal success/failure, base-zero/base-one
CSR, unsupported dtype/transpose/pointer-mode rejection, invalid CSR validation,
and double-destroy safety. It also builds `libcusolver.dylib` and checks the
opt-in dense FP32/FP64 cuSolverDN `getrf_bufferSize` / `getrf` / `getrs`
bridge, including pivoted and no-pivot solves, singular `devInfo`, workspace
validation, no-pivot zero-diagonal execution failure, overlap rejection,
`potrf_bufferSize` / `potrf` / `potrs` Cholesky factorization and solve,
untouched-triangle behavior, non-positive-definite `devInfo`, failed-solve
copyback, and
handle lifecycle behavior.
