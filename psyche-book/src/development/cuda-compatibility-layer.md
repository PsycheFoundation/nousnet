# Apple Silicon CUDA Compatibility Levels

This page tracks the CUDA-facing side of the native Apple Silicon work.

Josh's constraint is the right one: do not fake CUDA. Apple GPUs are not CUDA
devices, and Psyche should not tell PyTorch, Triton, FlashAttention, NCCL, or
CUDA extensions that CUDA is available when the actual compute path is MPS or
Metal.

The goal is layered support:

1. Accept CUDA-shaped user intent where Psyche owns the boundary.
2. Translate that intent to real Apple Silicon backends when the operation can
   honestly run there.
3. Keep unsupported CUDA runtime, driver, compiler, and kernel paths explicit.
4. Add deeper support by porting real operations to MPSGraph, Metal, MLX, or a
   future compiler/runtime bridge.

## Research Baseline

Authoritative docs split the problem into three layers:

- PyTorch exposes a large `torch.cuda` Python surface: availability probes,
  device properties, streams/events, memory accounting, seeding, and `.cuda()`
  helpers.
- NVIDIA's CUDA runtime and driver APIs are much wider. Device discovery alone
  includes `cudaGetDeviceCount`, `cudaGetDeviceProperties`, `cuInit`,
  `cuDeviceGetCount`, `cuDeviceGetName`, and `cuDeviceTotalMem`; execution,
  modules, streams, events, memory pools, graphs, libraries, and JIT loading are
  separate surfaces.
- Apple's supported compute route is Metal/MPS/MPSGraph. PyTorch MPS maps
  tensors and ops to MPSGraph and Metal Performance Shaders; unsupported ops can
  fall back to CPU with `PYTORCH_ENABLE_MPS_FALLBACK=1`, but that is not proof of
  GPU residency.

Existing compatibility projects are useful precedents without being direct
answers. ZLUDA-style work is driver/runtime translation for existing CUDA
binaries. SCALE-style work is a CUDA-compatible compiler/runtime stack for
non-NVIDIA GPUs. A real Apple Silicon equivalent would need a Metal/MLX compiler
and runtime path, not a Python flag.

## Level 1: Psyche Device Translation

The current first slice lives in:

```bash
python/python/psyche/cuda_compat.py
```

Enable it with:

```bash
PSYCHE_CUDA_COMPAT=1
```

What it does:

- Resolves Psyche-owned device arguments like `0`, `"cuda"`, `"cuda:0"`, and
  `torch.device("cuda:0")` to `torch.device("mps")` when real CUDA is absent and
  MPS is available.
- Does not treat `None` as a CUDA request. Callers must pass explicit
  CUDA-shaped intent at a Psyche-owned boundary.
- Returns a structured resolution status so callers can tell whether they are
  using real CUDA, redirected MPS, disabled compatibility, or an unavailable
  backend.
- Leaves PyTorch itself honest. `torch.cuda.is_available()` is not patched.
  `Tensor.cuda()`, `Module.cuda()`, `tensor.to("cuda")`,
  `torch.empty(..., device="cuda")`, `torch.device("cuda")`, and `tensor.is_cuda`
  are not patched.

Check the layer:

```bash
PSYCHE_CUDA_COMPAT=1 scripts/check-cuda-compat.py
python3 scripts/check-cuda-driver-stubs.py
python3 scripts/report-cuda-compat-coverage.py --check --json-out target/cuda-compat-coverage.json --markdown-out target/cuda-compat-coverage.md
PSYCHE_CUDA_COMPAT=1 scripts/check-sidecar-mps-device.py
PYTORCH_ENABLE_MPS_FALLBACK=0 PSYCHE_CUDA_COMPAT=1 scripts/check-hfauto-mps-redirect.py
```

## Level 2: MPS Operator Coverage

CUDA-shaped model code only helps if the redirected workload actually runs on
MPS. That work stays in:

```bash
python/python/psyche/mps_compat.py
```

The promotion rule remains strict: claim an op only when the no-fallback
baseline fails, the MPS route returns MPS tensors, and CPU/MPS parity is tested.
CPU fallback is a last resort, not compatibility proof.

When `PSYCHE_CUDA_COMPAT=1` redirects CUDA-shaped Psyche intent to MPS, the exact
MPS compatibility routes are enabled by default for MPS execution contexts. This
keeps redirected CUDA-shaped runs from landing on raw PyTorch MPS gaps that
Psyche already has exact routes for. Set `PSYCHE_CUDA_COMPAT_MPS_ROUTES=0` to
audit the unmodified PyTorch MPS surface; accepted false spellings are `0`,
`false`, `no`, and `off`. Approximate research routes, such as the SVD diagnostic
dispatch mode, are still not installed by default.

## Level 3: Package-Specific Source Rewrites

For packages that ship CUDA extensions or branch into CUDA-only kernels, the
honest path is source-level routing:

- choose SDPA over FlashAttention CUDA kernels on MPS;
- keep Liger, Triton, CUDA extensions, and NCCL disabled unless a real MPS/Metal
  implementation exists;
- port reference PyTorch implementations first;
- replace hot paths with MPSGraph, custom Metal kernels, or MLX once correctness
  is proven.

Redirected MPS is currently validated only for the `HfAuto` Psyche architecture.
That validation means a tiny HfAuto causal LM can load CPU state into MPS
parameters, redirect a CUDA-shaped request to MPS, normalize FlashAttention-2
intent to SDPA, run forward/backward with `PYTORCH_ENABLE_MPS_FALLBACK=0`, and
stay close to the CPU baseline. The same check asserts that a non-allowlisted
architecture is rejected under redirected CUDA-shaped intent. Other
architectures must opt in after their MPS path is tested; for example,
Torchtitan remains CUDA-only and should fail early instead of being handed an MPS
device through a CUDA-shaped request.

The Rust feature graph now separates high-level parallel-model code from
CUDA/NCCL bindings:

```bash
cargo check -p psyche-modeling --features python,parallelism-core
cargo check -p psyche-python-extension --features apple-silicon
```

Those Apple-safe targets compile without `tch/nccl` or `torch-sys/nccl`.
`parallelism-core` is the shared modeling code that does not select NCCL.
CUDA distributed builds must opt into `cuda-parallelism`, which is expected to
require CUDA/NCCL headers and remains unsupported on Apple Silicon.

## Level 4: Runtime And Driver Bridge

A future `libcuda` / `libcudart` bridge must still avoid fake success. A truthful
stub may report versions, errors, and zero supported devices by default. It
should not report a usable CUDA device, allocate fake device pointers, or claim
PTX/CUBIN execution until there is a real Metal-backed runtime and compiler
path. Narrow, named Psyche-native module formats are allowed when each supported
kernel is explicitly registered and verified.

The first native macOS-only shim lives in:

```bash
tools/cuda-compat-shim/
```

It builds minimal `libcuda`, `libcudart`, `libcublas`, `libcublasLt`,
`libcusparse`, `libcusolver`, `libnvidia-ml`, and `libcudnn` dynamic
libraries for discovery and linkage probes. The shim returns version
`0`, reports
`CUDA_ERROR_NO_DEVICE` from
driver initialization instead of claiming a working CUDA driver, reports zero
CUDA-capable devices from count queries, maps common error names/strings, exposes
a Psyche-specific stub identity symbol, and rejects arbitrary module load and
kernel launch paths outside the explicitly registered Psyche-native subset.
Allocation, managed-memory, copy, fill, pointer-query, and
pinned-host-memory calls report no device by default; a separate
`PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY` opt-in enables bounded CPU-backed memory
probes, stream-ordered allocation/free calls, host/no-location memory-pool
metadata, and host-side stream/event synchronization without reporting a CUDA
device. The bounded memory path now includes CPU-backed managed allocation,
fixed-ABI advice/prefetch, range queries, pointer queries, at-least-256-byte
aligned simulated device/managed/async/pool-backed allocations, explicit pinned
host pools, CUDA 13 location-default host and managed/no-location pools, and
linear 3D host/device copies as pitched row copies across depth; CUDA arrays,
textures, surfaces, unified-memory 3D operands, GPU-resident pools,
imported/exported pools, pool access-control, graph allocator ownership, real
managed-memory migration, residency, page faults, and access counters remain
unsupported. The memory-pool structs follow the current CUDA 13 `maxSize` /
`reserved[54]` / `usage` layout. Pool trim APIs validate handles but do not
model a retained backing-store cache: reserved bytes track live simulated
allocations. Runtime `cudaDeviceReset` reports no-device when there is neither
a CUDA device nor simulated shim state, but still succeeds when clearing
previously-created simulated allocations, pools, streams, events, or host
registrations. Simulated streams are registry-validated metadata handles; simulated events are CPU
monotonic-time markers; priorities are metadata-only and normalized to `0`;
runtime and driver handle domains stay separate; interprocess events, CUDA contexts,
context/device-wide synchronization, and device priority-range queries are still
unsupported. Under the same explicit simulated-memory opt-in, `cuModuleLoadData`
accepts a Psyche-native `PSYCHE_CUDA_MODULE_V1` blob declaring
`vector_add_f32` and/or `saxpy_f32` and/or `scale_f32` and/or `axpby_f32`,
`cuModuleGetFunction` resolves those registered functions, and `cuLaunchKernel`
runs their fixed 1D parameter schemas over simulated driver allocations.
`vector_add_f32` computes `out[i] = a[i] + b[i]`; `saxpy_f32` computes in-place
`y[i] = alpha * x[i] + y[i]`; `scale_f32` computes in-place
`x[i] = alpha * x[i]`; `axpby_f32` computes in-place
`x[i] = alpha * x[i] + beta * y[i]`. The default path is a CPU reference kernel; on
Apple Silicon, `PSYCHE_CUDA_COMPAT_METAL_KERNELS=required` verifies a real Metal
shared-buffer dispatch for those same registered kernels.
`PSYCHE_CUDA_COMPAT_METAL_KERNELS=1` prefers Metal and falls back to the CPU
reference path if the private Metal backend is unavailable. The private Metal
backend is synchronous: it copies simulated allocation spans into Metal-owned
shared buffers, waits for command-buffer completion, then copies mutated output
spans back only after a completed command. Exact aliases are supported for the
tested in-place/output cases (`saxpy_f32` `x == y`, `vector_add_f32`
`out == a` / `out == b`, and `axpby_f32` `x == y`). Required Metal mode rejects
partial overlaps involving a mutated span, while preferred Metal mode falls back
to the CPU reference kernel for those overlap shapes.
Raw PTX/CUBIN, `cuModuleLoad` files, arbitrary kernels, dynamic shared memory,
extra launch config, multidimensional launches, and general CUDA execution
remain unsupported. The `libnvidia-ml` shim is a discovery and telemetry-failure
surface, not an Apple GPU telemetry adapter: `nvmlInit*` uses NVML-like
refcount semantics, `nvmlDeviceGetCount[_v2]` reports zero NVIDIA devices,
system version helpers return parseable stub versions, and PyTorch's
NVML-based CUDA availability probe stays on the false/no-device path. Handle
lookup and per-device telemetry queries fail without mapping Apple GPU
identity, thermals, power, utilization, process accounting, or memory pressure
into NVIDIA NVML fields. The `libcudnn` shim is a discovery/status surface by
default: version, CUDART-version, max-device, property, and error-string helpers
report zero/stub values, and `cudnnCreate` clears the output handle and returns
not-initialized when simulated memory is not enabled. Under the same explicit
simulated-memory opt-in, it can create metadata handles plus tensor and
activation descriptors for a bounded `cudnnActivationForward` /
`cudnnActivationBackward` subset: contiguous 4D NCHW FP32
ReLU/Sigmoid/Tanh/Identity with host alpha/beta scalars and value-identical
tensor descriptors. Activation coefficients are ignored for those supported
modes, matching cuDNN's descriptor semantics. Forward
supports exact `x == y`; backward validates `y` but computes derivatives from
`x`, supports exact `dy == dx`, avoids reading old dx when beta is zero, and
sanitizes NaN x to zero under `CUDNN_NOT_PROPAGATE_NAN` while upstream dy NaNs
still propagate. The backward path is a bounded compatibility bridge, not
bitwise cuDNN parity for implementations that derive sigmoid/tanh gradients
from saved raw activation output y. The default activation path is a CPU
reference loop; on Apple Silicon,
`PSYCHE_CUDA_COMPAT_CUDNN_METAL=required` verifies real Metal shared-buffer
dispatch for the same subset, and `PSYCHE_CUDA_COMPAT_CUDNN_METAL=1` prefers
Metal with CPU fallback only for backend-availability failures. The Metal path
stages inputs and prior outputs separately and copies back only after command
completion, so required-mode launch failures leave outputs unchanged. Partial
x/y forward overlap, partial dy/dx backward overlap, backward x/dx or y/dx
overlap, NHWC, non-FP32, custom strides, clipped ReLU, ELU, swish, broader
convolution, broader normalization, graph, and general cuDNN kernel execution
remain unsupported.
Under the same explicit simulated-memory opt-in, `cudnnAddTensor` supports a
bounded 4D dense NCHW FP32 bias-add subset: each source A dimension must equal
the destination C dimension or be `1`, including same-shape adds and the common
`1xCx1x1` bias into `NxCxHxW`. It computes
`C = alpha * A_broadcast + beta * prior_C` with host alpha/beta scalars, rejects
any A/C byte-range overlap including exact aliasing, avoids reading prior C when
beta is zero, still evaluates `alpha * A` so alpha-zero NaN source values
propagate, defaults to CPU reference execution, and uses the same
required/preferred Metal route. 5D tensors, NHWC, non-FP32 tensors,
non-contiguous/custom strides, aliased A/C storage, and real cuDNN async
semantics remain unsupported.
Under the same explicit simulated-memory opt-in,
`cudnnBatchNormalizationForwardInference` supports the deprecated legacy cuDNN
inference API for bounded 4D dense NCHW FP32 tensors. X/Y descriptors must match,
`CUDNN_BATCHNORM_SPATIAL` uses `1xCx1x1` scale/bias/mean/variance descriptors,
and `CUDNN_BATCHNORM_PER_ACTIVATION` uses `1xCxHxW` descriptors. It computes
`y = beta * prior_y + alpha * (bias + scale * (x - mean) / sqrt(epsilon + variance))`,
rejects epsilon below `CUDNN_BN_MIN_EPSILON`, rejects
`CUDNN_BATCHNORM_SPATIAL_PERSISTENT`, avoids reading prior y when beta is zero,
still evaluates the normalized result so alpha-zero source/parameter NaNs
propagate, allows exact `x == y` in-place inference, rejects partial x/y overlap
and parameter/stat buffer overlap, leaves negative estimated variance to produce
the formula's natural NaN-domain result, defaults to CPU reference execution, and
uses the same required/preferred Metal route. Training forward, backward, 5D
tensors, NHWC, non-FP32 tensors, non-contiguous/custom strides, broader
normalization APIs, and real cuDNN async semantics remain unsupported.
Under the same explicit simulated-memory opt-in, `cudnnConvolutionForward`
supports a bounded deprecated legacy 2D convolution-forward subset. The bridge
adds cuDNN filter and convolution descriptors, `cudnnSetFilter4dDescriptor`,
`cudnnSetConvolution2dDescriptor`, `cudnnGetConvolution2dForwardOutputDim`,
the legacy forward algorithm query helpers, forward workspace-size query, and
`cudnnConvolutionForward` for contiguous 4D NCHW FP32 x/y tensors with
contiguous FP32 NCHW/KCRS filters using cuDNN grouped semantics: full x/y
descriptors, filter `C = input_C/groupCount`, positive `groupCount`,
`input_C % groupCount == 0`, and `K % groupCount == 0`. Depthwise and
depthwise-multiplier cases are covered by the same rule. The supported
algorithm is deterministic zero-workspace `CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM`
for `CUDNN_CROSS_CORRELATION` and `CUDNN_CONVOLUTION` by flipping only the
spatial R/S filter axes. Algorithm
queries validate the same bounded descriptor configuration, report exactly one
deterministic zero-workspace IMPLICIT_GEMM algorithm, and do not claim
alternates. It uses cuDNN's 2D padding/stride/dilation output formula, requires
y dimensions to match the computed `N,K,H,W`, accepts but ignores workspace for
the supported algorithm, rejects unsupported algorithms and any x/y/w byte-range
overlap, avoids reading prior y when beta is zero, follows the shim's literal
`alpha * result + beta * prior_y` formula so alpha-zero source/filter NaNs can
propagate, defaults to CPU reference execution, and uses the same
required/preferred Metal route for the same FP32 contiguous 4D NCHW/KCRS
grouped subset. Non-contiguous tensors, FP16, and non-NCHW layouts remain
unsupported rather than being silently reshaped. With
`PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required`, the Metal route can prefer an
MPSGraph `convolution2D` fast path for that same NCHW/OIHW grouped FP32 subset,
including cross-correlation, true-convolution, grouped, depthwise,
depthwise-multiplier, stride, padding, and dilation cases covered by the
regression harness. An MSL prepare kernel copies cross-correlation weights or
flips only the logical R/S axes for true convolution; dilation stays on the
MPSGraph descriptor. MPSGraph writes raw y into an internal shared buffer, then
an MSL epilogue applies alpha/beta and beta-zero no-read behavior before CPU
copy-back. Required mode leaves caller y unchanged on prepare, graph, or
epilogue failure, while preferred mode falls back to deterministic MSL.
Under the same explicit simulated-memory opt-in,
`cudnnConvolutionBiasActivationForward` supports a bounded legacy fused
forward subset for contiguous 4D NCHW FP32 x/z/y tensors, contiguous FP32 KCRS
filters using the same cuDNN grouped semantics as convolution forward, strict
FP32 `1xKx1x1` bias descriptors, value-identical z/y descriptors, ReLU with
`CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM`, and identity with
`CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM`. It computes
`y = act(alpha1 * conv_or_correlation(x, w) + alpha2 * z + bias[k])`, supports
exact `z == y` by staging the original residual before output copy-back, rejects
partial z/y overlap and broader x/w/z/bias/y byte-range overlap, avoids reading
z when alpha2 is zero, accepts but ignores workspace, and defaults to CPU
reference execution. Required cuDNN Metal mode verifies a real fused MSL
shared-buffer route for the same subset, preferred Metal mode falls back only
for backend-availability failures, and
`PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required` can prefer an MPSGraph
`convolution2D` raw-convolution fast path plus an MSL epilogue for alpha1,
alpha2/z, bias, and ReLU/identity activation. Required-mode prepare, graph, or
epilogue failures leave caller y unchanged, while preferred mode falls back to
deterministic fused MSL. No fused MLX route is claimed yet.
Under the same explicit simulated-memory opt-in, `cudnnConvolutionBackwardData`
supports the matching bounded legacy 2D data-gradient subset for contiguous 4D
NCHW FP32 dy/dx tensors and contiguous FP32 NCHW/KCRS filters using the same
cuDNN grouped semantics, including depthwise and depthwise-multiplier cases.
The supported algorithm is deterministic zero-workspace
`CUDNN_CONVOLUTION_BWD_DATA_ALGO_1`; `ALGO_0`, FFT, Winograd, and other
alternates are not claimed for this FP32 NCHW path. The legacy backward-data
algorithm query helpers and workspace-size query validate the same bounded
descriptor configuration and report exactly one zero-workspace path. The
operation treats `dxDesc` as the corresponding forward input descriptor,
requires `dyDesc` to match the forward output shape computed from dx, w, and
convDesc, computes `dx = alpha * dconv_data(w, dy) + beta * prior_dx`, rejects
any w/dy/dx byte-range overlap while allowing adjacent non-overlap, avoids
reading prior dx when beta is zero, defaults to CPU reference execution, and
uses the same required/preferred Metal route for the same FP32 contiguous 4D
NCHW/KCRS grouped subset. With
`PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required`, that route can prefer MPSGraph
`convolution2DDataGradient` for the same descriptor subset. It reuses the same
MSL weight-prepare rule, writes raw dx internally through MPSGraph, and applies
alpha/beta through an MSL epilogue before CPU copy-back. Required mode leaves
caller dx unchanged on prepare, graph, or epilogue failure, while preferred mode
falls back to deterministic MSL.

`cudnnConvolutionBackwardFilter` now supports the matching bounded legacy 2D
filter-gradient subset for contiguous 4D NCHW FP32 x/dy tensors and contiguous
FP32 NCHW/KCRS dw filters using the same cuDNN grouped semantics, including
depthwise and depthwise-multiplier cases. The supported algorithm is
deterministic zero-workspace `CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1`; `ALGO_0`,
`ALGO_3`, FFT, Winograd, and other alternates are not claimed for this FP32
NCHW path. The legacy backward-filter algorithm query helpers and
workspace-size query validate the same bounded descriptor configuration and
report exactly one zero-workspace path. The operation treats `dwDesc` as the
corresponding forward filter descriptor, requires `dyDesc` to match the forward
output shape computed from x, dw, and convDesc, computes
`dw = alpha * dconv_filter(x, dy) + beta * prior_dw`, rejects any x/dy/dw
byte-range overlap while allowing adjacent non-overlap, avoids reading prior dw
when beta is zero, defaults to CPU reference execution, and uses the same
required/preferred Metal route for the same FP32 contiguous 4D NCHW/KCRS
grouped subset. For true convolution, the physical KCRS dw write slot is
unchanged while the sampled input tap is spatially flipped to match the
forward/backward-data convention. When
`PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required` is also set, the Metal route
tries an MPSGraph `convolution2DWeightsGradient` fast path for the same
NCHW/OIHW grouped FP32 descriptor subset, including depthwise,
depthwise-multiplier, asymmetric padding, and dilated true-convolution cases
covered by the regression harness. MPSGraph writes raw dW into an internal
shared buffer; an MSL epilogue then applies alpha/beta, beta-zero no-read
behavior, and the cuDNN true-convolution R/S flip into a second internal buffer
before CPU copy-back. `required` mode leaves caller `dw` unchanged on MPSGraph
or epilogue failure, while preferred mode falls back to the deterministic MSL
route. The baseline Metal route keeps the deterministic one-thread-per-dw
serial kernel as fallback/oracle; larger reduction spans use a private-scratch
tiled MSL path with fixed-order threadgroup partial sums and fixed chunk-order
final reduction before the same post-completion CPU copy-back. MPSGraph checks
use FP32 tolerance because Apple controls the reduction order; MSL remains the
bitwise deterministic oracle. The private scratch is internal, so the public
workspace query remains zero. Broader MPSGraph layouts/dtypes and descriptor
semantics, plus MLX/MSL rewrites for future gaps, remain implementation targets.
Fused or bias convolution, alternate algorithms,
`cudnnFindConvolutionForwardAlgorithmEx`, 5D/Nd tensors, NHWC, non-FP32
tensors, non-contiguous/custom strides, and real cuDNN async semantics remain
unsupported.
Under the same explicit simulated-memory opt-in, `cudnnTransformTensor` supports
the scaled-copy subset for the shim's current contiguous 4D NCHW FP32 tensor
descriptors: `y = alpha * x + beta * prior_y` with host alpha/beta scalars,
source/destination dimension and descriptor-value equality, no source/dest
overlap, beta-zero execution without reading prior y, CPU reference execution by
default, and the same required/preferred Metal route. This is not arbitrary
layout conversion yet; same-dimension transforms with different strides,
non-FP32 tensors, NHWC, tensor-transform descriptors, and `TransformTensorEx`
remain unsupported until the descriptor layer grows those layouts.
Under the same explicit simulated-memory opt-in, the cuDNN shim can also create
pooling descriptors and run a bounded `cudnnPoolingForward` /
`cudnnPoolingBackward` subset: contiguous 4D NCHW FP32 2D max pooling for
`CUDNN_POOLING_MAX` and
`CUDNN_POOLING_MAX_DETERMINISTIC`, plus average pooling for
`CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING` and
`CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING`, with
`cudnnGetPooling2dDescriptor`, `cudnnGetPooling2dForwardOutputDim`, host
alpha/beta scalars, explicit `CUDNN_PROPAGATE_NAN` /
`CUDNN_NOT_PROPAGATE_NAN` handling, CPU reference execution by default, and the
same required/preferred Metal route. Average include-padding uses the full
window denominator and zero-valued padding; average exclude-padding uses the
per-output in-bounds denominator. Average backward is geometry-based, permits
null x/y descriptors and data as cuDNN documents, and distributes dy using the
same include/exclude padding denominators without reading input values. Max
backward requires x/y descriptors and data, validates them, then recomputes
deterministic first-max selection from x using the same row-major scan and NaN
policy as forward; both max modes use the deterministic route. Backward honors
host alpha/beta scalars, avoids reading prior dx when beta is zero, and permits
exact `dy == dx` only when descriptors match and staging can preserve the
original dy/prior dx. The Metal backward path computes one dx element per thread
and avoids FP32 atomics for Apple-family portability; it is correctness-first,
not a broad performance claim. Exact `x == y` in-place forward pooling is
allowed only when tensor descriptor values match; partial forward overlap,
partial dy/dx backward overlap, max-backward x/dx or y/dx overlap, Nd pooling
descriptors, NHWC, non-FP32, and custom-stride tensors remain unsupported.
Under the same explicit simulated-memory opt-in,
`cudnnSoftmaxForward` and `cudnnSoftmaxBackward` support contiguous 4D NCHW
FP32 `CUDNN_SOFTMAX_FAST`, `CUDNN_SOFTMAX_ACCURATE`, and `CUDNN_SOFTMAX_LOG`
for `CUDNN_SOFTMAX_MODE_CHANNEL` and `CUDNN_SOFTMAX_MODE_INSTANCE`, with host
alpha/beta scalars and value-identical tensor descriptors. Forward FAST
remains a straightforward overflow-prone softmax; ACCURATE and LOG use
max-scaled softmax/log-softmax. Backward uses `y * (dy - sum(y * dy))` for
FAST/ACCURATE and `dy - exp(y) * sum(dy)` for LOG. The shim defines
deterministic whole-vector NaN propagation, stable forward positive-infinity
handling for ACCURATE/LOG, beta-zero execution that does not read prior output
storage, CPU reference execution by default, exact `x == y` forward staging,
exact `dy == dx` backward staging, and the same required/preferred Metal route
using one cooperative 256-lane threadgroup per softmax vector. Partial x/y,
dy/dx, or y/dx overlap, 5D tensors, NHWC, non-FP32, and custom-stride tensors
remain unsupported.
Under the same explicit simulated-memory opt-in, the `libcublas` shim can create metadata handles, run
bounded FP32/FP64 real Level-1 `cublas<t>axpy`, `cublas<t>copy`, `cublas<t>dot`,
`cublas<t>scal`, `cublas<t>rot`, `cublas<t>rotg`, `cublas<t>rotm`, `cublas<t>rotmg`, `cublas<t>swap`, `cublas<t>asum`, `cublas<t>nrm2`,
`cublasI<t>amax`, and `cublasI<t>amin`, run bounded FP32/FP64 complex Level-1
`cublasC/Zaxpy`, `cublasC/Zcopy`, `cublasC/Zdotu`, `cublasC/Zdotc`,
`cublasC/Zscal`, `cublasC/Zswap`, `cublasCsscal`, and `cublasZdscal`,
run FP32/FP64 complex `cublasC/Zgemv`, `cublasC/Zgeru`, `cublasC/Zgerc`,
`cublasC/Zhemv`, `cublasC/Zher`, `cublasC/Zher2`, `cublasC/Zherk`, `cublasC/Zher2k`, `cublasC/Ztrmv`, `cublasC/Ztrsv`, `cublasC/Ztrmm`, and `cublasC/Ztrsm`,
run FP32/FP64 `cublas<t>gemv`,
`cublas<t>ger`, `cublas<t>symv`, `cublas<t>syr`, `cublas<t>syr2`,
`cublas<t>trmv`, `cublas<t>trsv`, `cublas<t>trmm`, `cublas<t>trsm`,
`cublas<t>symm`, `cublas<t>syrk`, and `cublas<t>syr2k`, and run
`cublasS/D/C/Zgemm[_v2]` plus
`cublasS/D/C/ZgemmBatched` and
`cublasS/D/C/ZgemmStridedBatched` as real
column-major CPU math over
host-accessible CPU pointers, including CPU-backed simulated runtime pointers.
On Darwin, real/complex `cublasS/D/C/Zgemm[_v2]`,
`cublasS/D/C/ZgemmBatched` and `cublasS/D/C/ZgemmStridedBatched` batch entries, and
real/complex TRMM/TRSM route through Accelerate/vecLib CBLAS after Psyche's
cuBLAS-shaped validation. GEMM keeps temporary output staging and alpha-zero /
`k == 0` no-read fallbacks; TRMM keeps alpha-zero no-read guards and
`B`-to-`C` staging. Non-Darwin builds keep the reference-loop implementation.
On Apple Silicon, `PSYCHE_CUDA_COMPAT_CUBLAS_METAL=required` verifies a real
Metal shared-buffer dispatch for contiguous FP32 `cublasSaxpy[_v2]` and
`cublasSscal[_v2]`, plus contiguous FP32 `cublasScopy[_v2]` and
`cublasSdot[_v2]`, `cublasSasum[_v2]`, `cublasSnrm2[_v2]`, plus
nonzero signed-stride FP32 `cublasSgemv[_v2]` and `cublasSger[_v2]`;
`PSYCHE_CUDA_COMPAT_CUBLAS_METAL=1` prefers that Metal path and falls back to
the CPU reference path for fallback-eligible backend errors;
`PSYCHE_CUDA_COMPAT_CUBLAS_METAL=required` returns the Metal-derived status
instead of falling back. For these Metal
routes, contiguous means `incx == 1 && incy == 1` for SAXPY/SCOPY/SDOT and
`incx == 1` for SSCAL/SASUM/SNRM2; SGEMV and SGER stage signed-strided
`x` and `y` into compact host buffers before dispatch, use Netlib/cuBLAS
negative-increment logical indexing for nonzero signed increments, reject zero
increments, and SGEMV scatters compact Metal output back to strided `y` only
after successful command-buffer completion. Strided FP32 AXPY, SSCAL, SCOPY, SDOT, SASUM, and SNRM2 remain CPU-backed, and required Metal mode returns
`CUBLAS_STATUS_NOT_SUPPORTED` for those shapes instead of falling back.
GEMV, GER, HEMV, HER, HER2, SYMV, SYR, and SYR2 reject zero increments, but nonzero negative
increments use signed logical vector order on their CPU-backed paths.
`cublasSaxpy[_v2]` and `cublasScopy[_v2]` allow exact
`x == y` aliasing in the contiguous Metal shim path, but required Metal mode
rejects partially overlapping source/destination ranges instead of promising
CPU-loop overlap behavior.
The Metal `cublasSgemv[_v2]` route stages `A`, compact `x`, and compact old
`y`, computes into a separate compact output buffer, and copies or scatters `y`
back only after command-buffer completion, so tested exact and partial host
overlaps follow the CPU temp-output semantics.
The Metal-backed cuBLAS path launches a real Metal compute kernel over shared
Apple Silicon memory; it does not implement CUDA device memory, CUDA UVA pointer
provenance, CUDA stream semantics, or `torch.cuda` availability.
The cuBLAS Metal routes copy host-accessible inputs and mutable spans into
Metal shared buffers, then copy mutated outputs or scalar results back after
command-buffer completion. The Metal SGEMV route is a baseline
one-thread-per-output kernel with serial accumulation over each output element,
and the Metal SGER route is a baseline one-thread-per-matrix-entry rank-1
update bounded to 32-bit logical update and staged-A element counts. Neither is
a tuned/tiled cuBLAS implementation. The Metal SDOT, SASUM, and SNRM2 routes use
parallel reduction order, so low-order floating-point bits can differ from the
CPU fallback. The Metal SNRM2 route uses a stable
`scale`/`ssq` pair reduction to avoid naive square overflow/underflow, but it is
not bitwise NVIDIA cuBLAS parity.
It also exposes `cublasSetVector`, `cublasGetVector`, `cublasSetMatrix`,
`cublasGetMatrix`, and their async variants as synchronous host-accessible
byte-span copies. Async transfer helper stream arguments are metadata only.
cuBLAS version/property helpers return `0`, and a successful shim handle is not
a claim that NVIDIA cuBLAS exists. Stream handles are stored and returned as
metadata only; the shim does not validate them against the driver/runtime stream
registries or synchronize work through them. The shim does not claim same-handle
thread safety; callers must serialize concurrent handle mutation, destruction,
and operation calls themselves. Pointer-mode, math-mode, and
atomics-mode state is metadata only: device pointer-mode scalars/results and
tensor/TF32 math modes can be stored, but operations that require host
scalar/result pointers, including AXPY, DOT/DOTU/DOTC, SCAL, ROT, ROTG, ROTM, ROTMG, reductions, GEMV, GER,
HEMV, HER, HER2, HERK, HER2K, SYMV, SYR, SYR2, TRMM, TRSM, SYMM, SYRK, SYR2K, and GEMM, return
`CUBLAS_STATUS_NOT_SUPPORTED` for device pointer mode; scalar-free COPY, SWAP,
TRMV, and TRSV are not blocked by pointer mode.
Mutating Level-1 vector ops, DOT/DOTU/DOTC, ROT, and ROTM model
positive strides only when work is required; GEMV, GER, HEMV, HER, HER2, SYMV, SYR, SYR2, TRMV, and TRSV
support nonzero signed strides; DOT/DOTU/DOTC write zero for `n <= 0` after result-pointer validation; DOTC conjugates the first complex vector; Csscal/Zdscal apply real scalar factors to complex vectors; complex GEMV/TRMV/TRSV honor `CUBLAS_OP_C` as conjugate transpose; complex GERU uses `y` as-is; complex GERC conjugates `y`; complex HEMV reads Hermitian diagonals as real without mutating `A`; complex HER/HER2/HERK/HER2K update only the stored triangle and force updated diagonal imaginary parts to zero; complex HERK/HER2K accept `CUBLAS_OP_T` as non-conjugate transpose and avoid reading `C` when `beta == 0` and product inputs when `alpha == 0` or `k == 0`; complex GEMM honors `CUBLAS_OP_C` as conjugate transpose; ASUM/NRM2/IAMAX/IAMIN return
zero for `n <= 0` or `incx <= 0`. Transfer helpers require positive element
sizes, vector strides, and matrix leading dimensions; matrix transfer leading
dimensions must be at least `rows` when `rows > 0`; zero-work transfer calls may
omit source/destination pointers; source bytes are staged so host-overlapping
copies are deterministic; and disabled simulated-memory transfer calls return
`CUBLAS_STATUS_NOT_INITIALIZED` without mutating destination buffers. SYRK/SYR2K update only the requested stored
`C` triangle and leave the opposite triangle untouched; complex HERK/HER2K follow the same stored-triangle rule; `beta == 0` avoids
reading `C` input storage, and `alpha == 0` or `k == 0` avoids reading product
inputs where cuBLAS permits. ROT stages original `x`/`y` inputs before writes
but does not guarantee arbitrary overlapping output vector semantics. ROTG
constructs FP32/FP64 host scalar Givens parameters and overwrites
`a`/`b`/`c`/`s` as `r`/`z`/`c`/`s` using Netlib-compatible rules, but it does
not guarantee arbitrary aliasing among the scalar output pointers. Except for
the opt-in contiguous FP32 `cublasSaxpy[_v2]`, `cublasSscal[_v2]`, and
`cublasScopy[_v2]`, plus `cublasSdot[_v2]`, `cublasSasum[_v2]`,
`cublasSnrm2[_v2]`, signed-stride `cublasSgemv[_v2]`, and signed-stride `cublasSger[_v2]` Metal
routes, the CPU accumulation path is not bitwise-equivalent to NVIDIA cuBLAS. The shim does not validate arbitrary
pointer provenance; callers must pass valid host-accessible CPU buffers large
enough for the requested shapes, strides, and byte spans. SYMM/SYRK/SYR2K/HERK/HER2K do not guarantee
arbitrary overlap handling. Non-`_v2` cuBLAS symbols use the current handle-based
cuBLAS ABI shape as aliases for the matching `_v2` implementations; legacy
cuBLAS v1 by-value / no-handle ABI is not modeled.
Under the same explicit simulated-memory opt-in, the `libcublasLt` shim can
create/destroy handles, initialize and allocate opaque matmul descriptors,
matrix layouts, preferences, and algorithm records, store and retrieve the
supported descriptor attributes, return one zero-workspace heuristic for
supported cases, and run real FP32/FP64 `cublasLtMatmul` with
`CUBLASLT_ORDER_COL` and `CUBLASLT_ORDER_ROW` matrix layouts over
host-accessible CPU pointers. The matmul path supports `CUBLAS_OP_N`,
`CUBLAS_OP_T`, and real-valued `CUBLAS_OP_C` as transpose, distinct `D` and
`C` buffers, `beta == 0` without reading `C`, null `C` data for that beta-zero
case when the descriptor is valid, compatible strided-batch layouts, and
`CUBLASLT_EPILOGUE_DEFAULT` / `CUBLASLT_EPILOGUE_RELU` /
`CUBLASLT_EPILOGUE_RELU_AUX` / `CUBLASLT_EPILOGUE_BIAS` /
`CUBLASLT_EPILOGUE_RELU_BIAS` / `CUBLASLT_EPILOGUE_RELU_AUX_BIAS` /
`CUBLASLT_EPILOGUE_DRELU` / `CUBLASLT_EPILOGUE_DRELU_BGRAD` /
`CUBLASLT_EPILOGUE_GELU` / `CUBLASLT_EPILOGUE_GELU_BIAS` /
`CUBLASLT_EPILOGUE_GELU_AUX` / `CUBLASLT_EPILOGUE_GELU_AUX_BIAS` /
`CUBLASLT_EPILOGUE_DGELU` / `CUBLASLT_EPILOGUE_DGELU_BGRAD`.
BIAS-bearing epilogues read a caller-owned CPU-resident host-accessible bias
vector with dtype unset/default or matching `D`, length equal to `D.rows`, and
optional `CUBLASLT_MATMUL_DESC_BIAS_BATCH_STRIDE` in elements; stride zero
broadcasts the same bias vector across batches, while positive strides select a
per-batch vector and must be at least `D.rows`. Bias is applied as `bias[row]`
after `alpha*A@B + beta*C` and before optional ReLU or GELU. `RELU_AUX` and
`RELU_AUX_BIAS` write a ReLU bit-mask AUX buffer using logical bit index
`row + col * AUX_LD`; NVIDIA documents ReLU-mask `AUX_LD` and positive
`AUX_BATCH_STRIDE` values as bits, divisible by 128, with `AUX_LD >= D.rows`,
so the shim's overlap span covers `AUX_LD * D.cols` bits. Within each byte the
host bridge uses an LSB-first convention covered by fixed-byte tests but still
needing a real NVIDIA AUX-buffer diff before claiming bit-for-bit interchange
with cuBLASLt. TODO: replace this convention note with a real NVIDIA AUX-buffer
byte diff once one is available. `DRELU` and `DRELU_BGRAD` read that same bit-mask and write
`raw_dy` where the mask bit is set, otherwise zero. Following cuBLASLt's
"apply independently ReLu and Bias gradient to matmul output" wording,
`DRELU_BGRAD` writes an independent raw-dy row-wise bias-gradient output, with
FP32 reductions accumulated in FP64. ReLU mask epilogues reject non-default AUX
data types because the AUX buffer is a bit mask, not a typed matrix. `GELU_AUX` and
`GELU_AUX_BIAS` write the pre-GELU logical output matrix to a caller-owned
CPU-resident host-accessible AUX buffer after optional bias and before GELU,
using column-major AUX indexing with `AUX_LD` in elements. `DGELU` and
`DGELU_BGRAD` read the same logical column-major AUX matrix as the saved GELU
preactivation input, apply the derivative of the documented tanh GELU
approximation to `alpha*A@B + beta*C`, and write the result to `D`.
`DGELU_BGRAD` also writes a bias-gradient vector of length `D.rows` where
`bias_gradient[row] = sum_col raw_dy[row,col]`, before the DGELU multiply.
FP32 bias-gradient reductions accumulate in FP64 before storing the FP32 output.
Multi-batch DGELU_BGRAD requires a positive bias stride at least `D.rows`;
stride-zero broadcast is only accepted for bias input epilogues. The DGELU
derivative is gradient-consistent with this shim's tanh-approximation
GELU/GELU_AUX paths, not with an exact-erf GELU implementation such as PyTorch's
default eager GELU. AUX dtype must be unset/default or match `D`, `AUX_LD` must
be divisible by 8 elements, at least `D.rows`, and within the backend indexing
ceiling, positive
`AUX_BATCH_STRIDE` values are in elements and must cover `AUX_LD * D.cols`, and
zero AUX stride is rejected for multi-batch AUX writes or reads that would alias
per-batch state. Execution rejects runtime AUX/D, D/bias-gradient, and
AUX/bias-gradient range overlap before writing output buffers. Row-major `D`
support for these epilogues is an intentional shim compatibility extension
beyond NVIDIA's documented row-major restriction.
ReLU clamps each logical output with CUDA-style
`value > 0 ? value : 0` semantics, while GELU applies NVIDIA's documented tanh
approximation before the `D` write. GELU propagates NaN, maps `+Inf` to `+Inf`,
and maps `-Inf` to `0` in this bounded reference path. Layout order, epilogue,
bias-data-type, and aux-data-type changes validate before mutating descriptor
state. BIAS/AUX pointers are validated for non-null state and element-size
alignment, but not lifetime, bounds, or arbitrary CUDA/UVA pointer provenance
beyond the AUX/D overlap check.
On Darwin, all-column-major supported cases use Accelerate/vecLib CBLAS for the
raw GEMM core after cuBLASLt-shaped validation; cuBLASLt epilogues are applied
afterward by CPU postprocessing. Row-major or mixed-order layouts use the CPU
reference-loop GEMM core.
Non-Darwin builds keep the reference-loop implementation. Streams and
workspaces are accepted as metadata only. Pointer-array batch layout mode is
supported for DEFAULT-only matmuls when A/B/C/D descriptors all use
`CUBLASLT_BATCH_MODE_POINTER_ARRAY` with matching batch counts and
CPU-addressable pointer arrays; strided batch offsets are ignored in that mode,
required null entries are rejected before any `D` batch is written, and
non-DEFAULT pointer-array epilogues return no heuristic and fail execution as
not-supported.
`cublasLtMatrixTransform` and its transform descriptor APIs now support a
bounded host bridge for real FP32/FP64 `CUBLASLT_ORDER_COL` and
`CUBLASLT_ORDER_ROW` layouts: host pointer-mode alpha/beta scalars, FP32 or
FP64 scale type with input conversion to scale type and output conversion to
C's data type, `CUBLAS_OP_N`, `CUBLAS_OP_T`, and real `CUBLAS_OP_C` as
transpose, alpha-zero and beta-zero no-read behavior, strided batches with
input batch-count-one broadcast to C's batch count, and pointer-array batches
when participating A/B/C descriptors all use pointer-array mode with exact batch
counts. Pointer-array entries required by nonzero alpha/beta and output writes
are preflighted before any C batch is mutated. Unsafe source/C byte-range
overlap is rejected, while exact same-layout no-transpose in-place sources are
allowed. Validation and preflight rejection paths leave C unchanged; successful
CPU arithmetic writes C elementwise and does not promise rollback for ordinary
floating-point NaN/Inf/overflow results. Device pointer-mode transform
descriptors preserve the attribute for ABI/config compatibility only, but
execution returns not-supported.
Tiled layouts, half/BF16/FP8/complex/int
data types, tensor-core and TF32 modes, device/vector pointer modes,
AUX scale/amax outputs, grouped batches, real CUDA
async semantics, and device-resident pointer provenance remain unsupported.
Any future complex MatrixTransform bridge must implement `CUBLAS_OP_C` as a
true conjugating transpose rather than reusing the current real-type transpose
equivalence.
Under the same explicit simulated-memory opt-in, the `libcusparse` shim can
create/destroy handles, store stream and pointer-mode metadata, create/destroy
CSR sparse-matrix descriptors, dense-vector descriptors, and dense-matrix
descriptors, return status/version/property helpers, and run bounded
`cusparseSpMV_bufferSize` / `cusparseSpMV` and `cusparseSpMM_bufferSize` /
`cusparseSpMM` subsets. The SpMV executable subset is FP32 CSR SpMV over
host-accessible CPU pointers: `CUSPARSE_OPERATION_NON_TRANSPOSE`, `CUDA_R_32F`
matrix/vector/compute types, matching 32-bit or 64-bit row and column indices
on the CPU route, zero-based or one-based CSR, host alpha/beta scalars, and
`CUSPARSE_SPMV_ALG_DEFAULT`, `CUSPARSE_SPMV_CSR_ALG1`, or
`CUSPARSE_SPMV_CSR_ALG2`. The workspace query
returns zero bytes because the shim uses internal staging. The operation
validates descriptor lifetimes, dimensions, row-offset monotonicity and
endpoints, column ranges, alignment, and output/source overlap before mutating
`y`; the CPU path computes into a temporary output buffer and copies back only
after validation and execution succeed. On Apple Silicon,
`PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=required` verifies a real Metal
shared-buffer CSR SpMV route for that same subset, while
`PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=1` prefers Metal with CPU fallback only for
backend-availability failures. The Metal route currently requires 32-bit row
and column indices, stages CSR, `x`, and prior `y` into shared buffers,
computes one output row per thread into a separate output buffer, and copies
`y` back only after command-buffer completion. In required-Metal mode, 64-bit
CSR indices return not-supported without CPU fallback until the local Metal
toolchain can prove an MSL 64-bit-index kernel. The SpMM executable subset is
FP32 CSR SpMM with `opA == opB == CUSPARSE_OPERATION_NON_TRANSPOSE`,
`CUDA_R_32F` sparse/dense/compute types, matching 32-bit or 64-bit row and
column indices, zero-based or one-based CSR, host alpha/beta scalars,
`CUSPARSE_ORDER_COL` or `CUSPARSE_ORDER_ROW` dense B/C matrices with
leading-dimension validation, and `CUSPARSE_SPMM_ALG_DEFAULT`,
`CUSPARSE_SPMM_CSR_ALG1`, `CUSPARSE_SPMM_CSR_ALG2`, or
`CUSPARSE_SPMM_CSR_ALG3`. Its workspace query also returns zero bytes and
validates descriptor/contract metadata; in required-Metal mode it also applies
the 32-bit-index/uint-limit Metal supportability preflight, but it intentionally
does not read CSR row/column contents. The CPU SpMM path validates CSR indices, dense layouts,
dimensions, B/C aliasing, and C overlap with CSR storage before writing; it
computes into a temporary logical C buffer, copies back only after success, and
does not read prior C when beta is zero. On Apple Silicon,
`PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=required` verifies a real Metal
shared-buffer CSR SpMM route for the 32-bit-index subset, stages CSR, `B`, and
prior `C` when beta is nonzero, computes compact logical `C` into a separate
shared buffer, and copies `C` back only after command-buffer completion.
`PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=1` prefers Metal and falls back to the CPU
reference path only for backend-availability failures. In required-Metal mode,
64-bit CSR indices return not-supported without CPU fallback until the local
Metal toolchain can prove an MSL 64-bit-index kernel. Device pointer-mode scalars, transpose/conjugate transpose,
non-CSR formats, 16-bit indices, non-FP32/complex/low-precision values,
preprocess and update APIs, broader SpMM formats/algorithms, SpSV/SpSM,
sparse/dense conversions, batched sparse APIs, external workspace semantics,
CUDA streams, CUDA graphs, and asynchronous behavior are not modeled.
Under the same explicit simulated-memory opt-in, `libcusolver` can create and
destroy cuSolverDN handles, store stream metadata, report version/property/status
helpers, and run bounded dense FP32/FP64 `cusolverDnSgetrf` /
`cusolverDnDgetrf` plus `cusolverDnSgetrs` / `cusolverDnDgetrs` paths over
host-accessible column-major pointers. On Darwin, pivoted LU/solve routes
through Accelerate/LAPACK and preserves cuSOLVER/LAPACK-style 1-based pivots;
singular factors return success with positive `devInfo`. Singularity detection
follows exact-zero pivot signaling; this shim does not estimate conditioning or
warn on near-singular matrices. Non-Darwin builds use a deterministic CPU
reference partial-pivot LU/solve for the same bounded subset. `devIpiv == NULL`
is treated as cuSOLVER's documented no-pivot
factorization/solve mode and uses a deterministic reference triangular solve.
If no-pivot `getrs` encounters a zero diagonal, it
returns `CUSOLVER_STATUS_EXECUTION_FAILED` with positive `devInfo` and leaves B
unchanged; `getrs` does not inherit `getrf`'s positive-success singular
contract. `getrf_bufferSize` returns `m * n` elements and
`getrf` validates a non-null workspace when work is required, even though
Accelerate uses its own internal storage. Mutable A/B operands are staged so
validation, allocation, failed no-pivot solves, or required-backend failures
leave caller buffers unchanged. The same opt-in path now covers bounded dense
FP32/FP64 Cholesky `cusolverDnS/Dpotrf_bufferSize`,
`cusolverDnS/Dpotrf`, and `cusolverDnS/Dpotrs` over host-accessible
column-major pointers. `potrf_bufferSize` returns a conservative `n * n`
element workspace; `potrf` validates that workspace even though Accelerate uses
internal storage. On Darwin, `potrf`/`potrs` route through Accelerate/LAPACK
after explicit lower/upper translation; non-Darwin builds use deterministic CPU
reference Cholesky and triangular solves. Only the requested lower or upper
triangle is referenced, the opposite triangle is left untouched, and positive
non-positive-definite `potrf` `devInfo` leaves caller A unchanged because
neither cuSOLVER nor LAPACK guarantees useful partial factors. `potrs` stages
B so failed solves leave it unchanged; the exact-zero diagonal `potrs` precheck
is a shim-safety behavior, not a real cuSOLVER guarantee. Device pointers, async execution, CUDA
stream semantics beyond metadata, CUDA graphs, batched dense solvers, sparse
cuSolverSP/cuDSS-style solves, QR, eigen, SVD, IRS, RF/Mg, complex and
low-precision data types, real GPU residency, and bitwise NVIDIA parity are not
modeled.
Arbitrary real CUDA/UVA/foreign pointers, tensor cores, CUDA kernels,
cuBLASLt advanced layouts/epilogues/tensor modes/low-precision paths,
complex Level-2 outside GEMV/GERU/GERC/HEMV/HER/HER2/TRMV/TRSV and complex Level-3 outside
GEMM/HERK/HER2K/TRMM/TRSM paths, half/TF32 paths, and Apple GPU execution outside the opt-in
contiguous FP32 `cublasSaxpy[_v2]`, `cublasSscal[_v2]`, and `cublasScopy[_v2]`
plus `cublasSdot[_v2]`, `cublasSasum[_v2]`, `cublasSnrm2[_v2]`, and
signed-stride `cublasSgemv[_v2]`, and signed-stride `cublasSger[_v2]` Metal routes are not modeled. This is useful for packages
that check whether CUDA libraries exist, allocate scratch buffers, create
streams/events, and then decide whether deeper CUDA work is possible, but it is
deliberately not enough for packages that need real CUDA execution.

Do not put the shim output directory on the library search path for real
training. The libraries use canonical CUDA names and are meant only for
controlled Apple Silicon discovery tests; they must not shadow a real CUDA
installation.

Check it with:

```bash
python3 scripts/check-cuda-driver-stubs.py
```

Escalated compatibility attempts / still unsupported arbitrary forms:

- PTX, cubins, `cuModuleLoad` files, arbitrary `cuModuleLoadData` blobs,
  arbitrary `cuLaunchKernel` functions, and arbitrary `cudaLaunchKernel`
  function tokens outside the explicit Psyche runtime-token subset;
- Triton CUDA kernels and NVRTC/ptxas flows;
- CUDA extensions, cuBLAS outside the narrow CPU-backed transfer-helper/bounded real Level-1/bounded complex Level-1/complex-GEMV-GERU-GERC-HEMV-HER-HER2-TRMV-TRSV-HERK-HER2K/GEMV/GER/SYMV/SYR/SYR2/TRMV/TRSV/SYMM/SYRK/SYR2K/real-and-complex-GEMM shim plus Darwin Accelerate-backed real/complex GEMM/TRMM/TRSM and real/complex GEMM batched/strided-batched batch entries,
  cuBLASLt advanced layouts/epilogues/tensor modes/low-precision paths,
  cuSPARSE outside bounded FP32 CSR SpMV/SpMM, cuSOLVER outside dense S/D
  `getrf`/`getrs`, real cuDNN operations, NCCL, real NVML per-device telemetry and Apple
  hardware mapping, GPUDirect, IPC, peer access;
- CUDA Graphs, stream capture, cooperative groups, textures, and surfaces.

## Truth Ledger

The CUDA compatibility surface is tracked by a generated truth ledger:

```bash
python3 scripts/report-cuda-compat-coverage.py --check --json-out target/cuda-compat-coverage.json --markdown-out target/cuda-compat-coverage.md
```

The report is source-backed and machine-checkable. It loads the MPS
compatibility route manifest from `python/python/psyche/mps_compat.py`, verifies
that the manifest matches the actual compatibility installer, parses every
`PSYCHE_CUDA_STUB_API` export in `tools/cuda-compat-shim/libcuda_stub.c`,
`tools/cuda-compat-shim/libcudart_stub.c`,
`tools/cuda-compat-shim/libcublas_stub.c`, and
`tools/cuda-compat-shim/libcublasLt_stub.c`, and
`tools/cuda-compat-shim/libcusparse_stub.c`, and
`tools/cuda-compat-shim/libcusolver_stub.c`, and
`tools/cuda-compat-shim/libnvidia_ml_stub.c`, and
`tools/cuda-compat-shim/libcudnn_stub.c`, verifies that every native
CUDA-, NVML-, cuSPARSE-, cuSOLVER-, or cuDNN-shaped symbol is classified in the manifest, verifies that
`cuGetProcAddress` exposes only discovery/proc-policy symbols, and emits both
JSON and Markdown reports under `target/`.

Support levels are deliberately narrow:

- `python_redirect_to_mps`: Psyche-owned CUDA-shaped intent redirects to a real
  MPS device under explicit opt-in.
- `mps_exact_route`: an exact ATen route is registered at the MPS dispatch key.
- `native_discovery_stub`: a native CUDA-shaped symbol exists for probing or
  linkage but reports no usable CUDA device.
- `native_hard_fail_stub`: a native CUDA-shaped symbol exists but rejects
  unsupported module-file loading, device/context-wide synchronization,
  priority-range queries, or other unsupported runtime work.
- `native_nvml_discovery_stub`: a native NVML-shaped `libnvidia-ml` symbol is
  available for dependency probes. The compatibility library initializes with
  NVML refcount semantics, exposes parseable stub versions and error strings,
  reports zero NVIDIA devices, keeps PyTorch's NVML-based CUDA availability
  probe on the false/no-device path, and rejects handle lookup or telemetry
  instead of reporting Apple GPU identity or metrics as NVIDIA telemetry.
- `native_cudnn_discovery_stub`: a native cuDNN-shaped `libcudnn` symbol is
  available for dependency probes. Version/property/error-string helpers return
  zero/stub discovery values; by default `cudnnCreate` clears the output handle
  and returns not-initialized. Under simulated-memory opt-in, it mints metadata
  handles only for the bounded simulated cuDNN subset.
- `native_simulated_cudnn_activation_op`: cuDNN tensor/activation descriptors
  and `cudnnActivationForward`/`cudnnActivationBackward` run for contiguous 4D NCHW FP32
  ReLU/Sigmoid/Tanh/Identity under explicit opt-in, with an Apple Silicon Metal route
  gated by `PSYCHE_CUDA_COMPAT_CUDNN_METAL`.
- `native_simulated_cudnn_add_op`: `cudnnAddTensor` runs same-shape and
  broadcasted dense 4D NCHW FP32 adds, including `1xCx1x1` bias into
  `NxCxHxW`, under explicit opt-in with the same Apple Silicon Metal route.
- `native_simulated_cudnn_batchnorm_inference_op`:
  `cudnnBatchNormalizationForwardInference` runs the bounded 4D NCHW FP32
  legacy inference subset for `CUDNN_BATCHNORM_SPATIAL` and
  `CUDNN_BATCHNORM_PER_ACTIVATION`, under explicit opt-in with the same Apple
  Silicon Metal route.
- `native_simulated_cudnn_convolution_forward_op`: cuDNN filter/convolution
  descriptors, `cudnnGetConvolution2dForwardOutputDim`, forward algorithm
  query helpers, forward workspace-size query, and `cudnnConvolutionForward`
  run the bounded 4D NCHW FP32 x/y plus FP32 KCRS filter 2D forward subset for
  cuDNN grouped semantics (`groupCount > 0`, `input_C % groupCount == 0`,
  `K % groupCount == 0`, and filter `C = input_C/groupCount`), including
  depthwise and depthwise-multiplier cases, with deterministic zero-workspace
  `IMPLICIT_GEMM` under explicit opt-in with the same Apple Silicon Metal route,
  plus an optional `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required` MPSGraph
  `convolution2D` fast path with deterministic MSL fallback/oracle.
- `native_simulated_cudnn_convolution_bias_activation_forward_op`:
  `cudnnConvolutionBiasActivationForward` runs the bounded 4D NCHW FP32 fused
  convolution, residual z, bias, and ReLU/identity activation subset under
  explicit opt-in. CPU reference execution remains the default, while
  `PSYCHE_CUDA_COMPAT_CUDNN_METAL=required` verifies a real fused MSL route for
  the same subset and `PSYCHE_CUDA_COMPAT_CUDNN_METAL=1` prefers Metal with CPU
  fallback only for backend-availability failures. With
  `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required`, that Metal route can prefer
  MPSGraph raw convolution plus an MSL fused epilogue, with deterministic fused
  MSL fallback in preferred mode.
- `native_simulated_cudnn_convolution_backward_data_op`: backward-data
  algorithm query helpers, backward-data workspace-size query, and
  `cudnnConvolutionBackwardData` run the matching bounded 4D NCHW FP32 dy/dx
  plus FP32 KCRS filter 2D data-gradient subset for the same cuDNN grouped
  semantics, including depthwise and depthwise-multiplier cases, with
  deterministic zero-workspace `CUDNN_CONVOLUTION_BWD_DATA_ALGO_1` under
  explicit opt-in with the same Apple Silicon Metal route, plus an optional
  `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required` MPSGraph
  `convolution2DDataGradient` fast path with deterministic MSL fallback/oracle.
- `native_simulated_cudnn_convolution_backward_filter_op`: backward-filter
  algorithm query helpers, backward-filter workspace-size query, and
  `cudnnConvolutionBackwardFilter` run the matching bounded 4D NCHW FP32 x/dy
  plus FP32 KCRS dw 2D filter-gradient subset for the same cuDNN grouped
  semantics, including depthwise and depthwise-multiplier cases, with
  deterministic zero-workspace `CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1` under
  explicit opt-in with the same Apple Silicon Metal route, including private
  tiled-MSL partial reductions for larger `N*outH*outW` spans and an optional
  `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required` MPSGraph weights-gradient
  fast path with deterministic MSL fallback/oracle.
- `native_simulated_cudnn_transform_op`: `cudnnTransformTensor` runs the
  scaled-copy subset for contiguous 4D NCHW FP32 tensors under explicit opt-in,
  with no source/destination overlap and the same Apple Silicon Metal route.
- `native_simulated_cudnn_pooling_op`: cuDNN pooling descriptors,
  `cudnnGetPooling2dForwardOutputDim`, `cudnnPoolingForward`, and
  `cudnnPoolingBackward` run for contiguous 4D NCHW FP32 2D max and average
  pooling under explicit opt-in, with the same Apple Silicon Metal route and
  explicit NaN propagation handling.
- `native_simulated_cudnn_softmax_op`: `cudnnSoftmaxForward` and
  `cudnnSoftmaxBackward` run for contiguous 4D NCHW FP32 FAST/ACCURATE/LOG
  softmax in CHANNEL and INSTANCE modes under explicit opt-in, with CPU
  reference execution by default and the same Apple Silicon Metal route.
- `native_simulated_driver_memory_op`: a native CUDA Driver API
  allocation/pitched-allocation/managed-allocation/advice/prefetch/range-query/
  pointer-query/copy/2D-copy/3D-copy/fill/2D-fill/pinned-host-memory symbol is
  bounded to CPU memory under explicit opt-in.
  Async forms run synchronously on the null/zero stream or a shim-created
  stream.
- `native_simulated_driver_kernel_op`: native CUDA Driver API
  module/function/launch symbols accept a Psyche-native module blob and run
  registered `vector_add_f32`, `saxpy_f32`, `scale_f32`, and `axpby_f32`
  kernels over simulated driver allocations under explicit opt-in. The default
  path is CPU reference execution; Apple Silicon `PSYCHE_CUDA_COMPAT_METAL_KERNELS=required` verifies
  real Metal shared-buffer dispatch for those registered kernels through
  copy-in/copy-back staged buffers. Exact aliases are supported for tested
  in-place/output cases, and partial overlaps involving a mutated span fall back
  to CPU in preferred Metal mode or return unsupported in required Metal mode.
  Raw PTX/CUBIN, arbitrary kernels, multidimensional launches, dynamic shared
  memory, extra launch config, real contexts, and general CUDA execution are not
  modeled yet.
- `native_simulated_driver_mempool_op`: a native CUDA Driver API
  stream-ordered allocation/free or host/no-location memory-pool symbol is
  bounded to CPU memory under explicit opt-in. Device-default pool APIs reject
  absent CUDA devices; IPC/export/access-control paths are exported but
  unsupported; and no CUDA context, graph allocator, or GPU-resident pool is
  reported.
- `native_simulated_driver_sync_op`: a native CUDA Driver API
  stream/event/query/synchronization symbol is a host-side shim object under
  explicit opt-in. Streams are registry-validated metadata handles, events are
  CPU monotonic-time markers, and no CUDA device, real context, module, kernel,
  graph, or GPU timing support is reported.
- `native_simulated_runtime_memory_op`: a native CUDA runtime-shaped
  allocation/pitched-allocation/managed-allocation/advice/prefetch/range-query/
  pointer-query/copy/2D-copy/3D-copy/fill/2D-fill/pinned-host-memory symbol is
  bounded to CPU memory. No CUDA device is reported;
  `cudaMemcpy(..., cudaMemcpyHostToHost)`, `cudaMemcpy2D(...,
  cudaMemcpyHostToHost)`, linear `cudaMemcpy3D(..., cudaMemcpyHostToHost)`,
  and their null-stream async forms can run by default;
  async forms run synchronously on the null/zero stream or a shim-created stream;
  managed-memory migration is not modeled; and
  device-direction memory operations require
  `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY` to be set to `1`, `true`, `yes`, or
  `on`.
- `native_simulated_runtime_mempool_op`: a native CUDA Runtime API
  stream-ordered allocation/free or host/no-location memory-pool symbol is
  bounded to CPU memory under explicit opt-in. Device-default pool APIs reject
  absent CUDA devices; IPC/export/access-control paths are exported but
  unsupported; and no CUDA context, graph allocator, or GPU-resident pool is
  reported.
- `native_simulated_runtime_sync_op`: a native CUDA runtime
  stream/event/query/synchronization symbol is a host-side shim object under
  explicit opt-in. Streams are registry-validated metadata handles, events are
  CPU monotonic-time markers, and no CUDA device, kernel, graph, or GPU timing
  support is reported.
- `native_simulated_runtime_kernel_op`: native `cudaLaunchKernel` accepts
  exported Psyche runtime kernel token functions for `vector_add_f32`,
  `saxpy_f32`, `scale_f32`, and `axpby_f32` over simulated runtime allocations
  under explicit opt-in. The path is CPU reference execution, accepts the
  null/default stream, validates non-null runtime-owned stream handles, requires
  a CUDA-shaped `void **args` array with the expected parameter slots, stages
  mutating multi-input outputs so exact and partial allocation overlaps use
  original inputs deterministically, keeps `scale_f32` as an intentional
  single-buffer element-local in-place update, rejects dynamic shared memory
  and multidimensional launches, and does not model CUDA fatbins, device-side
  function registration, PTX/CUBIN ingestion, arbitrary kernels, real CUDA
  streams, real contexts, or GPU-resident execution.
- `native_simulated_cublas_op`: a native cuBLAS-shaped symbol is available
  under explicit simulated-memory opt-in. Handles, stream/pointer/math/atomics
  metadata, status helpers, vector/matrix transfer helpers, bounded FP32/FP64 real Level-1 AXPY/COPY/DOT/SCAL/ROT/ROTG/ROTM/ROTMG/SWAP,
  ASUM/NRM2/IAMAX/IAMIN plus bounded FP32/FP64 complex Level-1 AXPY/COPY/DOTU/DOTC/SCAL/real-SCAL/SWAP, FP32/FP64 complex GEMV/GERU/GERC/HEMV/HER/HER2/TRMV/TRSV/TRMM/TRSM, FP32/FP64 GEMV/GER/SYMV/SYR/SYR2/TRMV/TRSV/TRMM/TRSM/SYMM/SYRK/SYR2K,
  FP32/FP64 real GEMM plus real GEMM batched/strided-batched, and FP32/FP64 complex GEMM/HERK/HER2K plus complex GEMM batched/strided-batched run on
  host-accessible CPU pointers, including CPU-backed simulated runtime pointers.
  On Darwin, real/complex `cublasS/D/C/Zgemm[_v2]`,
  `cublasS/D/C/ZgemmBatched` and `cublasS/D/C/ZgemmStridedBatched` batch entries, and
  real/complex TRMM/TRSM route through Accelerate/vecLib CBLAS after Psyche's
  cuBLAS-shaped validation. GEMM keeps temporary output staging and alpha-zero /
  `k == 0` no-read fallbacks; TRMM keeps alpha-zero no-read guards and
  `B`-to-`C` staging. Non-Darwin builds keep the reference-loop implementation.
  On Apple Silicon, `PSYCHE_CUDA_COMPAT_CUBLAS_METAL=required` verifies
  real Metal shared-buffer dispatch for contiguous FP32 `cublasSaxpy[_v2]`
  and `cublasSscal[_v2]`, plus contiguous FP32 `cublasScopy[_v2]` and
  `cublasSdot[_v2]`, `cublasSasum[_v2]`, `cublasSnrm2[_v2]`, plus
  nonzero signed-stride FP32 `cublasSgemv[_v2]` and `cublasSger[_v2]`;
  `PSYCHE_CUDA_COMPAT_CUBLAS_METAL=1` prefers Metal and falls back to the CPU
  reference path for fallback-eligible backend errors, while
  `PSYCHE_CUDA_COMPAT_CUBLAS_METAL=required` returns the Metal-derived status. Strided FP32 AXPY, SSCAL, SCOPY, SDOT, SASUM, and SNRM2 remain
  CPU-backed unless required Metal mode is set, in which case they return
  not-supported instead of falling back.
  `cublasSet/GetVector/Matrix` and async variants copy host-accessible byte
  spans synchronously. Their stream arguments are metadata only, positive
  element sizes, vector strides, and matrix leading dimensions are required,
  matrix leading dimensions must be at least `rows` when `rows > 0`, zero-work
  calls may omit source/destination pointers, staged copies make host overlap
  deterministic, disabled simulated-memory calls return
  `CUBLAS_STATUS_NOT_INITIALIZED` without mutating destination buffers, and no
  real CUDA transfer or stream synchronization is modeled.
  Nonzero signed vector strides are supported for GEMV, GER, HEMV, HER, HER2,
  SYMV, SYR, SYR2, TRMV, and TRSV; positive vector strides are required when work is required for
	  mutating vector ops, DOT/DOTU/DOTC, ROT, and ROTM;
  DOT/DOTU/DOTC write zero for `n <= 0` after result-pointer validation; ASUM/NRM2/IAMAX/IAMIN return zero for `n <= 0` or `incx <= 0`. Complex DOTC conjugates the first vector, Csscal/Zdscal apply real scalar factors to complex vectors, complex GEMV/TRMV/TRSV honor `CUBLAS_OP_C` as conjugate transpose, complex GERU uses `y` as-is, complex GERC conjugates `y`, complex HEMV reads Hermitian diagonals as real without mutating `A`, complex HER/HER2/HERK/HER2K update only the stored triangle and force updated diagonal imaginary parts to zero, complex HERK/HER2K accept `CUBLAS_OP_T` as non-conjugate transpose and avoid reading `C` when `beta == 0` and product inputs when `alpha == 0` or `k == 0`, and complex GEMM honors `CUBLAS_OP_C` as conjugate transpose. ROTM applies FP32/FP64 host-param modified Givens transforms for flags `-2`, `-1`, `0`, and `1`, stages original `x`/`y` inputs before writes, rejects undefined flags, and does not guarantee arbitrary overlapping output vector semantics. ROTMG constructs FP32/FP64 host scalar modified Givens parameters using Netlib scaling rules, writes the flag plus relevant parameter entries, leaves flag-implied entries unchanged, and does not guarantee arbitrary scalar aliasing. ROTG
  constructs FP32/FP64 host scalar Givens parameters and overwrites
  `a`/`b`/`c`/`s` as `r`/`z`/`c`/`s` using Netlib-compatible rules, but it does
  not guarantee arbitrary aliasing among the scalar output pointers. GEMM
  strided-batched permits zero A/B strides to broadcast shared input matrices
  across batches, rejects negative batch strides, and rejects zero C stride when
  multiple output batches would overlap. Device
  pointer-mode scalars/results and tensor/TF32 math modes are metadata-only;
  operations requiring host scalar/result pointers, including AXPY, DOT/DOTU/DOTC, SCAL,
  ROT, ROTG, ROTM, ROTMG, reductions, GEMV, GER, HEMV, HER, HER2, SYMV, SYR, SYR2, TRMM, TRSM, SYMM, SYRK, SYR2K, GEMM,
  and GEMM batched/strided-batched, return not-supported for device pointer mode;
  scalar-free COPY, SWAP, TRMV, and TRSV are not blocked by pointer mode;
  GEMM also returns not-supported for tensor/TF32 math modes.
  SYRK/SYR2K and complex HERK/HER2K update only the requested stored `C` triangle and leave the
  opposite triangle untouched; `beta == 0` avoids reading `C` input storage,
  and `alpha == 0` or `k == 0` avoids reading product inputs where cuBLAS
  permits. ROT stages original `x`/`y` inputs before writes but does not
  guarantee arbitrary overlapping output vector semantics.
  Stream handles are not validated or synchronized. Tensor cores, CUDA kernels,
  cuBLASLt advanced layouts/epilogues/tensor modes/low-precision paths,
  complex Level-2 outside GEMV/GERU/GERC/HEMV/HER/HER2/TRMV/TRSV and complex Level-3 outside GEMM/HERK/HER2K/TRMM/TRSM paths, half/TF32 paths, arbitrary real
  CUDA/UVA/foreign pointers, bitwise NVIDIA cuBLAS parity, and real GPU
  execution outside the opt-in contiguous FP32 `cublasSaxpy[_v2]`,
  `cublasSscal[_v2]`, `cublasScopy[_v2]`, `cublasSdot[_v2]`,
	  `cublasSasum[_v2]`, `cublasSnrm2[_v2]`, signed-stride `cublasSgemv[_v2]`, and signed-stride `cublasSger[_v2]` Metal routes are not modeled. The shim does not validate arbitrary pointer
  provenance; callers must pass valid host-accessible CPU buffers large enough
  for the requested shapes, strides, byte spans, and batches. Non-`_v2` cuBLAS symbols use the current
  handle-based cuBLAS ABI shape as aliases for the matching `_v2`
  implementations; legacy cuBLAS v1 by-value / no-handle ABI is not modeled.
- `native_simulated_cublaslt_op`: a native cuBLASLt-shaped symbol is available
  under explicit simulated-memory opt-in. Handles, opaque matmul descriptors,
  matrix layouts, preferences, algorithm records, status/version/property
  helpers, heuristic selection, and real FP32/FP64 `cublasLtMatmul` with
  `CUBLASLT_ORDER_COL` and `CUBLASLT_ORDER_ROW` matrix layouts run over
  host-accessible CPU pointers, including CPU-backed simulated runtime pointers.
  On Darwin, all-column-major supported cases route through Accelerate/vecLib
  CBLAS after cuBLASLt-shaped validation; row-major or mixed-order cases use the
  reference loop. Non-Darwin builds keep the reference-loop implementation. The
  supported matmul subset accepts `CUBLAS_OP_N`, `CUBLAS_OP_T`, and real-valued
  `CUBLAS_OP_C` as transpose, distinct `D` and `C` buffers, `beta == 0` without
  reading `C`, null `C` data for that beta-zero case when the descriptor is
  valid, compatible strided-batch layouts, leading-dimension validation for the
  selected matrix order, and `CUBLASLT_EPILOGUE_DEFAULT` /
  `CUBLASLT_EPILOGUE_RELU` / `CUBLASLT_EPILOGUE_RELU_AUX` /
  `CUBLASLT_EPILOGUE_BIAS` / `CUBLASLT_EPILOGUE_RELU_BIAS` /
  `CUBLASLT_EPILOGUE_RELU_AUX_BIAS` / `CUBLASLT_EPILOGUE_DRELU` /
  `CUBLASLT_EPILOGUE_DRELU_BGRAD` / `CUBLASLT_EPILOGUE_GELU` /
  `CUBLASLT_EPILOGUE_GELU_BIAS` / `CUBLASLT_EPILOGUE_GELU_AUX` /
  `CUBLASLT_EPILOGUE_GELU_AUX_BIAS` / `CUBLASLT_EPILOGUE_DGELU` /
  `CUBLASLT_EPILOGUE_DGELU_BGRAD`. BIAS-bearing epilogues use a caller-owned
  CPU-resident host-accessible bias vector with dtype unset/default or matching
  `D`, length equal to `D.rows`, optional element stride for per-batch vectors,
  stride-zero broadcast semantics, and positive strides at least `D.rows`.
  ReLU_AUX epilogues write a bit-mask AUX buffer with `AUX_LD` and
  `AUX_BATCH_STRIDE` in bits, divisible by 128; DRELU epilogues read that mask
  and zero raw dy where the bit is clear. The host bridge uses an LSB-first
  intra-byte convention covered by fixed-byte tests but not yet diffed against a
  real NVIDIA AUX buffer; TODO replace this convention note once a real NVIDIA
  AUX byte diff is available. DRELU_BGRAD writes the independent raw-dy row-wise bias
  gradient with FP32 reductions accumulated in FP64.
  GELU_AUX epilogues write the pre-GELU logical output matrix to a caller-owned
  CPU-resident host-accessible AUX buffer; DGELU epilogues read that same
  logical column-major AUX matrix as the saved GELU preactivation input, apply
  the derivative of the documented tanh GELU approximation to
  `alpha*A@B + beta*C`, and write the result to `D`. DGELU_BGRAD also writes a
  bias-gradient vector of length `D.rows` from row-wise sums of the raw
  `alpha*A@B + beta*C` gradient before DGELU multiplication; multi-batch
  DGELU_BGRAD requires a positive bias stride at least `D.rows`. FP32
  bias-gradient reductions accumulate in FP64 before storing the FP32 output.
  DGELU is gradient-consistent with the shim's tanh-approximation GELU paths,
  not exact-erf GELU. AUX dtype must be unset/default or matching `D`, `AUX_LD`
  divisible by 8 elements, at least `D.rows`, and within the backend indexing
  ceiling, and nonoverlapping positive
  AUX stride for multi-batch output/input. Runtime AUX/D, D/bias-gradient, and
  AUX/bias-gradient overlap is rejected before output buffers are written.
  Row-major `D` DGELU support is an intentional shim compatibility extension.
  ReLU clamps logical outputs before writing `D`, while GELU applies NVIDIA's documented tanh approximation. Streams and workspaces are metadata
  only. Pointer-array batch layout mode is supported for DEFAULT-only matmuls
  when A/B/C/D descriptors all use `CUBLASLT_BATCH_MODE_POINTER_ARRAY` with
  matching batch counts; A/B/C/D call arguments are CPU-addressable arrays of
  host-accessible matrix pointers, strided batch offsets are ignored in that
  mode, required null entries are rejected before any `D` batch is written, and
  non-DEFAULT pointer-array epilogues return no heuristic and fail execution as
  not-supported. MatrixTransform descriptor APIs and bounded FP32/FP64
  `cublasLtMatrixTransform` are included in this support level for host
  pointer-mode row/column-major layouts, scale-type conversion, transpose,
  strided broadcast, and exact-count pointer-array batches. Tiled layouts,
  half/BF16/FP8/complex/int data types, tensor-core and TF32
  modes, device/vector pointer modes, AUX scale/amax outputs,
  grouped batches, real CUDA async semantics, and device-resident pointer
  provenance are not modeled.
- `native_simulated_cusparse_spmv_op`: a native cuSPARSE-shaped symbol is
  available under explicit simulated-memory opt-in. Handles, stream and
  pointer-mode metadata, CSR sparse-matrix descriptors, dense-vector
  descriptors, status/version/property helpers, `cusparseSpMV_bufferSize`, and
  `cusparseSpMV` run for bounded FP32 CSR SpMV over host-accessible CPU
  pointers. The supported subset is non-transpose CSR, matching 32-bit or
  64-bit row/column indices on the CPU route, zero-based or one-based indexing,
  host alpha/beta scalars,
  `CUDA_R_32F` matrix/vector/compute types, and DEFAULT/CSR_ALG1/CSR_ALG2
  algorithms. The workspace query returns zero bytes because this bridge uses
  internal staging. On Apple Silicon,
  `PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=required` verifies real Metal
  shared-buffer CSR SpMV dispatch for the same subset, while
  `PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=1` prefers Metal and falls back only for
  backend-availability failures. The Metal route currently remains 32-bit-index
  only; in required-Metal mode, 64-bit CSR indices return not-supported without
  CPU fallback until the local Metal toolchain can prove an MSL 64-bit-index
  kernel. Descriptor lifetime, CSR monotonicity,
  endpoint, column-range, alignment, and output/source overlap failures leave
  `y` unchanged. The same cuSPARSE support level now also includes dense-matrix
  descriptors and a CPU-backed FP32 CSR SpMM subset for non-transpose A/B,
  matching 32-bit or 64-bit CSR indices, row-major or column-major dense B/C
  layouts, host alpha/beta scalars, beta-zero no-read behavior for C, and
  DEFAULT/CSR_ALG1/CSR_ALG2/CSR_ALG3 algorithms. The SpMM workspace query
  validates descriptor/contract metadata, applies required-Metal supportability
  preflight, and returns zero bytes, while CSR row/column content validation is performed by execution. On Apple Silicon,
  required-Metal mode verifies a real Metal shared-buffer CSR SpMM route for the
  32-bit-index subset, while preferred Metal mode falls back to the CPU
  reference path only for backend-availability failures. Required-Metal 64-bit
  CSR indices return not-supported until the local Metal toolchain can prove an
  MSL 64-bit-index kernel. Transpose/conjugate
  transpose, device pointer-mode scalars, non-CSR formats, 16-bit indices,
  non-FP32/complex/low-precision values, preprocess/update APIs, broader SpMM
  formats/algorithms, SpSV/SpSM, sparse/dense conversions, batched sparse APIs,
  external workspace semantics, CUDA streams, CUDA graphs, and async behavior
  remain unsupported.
- `native_simulated_cusolver_dense_lu_op`: a native cuSolverDN-shaped symbol is
  available under explicit simulated-memory opt-in. Handles, stream metadata,
  version/property/status helpers, `cusolverDnS/Dgetrf_bufferSize`,
  `cusolverDnS/Dgetrf`, and `cusolverDnS/Dgetrs` run for dense FP32/FP64
  column-major host-accessible pointers. On Darwin, pivoted execution routes
  through Accelerate/LAPACK and preserves 1-based pivot indices; singular
  factorizations return success with positive `devInfo`. Singularity detection
  follows exact-zero pivot signaling; this shim does not estimate conditioning
  or warn on near-singular matrices. Non-Darwin builds use a deterministic CPU
  reference partial-pivot LU/solve for the same bounded subset. A null `devIpiv`
  follows cuSOLVER's no-pivot LU mode and uses a
  deterministic reference LU/triangular solve. No-pivot zero-diagonal `getrs` returns
  `CUSOLVER_STATUS_EXECUTION_FAILED` with positive `devInfo` and leaves B
  unchanged. Workspace queries return `m * n` elements, while execution
  validates a non-null workspace when work is required and stages mutable A/B
  buffers before copy-back. Device pointers, async CUDA stream
  execution, CUDA graphs, batched solvers, sparse cuSolverSP/cuDSS-style solves,
  QR, eigen, SVD, IRS, RF/Mg, complex/low-precision types, real GPU
  residency, and bitwise NVIDIA parity remain unsupported.
- `native_simulated_cusolver_dense_cholesky_op`: a native cuSolverDN-shaped
  symbol is available under explicit simulated-memory opt-in for dense FP32/FP64
  column-major host-accessible pointers. `cusolverDnS/Dpotrf_bufferSize`
  and `cusolverDnS/Dpotri_bufferSize` return the shim's conservative `n * n`
  element workspace; `cusolverDnS/Dpotrf` and `cusolverDnS/Dpotri` validate
  that workspace, stage A, operate only over the requested lower or upper
  triangle, and leave the opposite triangle untouched. On Darwin, Cholesky
  factorization/inversion routes through Accelerate/LAPACK; non-Darwin builds
  use deterministic CPU reference factorization/inverse/solve routes. Positive
  non-positive-definite `potrf` `devInfo` returns success and leaves caller A
  unchanged. `cusolverDnS/Dpotri` consumes a Cholesky factor, copies back only
  the requested inverse triangle on success, and returns success with positive
  `devInfo` for an exact-zero factor diagonal while leaving A unchanged.
  `cusolverDnS/Dpotrs` stages B, handles multiple RHS with `ldb * nrhs`
  storage, and copies back only on success; the exact-zero diagonal `potrs`
  precheck is a shim-safety behavior, not a real cuSOLVER guarantee. Batched
  Cholesky/inverse, complex and low-precision types, device pointers, async
  CUDA stream execution, CUDA graphs, and bitwise NVIDIA parity remain unsupported.
- `native_proc_address_stub`: `cuGetProcAddress` only exposes safe discovery
  symbols; simulated or execution symbols return a null function pointer and
  not-found status.
- `unsupported_requires_bridge`: the operation is still unsupported until a real
  Metal, MPSGraph, MLX, compiler, runtime, or library bridge exists.

As of the current checked-in manifest, the generated report accounts for 3
Psyche-owned Python redirection boundaries, 29 `mps_exact_route` ledger entries,
610 native CUDA/NVML/cuSPARSE/cuSOLVER/cuDNN-shaped stub symbols, and 11 unsupported bridge categories. The MPS
ledger count is split into 24 default ATen/MPS route surfaces, 4 gated
experimental exact-intent route surfaces, and 1 Python compatibility boundary
entry. The native count is split into 28 discovery symbols, 5 hard-fail symbols,
59 bounded simulated driver-memory operations, 20 bounded simulated
driver-mempool operations, 16 bounded simulated driver-sync operations, 3
bounded simulated driver-kernel operations, 26
bounded simulated runtime-memory operations, 20 bounded simulated
runtime-mempool operations, 16 bounded simulated runtime-sync operations, 5
bounded simulated runtime-kernel operations, 221
bounded simulated cuBLAS operations, 36 bounded simulated cuBLASLt operations,
18 bounded simulated cuSPARSE CSR SpMV symbols, 5 bounded simulated cuSPARSE CSR SpMM symbols,
16 bounded simulated cuSOLVER dense LU/solve symbols, 6 bounded simulated
cuSOLVER dense Cholesky symbols,
25 NVML discovery symbols, 14 cuDNN discovery symbols, 8 bounded simulated
cuDNN activation symbols, 1 bounded simulated cuDNN add-tensor symbol, 1 bounded
simulated cuDNN batch-normalization inference symbol, 17 bounded simulated cuDNN
convolution-forward symbols, 1 bounded simulated cuDNN fused conv-bias-activation
symbol, 16 bounded simulated cuDNN convolution-backward-data symbols,
16 bounded simulated cuDNN convolution-backward-filter symbols, 1 bounded
simulated cuDNN transform symbol, 7 bounded simulated cuDNN pooling symbols, 2
bounded simulated cuDNN softmax symbols, and 1
`cuGetProcAddress` policy symbol.

## Support Matrix

| Pattern | Status |
| --- | --- |
| `resolve_device(0 / "cuda" / "cuda:0")` | Redirected to MPS only when `PSYCHE_CUDA_COMPAT=1`, CUDA is absent, and MPS is available. |
| `resolve_device(None)` | Rejected. `None` is not silently treated as CUDA device 0. |
| Exact MPS fallback fixes under CUDA compat | Enabled for MPS contexts when `PSYCHE_CUDA_COMPAT=1`; opt out with `PSYCHE_CUDA_COMPAT_MPS_ROUTES=0`. |
| Psyche `make_causal_lm(..., device="cuda:0")` | Resolves to a real MPS device under the same opt-in conditions. |
| Redirected MPS architecture support | Currently validated for `HfAuto` with fallback-disabled forward/backward; other architectures fail until they opt in. |
| Python sidecar device arguments | Explicit device strings are accepted. The Rust launcher passes CUDA ranks as `cuda:N` and MPS as `mps`. Bare integer strings remain a legacy Psyche-owned CUDA-shaped boundary and are treated like CUDA ordinals, not generic devices. |
| Python sidecar subprocess boundary | `scripts/check-sidecar-mps-device.py` verifies parser wiring, env propagation, CUDA-shaped MPS redirection, Gloo-only backend validation, MPS single-rank rejection, and a single-rank Gloo CPU-staged collective smoke. |
| `libcuda` / `libcudart` / `libcublas` / `libcublasLt` / `libcusparse` / `libcusolver` discovery | Minimal macOS native stubs can be built from `tools/cuda-compat-shim/`; they identify themselves as Psyche stubs, report no CUDA device, and reject arbitrary execution paths instead of spoofing Apple GPUs as CUDA. |
| `libnvidia-ml` / NVML discovery | Initializes as a compatibility library with NVML-style refcount semantics, reports zero NVIDIA devices through `nvmlDeviceGetCount[_v2]`, returns parseable stub driver/NVML versions, keeps PyTorch's NVML-based CUDA availability probe false, and rejects handle lookup or telemetry without reporting Apple GPU identity, power, thermals, utilization, process accounting, or memory as NVIDIA data. |
| `libcudnn` / cuDNN discovery | Exposes cuDNN version/property/error-string discovery helpers as zero/stub values. By default, `cudnnCreate` clears the output handle and returns not-initialized; graph-version calls still report unsupported rather than claiming graph execution. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuDNN activation | Enables cuDNN metadata handles, tensor descriptors, activation descriptors, `cudnnActivationForward`, and `cudnnActivationBackward` for contiguous 4D NCHW FP32 ReLU/Sigmoid/Tanh/Identity over host-accessible buffers, with activation coefficients ignored for those supported modes. Forward supports exact `x == y`; backward validates `y` but computes derivatives from `x`, supports exact `dy == dx`, avoids reading old dx when beta is zero, sanitizes NaN x to zero under `CUDNN_NOT_PROPAGATE_NAN`, lets upstream dy NaNs propagate, and rejects backward x/dx or y/dx overlap. This is a bounded compatibility bridge, not bitwise cuDNN parity for sigmoid/tanh backward paths that derive gradients from saved raw y. The default path is CPU reference execution; `PSYCHE_CUDA_COMPAT_CUDNN_METAL=required` verifies real Metal shared-buffer dispatch for that subset and leaves outputs unchanged on backend failure, while `PSYCHE_CUDA_COMPAT_CUDNN_METAL=1` prefers Metal and falls back only for backend-availability failures. Partial x/y forward overlap, partial dy/dx backward overlap, NHWC, non-FP32, custom strides, clipped ReLU, ELU, swish, and broader activation/cuDNN operations remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuDNN add tensor | Enables `cudnnAddTensor` for same-shape and broadcasted contiguous 4D NCHW FP32 tensors where each A dimension equals the corresponding C dimension or is `1`, including the common `1xCx1x1` bias-add case. The bridge computes `C = alpha * A_broadcast + beta * prior_C`, rejects any A/C overlap including exact aliasing, avoids reading prior C when beta is zero, still evaluates `alpha * A` so alpha-zero NaN source values propagate, defaults to CPU reference execution, and uses the same required/preferred Metal route. 5D tensors, NHWC, non-FP32 tensors, custom strides, aliased A/C storage, and full cuDNN async semantics remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuDNN batch-normalization inference | Enables `cudnnBatchNormalizationForwardInference` for contiguous 4D NCHW FP32 x/y tensors with matching descriptors and FP32 scale/bias/mean/variance descriptors for `CUDNN_BATCHNORM_SPATIAL` (`1xCx1x1`) and `CUDNN_BATCHNORM_PER_ACTIVATION` (`1xCxHxW`). The bridge computes the legacy inference formula with host alpha/beta scalars, rejects epsilon below `CUDNN_BN_MIN_EPSILON`, rejects `CUDNN_BATCHNORM_SPATIAL_PERSISTENT`, avoids reading prior y when beta is zero, allows exact `x == y`, rejects partial x/y and parameter/stat buffer overlap, defaults to CPU reference execution, and uses the same required/preferred Metal route. Training forward, backward, 5D tensors, NHWC, non-FP32 tensors, custom strides, and broader normalization APIs remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuDNN convolution forward | Enables cuDNN filter/convolution descriptors, `cudnnGetConvolution2dForwardOutputDim`, `cudnnGetConvolutionForwardAlgorithmMaxCount`, `cudnnGetConvolutionForwardAlgorithm`, `cudnnGetConvolutionForwardAlgorithm_v7`, `cudnnFindConvolutionForwardAlgorithm`, `cudnnGetConvolutionForwardWorkspaceSize`, and `cudnnConvolutionForward` for contiguous 4D NCHW FP32 x/y tensors with contiguous FP32 NCHW/KCRS filters. The bridge supports cuDNN grouped semantics (`groupCount > 0`, full x/y descriptors, filter `C = input_C/groupCount`, `input_C % groupCount == 0`, and `K % groupCount == 0`), including depthwise and depthwise-multiplier cases, deterministic zero-workspace `CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM`, cross-correlation, true convolution via spatial R/S filter flipping, host alpha/beta scalars, beta-zero no-read behavior, CPU reference execution by default, and the same required/preferred Metal route for that FP32 contiguous 4D NCHW/KCRS grouped subset. With `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required`, the Metal route can prefer an MPSGraph `convolution2D` fast path that prepares weights, writes raw y internally, applies alpha/beta through an MSL epilogue, leaves y unchanged on required-mode prepare/graph/epilogue failure, and falls back to deterministic MSL in preferred mode. Workspace is accepted but ignored for the supported algorithm. Alternate algorithms, `cudnnFindConvolutionForwardAlgorithmEx`, broader fused convolution APIs, 5D/Nd tensors, NHWC, non-FP32 tensors, custom strides, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, and full cuDNN async semantics are outside this bounded slice. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuDNN convolution bias activation forward | Enables `cudnnConvolutionBiasActivationForward` for contiguous 4D NCHW FP32 x/z/y tensors with contiguous FP32 NCHW/KCRS filters, strict `1xKx1x1` FP32 bias, value-identical z/y descriptors, ReLU with `IMPLICIT_GEMM`, and identity with `IMPLICIT_PRECOMP_GEMM`. The bridge computes `y = act(alpha1 * conv_or_correlation(x, w) + alpha2 * z + bias[k])`, supports exact `z == y`, rejects partial z/y overlap and broader x/w/z/bias/y overlap, avoids reading z when alpha2 is zero, accepts but ignores workspace, and leaves y unchanged on validation failure or required Metal backend failure. The default path is CPU reference execution; `PSYCHE_CUDA_COMPAT_CUDNN_METAL=required` verifies a real fused MSL shared-buffer route for the same subset, and `PSYCHE_CUDA_COMPAT_CUDNN_METAL=1` prefers Metal with CPU fallback only for backend-availability failures. The Metal route stages x, w, z, and bias, uses a separate output buffer so exact `z == y` is preserved, suppresses ReLU NaNs under `CUDNN_NOT_PROPAGATE_NAN`, and copies y back only after command-buffer completion. With `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required`, the Metal route can prefer MPSGraph `convolution2D` for raw grouped NCHW/OIHW FP32 convolution, then apply alpha1, optional alpha2/z, bias, and ReLU/identity through an MSL epilogue; required-mode prepare, graph, or epilogue failures leave y unchanged, while preferred mode falls back to deterministic fused MSL. No fused MLX route is claimed yet. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuDNN convolution backward data | Enables `cudnnGetConvolutionBackwardDataAlgorithmMaxCount`, `cudnnGetConvolutionBackwardDataAlgorithm`, `cudnnGetConvolutionBackwardDataAlgorithm_v7`, `cudnnFindConvolutionBackwardDataAlgorithm`, `cudnnGetConvolutionBackwardDataWorkspaceSize`, and `cudnnConvolutionBackwardData` for contiguous 4D NCHW FP32 dy/dx tensors with contiguous FP32 NCHW/KCRS filters. The bridge supports the same cuDNN grouped semantics (`groupCount > 0`, full dy/dx descriptors, filter `C = input_C/groupCount`, `input_C % groupCount == 0`, and `K % groupCount == 0`), including depthwise and depthwise-multiplier cases, deterministic zero-workspace `CUDNN_CONVOLUTION_BWD_DATA_ALGO_1`, cross-correlation, true convolution via spatial R/S filter flipping, host alpha/beta scalars, beta-zero no-read behavior, CPU reference execution by default, and the same required/preferred Metal route for that FP32 contiguous 4D NCHW/KCRS grouped subset. With `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required`, the Metal route can prefer an MPSGraph `convolution2DDataGradient` fast path that prepares weights, writes raw dx internally, applies alpha/beta through an MSL epilogue, leaves dx unchanged on required-mode prepare/graph/epilogue failure, and falls back to deterministic MSL in preferred mode. Workspace is accepted but ignored for the supported algorithm. `ALGO_0`, FFT, Winograd, broader fused convolution APIs, FindEx timing/benchmark execution, 5D/Nd tensors, NHWC, non-FP32 tensors, custom strides, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, and full cuDNN async semantics are outside this bounded slice. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuDNN convolution backward filter | Enables `cudnnGetConvolutionBackwardFilterAlgorithmMaxCount`, `cudnnGetConvolutionBackwardFilterAlgorithm`, `cudnnGetConvolutionBackwardFilterAlgorithm_v7`, `cudnnFindConvolutionBackwardFilterAlgorithm`, `cudnnGetConvolutionBackwardFilterWorkspaceSize`, and `cudnnConvolutionBackwardFilter` for contiguous 4D NCHW FP32 x/dy tensors with contiguous FP32 NCHW/KCRS dw filters. The bridge supports the same cuDNN grouped semantics (`groupCount > 0`, full x/dy descriptors, filter `C = input_C/groupCount`, `input_C % groupCount == 0`, and `K % groupCount == 0`), including depthwise and depthwise-multiplier cases, deterministic zero-workspace `CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1`, cross-correlation, true convolution via spatial input-tap flipping while leaving the physical KCRS dw write slot unchanged, host alpha/beta scalars, beta-zero no-read behavior, CPU reference execution by default, and the same required/preferred Metal route for that FP32 contiguous 4D NCHW/KCRS grouped subset. Workspace is accepted but ignored for the supported algorithm. The Metal route keeps a deterministic one-thread-per-dw serial kernel as fallback/oracle, and for larger `N*outH*outW` spans uses a private-scratch two-pass tiled reduction with fixed-order threadgroup partial sums, fixed chunk-order final reduction, guarded beta-zero prior reads, and CPU copy-back only after successful command-buffer completion. With `PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required`, the same Metal route can prefer a weak-linked MPSGraph `convolution2DWeightsGradient` backend for NCHW/OIHW grouped FP32 backward-filter work. MPSGraph writes raw dW into an internal shared buffer, then an MSL epilogue writes alpha/beta and true-convolution R/S flip results into a second internal buffer before CPU copy-back; required-mode MPSGraph or epilogue failures leave caller `dw` unchanged, while preferred mode falls back to deterministic MSL. MPSGraph numerical checks are tolerance-based; MSL remains the bitwise deterministic oracle. The private scratch is internal implementation storage, not caller workspace, so public workspace remains zero. `ALGO_0`, `ALGO_3`, FFT, Winograd, broader fused convolution APIs, FindEx timing/benchmark execution, 5D/Nd tensors, NHWC, non-FP32 tensors, custom strides, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, and full cuDNN async semantics are outside this bounded slice. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuDNN transform | Enables `cudnnTransformTensor` for the scaled-copy subset on contiguous 4D NCHW FP32 tensors with value-identical descriptors: `y = alpha * x + beta * prior_y`. Source/destination overlap is rejected, beta-zero avoids reading prior y, CPU reference execution is the default, and `PSYCHE_CUDA_COMPAT_CUDNN_METAL=required` / `=1` use the same required/preferred Metal route. Arbitrary-stride layout conversion, NHWC, non-FP32 tensors, tensor-transform descriptors, and `TransformTensorEx` remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuDNN pooling | Enables cuDNN pooling descriptors, `cudnnGetPooling2dDescriptor`, `cudnnGetPooling2dForwardOutputDim`, `cudnnPoolingForward`, and `cudnnPoolingBackward` for contiguous 4D NCHW FP32 2D max and average pooling over host-accessible buffers. `CUDNN_POOLING_MAX`, `CUDNN_POOLING_MAX_DETERMINISTIC`, `CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING`, and `CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING` are supported with host alpha/beta scalars, exact `x == y` forward support only for value-identical tensor descriptors, exact `dy == dx` backward support only for value-identical tensor descriptors, explicit NaN propagation behavior, CPU reference execution by default, and the same required/preferred Metal route. Average backward is geometry-based and permits null x/y descriptors and data; max backward requires x/y descriptors and data but recomputes deterministic first-max selection from x. Partial x/y forward overlap, partial dy/dx backward overlap, max-backward x/dx or y/dx overlap, Nd pooling descriptors, NHWC, non-FP32, custom strides, and broader cuDNN operations remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuDNN softmax | Enables `cudnnSoftmaxForward` and `cudnnSoftmaxBackward` for contiguous 4D NCHW FP32 tensors with `CUDNN_SOFTMAX_FAST`, `CUDNN_SOFTMAX_ACCURATE`, and `CUDNN_SOFTMAX_LOG` in CHANNEL and INSTANCE modes. The bridge supports host alpha/beta scalars, deterministic whole-vector NaN propagation, stable forward positive-infinity handling for ACCURATE/LOG, beta-zero execution without reading prior output storage, exact `x == y` forward staging, exact `dy == dx` backward staging, CPU reference execution by default, and the same required/preferred Metal route with one cooperative 256-lane threadgroup per softmax vector. Partial x/y, dy/dx, or y/dx overlap, 5D tensors, NHWC, non-FP32, custom strides, and broader cuDNN operations remain unsupported. |
| `cudaMemcpy(..., cudaMemcpyHostToHost)` / `cudaMemcpy2D(..., cudaMemcpyHostToHost)` / linear `cudaMemcpy3D(..., cudaMemcpyHostToHost)` | Implemented as real host memory copies in the runtime shim. Linear copies use `memmove`, 2D copies copy bounded rows, and 3D copies stage through a temporary CPU buffer so overlapping volumes are well-defined. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` memory ops | Opts the native shims into CPU-backed simulated allocation, async allocation/free, managed allocation, advice, prefetch, range query, pointer query, pitched allocation, copy, 2D copy, 3D copy, fill, 2D fill, pinned-host-memory probes, and host/no-location memory-pool metadata. Driver coverage includes `cuMemAllocAsync`, `cuMemAllocFromPoolAsync`, `cuMemFreeAsync`, `cuMemPool*`, `cuMemGetDefaultMemPool`, `cuMemGetMemPool`, `cuMemSetMemPool`, `cuMemAllocManaged`, `cuMemAdvise`, `cuMemPrefetchAsync`, `cuMemRangeGetAttribute(s)`, `cuPointerGetAttribute(s)`, and `cuPointerSetAttribute` alongside the existing allocation/copy/fill/pinned-host APIs; runtime coverage includes `cudaMallocAsync`, `cudaMallocFromPoolAsync`, `cudaFreeAsync`, `cudaMemPool*`, `cudaMemGetDefaultMemPool`, `cudaMemGetMemPool`, `cudaMemSetMemPool`, `cudaMallocManaged`, `cudaMemAdvise`, `cudaMemPrefetchAsync`, `cudaMemRangeGetAttribute(s)`, and `cudaPointerGetAttributes` alongside the existing allocation/copy/fill/pinned-host APIs. Only `1`, `true`, `yes`, or `on` enable it; `0`, `false`, `no`, `off`, empty string, unset, and arbitrary strings keep it disabled. Managed allocations are CPU-backed; memory advice, CPU prefetch, range attributes, pointer attributes, and pool attributes are metadata-only. The advice/prefetch APIs use fixed CUDA-shaped device-ordinal prototypes, prefetch and async allocation stream arguments are validated against the same shim-family stream registry as other async APIs, and simulated device/managed/async/pool-backed allocations are at least 256-byte aligned. GPU advice/prefetch destinations fail with invalid-device errors. Device-default pool APIs reject absent CUDA devices; IPC/export/access-control pools, graph allocator ownership, GPU-resident pools, managed-memory migration, residency, page faults, and access counters are not modeled. Simulated memory operations are serialized by the shim allocation mutexes; linear copies use `memmove`, pitched 2D copies copy bounded rows, and linear 3D copies stage through a temporary CPU buffer so overlapping volumes are well-defined; async forms run synchronously. CUDA arrays, textures, surfaces, and unified-memory 3D operands are unsupported. Runtime and driver simulated host ownership are separate; cross-family async, pool-backed, free, and unregister calls fail safely. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` driver kernel subset | `cuModuleLoadData` accepts a Psyche-native `PSYCHE_CUDA_MODULE_V1` blob declaring `vector_add_f32` and/or `saxpy_f32` and/or `scale_f32` and/or `axpby_f32`; `cuModuleGetFunction` resolves those registered functions; `cuLaunchKernel` runs fixed 1D parameter schemas over simulated driver allocations. `vector_add_f32` computes `out[i] = a[i] + b[i]`; `saxpy_f32` computes in-place `y[i] = alpha * x[i] + y[i]`; `scale_f32` computes in-place `x[i] = alpha * x[i]`; `axpby_f32` computes in-place `x[i] = alpha * x[i] + beta * y[i]`. The default path is CPU reference execution; on Apple Silicon, `PSYCHE_CUDA_COMPAT_METAL_KERNELS=required` verifies real Metal shared-buffer dispatch for those same registered kernels through copy-in/copy-back staged buffers. Exact aliases are supported for tested in-place/output cases, and partial overlaps involving a mutated span fall back to CPU in preferred Metal mode or return unsupported in required Metal mode. Raw PTX/CUBIN, `cuModuleLoad` files, arbitrary kernels, dynamic shared memory, extra launch config, multidimensional launches, and general CUDA execution remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` runtime kernel subset | `libcudart` exports `psyche_cuda_runtime_kernel_vector_add_f32`, `psyche_cuda_runtime_kernel_saxpy_f32`, `psyche_cuda_runtime_kernel_scale_f32`, and `psyche_cuda_runtime_kernel_axpby_f32` as function-address tokens. `cudaLaunchKernel` accepts only those tokens, accepts the null/default stream, validates non-null runtime-owned stream handles, requires a CUDA-shaped `void **args` array with the expected parameter slots, stages mutating multi-input outputs so exact and partial allocation overlaps use original inputs deterministically, keeps `scale_f32` as an intentional single-buffer element-local in-place update, rejects dynamic shared memory and multidimensional launch geometry, and runs the same fixed 1D parameter schemas over simulated runtime allocations through a CPU reference executor. Arbitrary CUDA fatbins, device-side runtime registration, PTX/CUBIN ingestion, arbitrary runtime function pointers, real CUDA streams/contexts, and GPU-resident execution remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuBLAS ops | Enables `libcublas` handle creation, metadata helpers, host-accessible transfer helpers, broad CPU-backed FP32/FP64 real and complex Level-1/GEMV/GER/TRMV/TRSV/GEMM-family coverage over host-accessible buffers, Darwin Accelerate-backed real/complex GEMM/TRMM/TRSM and real/complex GEMM batched/strided-batched batch entries, and the opt-in Apple Silicon Metal route for contiguous FP32 `cublasSaxpy[_v2]`, `cublasSscal[_v2]`, `cublasScopy[_v2]`, `cublasSdot[_v2]`, `cublasSasum[_v2]`, `cublasSnrm2[_v2]`, signed-stride `cublasSgemv[_v2]`, and signed-stride `cublasSger[_v2]` via `PSYCHE_CUDA_COMPAT_CUBLAS_METAL`. Detailed semantics and caveats are listed in the cuBLAS paragraph above; arbitrary CUDA/UVA pointers, tensor cores, unsupported complex/half/TF32 surfaces, real CUDA transfer, and real Apple GPU execution outside those explicit SAXPY/SSCAL/SCOPY/SDOT/SASUM/SNRM2/SGEMV/SGER routes remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuBLASLt ops | Enables `libcublasLt` handle creation, opaque descriptor/layout/preference/algorithm records, supported descriptor attributes, heuristic queries, algorithm initialization/checks, real FP32/FP64 `cublasLtMatmul` with `CUBLASLT_ORDER_COL` and `CUBLASLT_ORDER_ROW` matrix layouts over host-accessible CPU buffers, and bounded FP32/FP64 `cublasLtMatrixTransform` over those row/column-major host-accessible layouts. Supported matmul cases accept real `CUBLAS_OP_N`, `CUBLAS_OP_T`, and `CUBLAS_OP_C` as transpose, distinct `D` and `C` buffers, `beta == 0` without reading `C`, null `C` data in that beta-zero case, compatible strided-batch layouts, DEFAULT-only pointer-array batch layouts when all A/B/C/D descriptors use `CUBLASLT_BATCH_MODE_POINTER_ARRAY` with matching batch counts and CPU-addressable pointer arrays, leading-dimension validation for the selected matrix order, and `CUBLASLT_EPILOGUE_DEFAULT` / `CUBLASLT_EPILOGUE_RELU` / `CUBLASLT_EPILOGUE_RELU_AUX` / `CUBLASLT_EPILOGUE_BIAS` / `CUBLASLT_EPILOGUE_RELU_BIAS` / `CUBLASLT_EPILOGUE_RELU_AUX_BIAS` / `CUBLASLT_EPILOGUE_DRELU` / `CUBLASLT_EPILOGUE_DRELU_BGRAD` / `CUBLASLT_EPILOGUE_GELU` / `CUBLASLT_EPILOGUE_GELU_BIAS` / `CUBLASLT_EPILOGUE_GELU_AUX` / `CUBLASLT_EPILOGUE_GELU_AUX_BIAS` / `CUBLASLT_EPILOGUE_DGELU` / `CUBLASLT_EPILOGUE_DGELU_BGRAD` / `CUBLASLT_EPILOGUE_BGRADA` / `CUBLASLT_EPILOGUE_BGRADB`. MatrixTransform supports host pointer-mode alpha/beta scalars, FP32 or FP64 scale type with input and output conversion, `CUBLAS_OP_N/T/C` where real `OP_C` is transpose, alpha-zero and beta-zero no-read behavior, strided input batch-count-one broadcast, exact-count pointer-array batches, all-batch pointer-array preflight before any C write, unsafe source/C overlap rejection, and exact same-layout no-transpose in-place sources. BIAS-bearing epilogues use a caller-owned CPU-resident host-accessible bias vector with dtype unset/default or matching `D`, length equal to `D.rows`, optional element stride for per-batch vectors, stride-zero broadcast semantics, and positive strides at least `D.rows`; ReLU_AUX epilogues write a bit-mask AUX buffer with `AUX_LD` and `AUX_BATCH_STRIDE` in bits, divisible by 128, using an LSB-first intra-byte host-bridge convention that is fixed-byte tested but not yet diffed against real NVIDIA AUX output; TODO replace this convention note once a real NVIDIA AUX byte diff is available. DRELU epilogues read that mask and zero raw dy where the bit is clear. DRELU_BGRAD writes the independent raw-dy row-wise bias gradient with FP32 reductions accumulated in FP64. GELU_AUX epilogues write the pre-GELU logical output matrix to a caller-owned CPU-resident host-accessible AUX buffer; DGELU epilogues read that same logical column-major AUX matrix as saved GELU preactivation input, apply the derivative of the documented tanh GELU approximation to `alpha*A@B + beta*C`, and write `D`; DGELU_BGRAD additionally writes row-wise sums of raw `alpha*A@B + beta*C` into a `D.rows` bias-gradient vector before DGELU multiplication, accumulating FP32 reductions in FP64 before storing FP32 output, with positive per-batch bias stride required for multi-batch output. BGRADA/BGRADB write operand-source bias-gradient vectors without alpha/beta scaling: BGRADA writes length `D.rows` with `bias_gradient[row] = sum_k op(A)[row,k]`, and BGRADB writes length `D.cols` with `bias_gradient[col] = sum_k op(B)[k,col]`; the reduced source operand is required even when alpha is zero, positive per-batch bias stride must cover the selected gradient length, and multi-batch BGRADA/BGRADB rejects stride-zero broadcast. FP32 operand-gradient reductions accumulate in FP64. Pointer-array mode ignores strided-batch offsets, rejects required null entries before any `D` batch is written, returns no heuristic for non-DEFAULT pointer-array epilogues, and fails non-DEFAULT pointer-array execution as not-supported. BGRADA/BGRADB currently leave `D` as the raw DEFAULT matmul output on the host bridge and emit a one-time runtime warning for that unverified D-output behavior; the D-output behavior and reduction order still need a real NVIDIA hardware byte diff before claiming bitwise parity. DGELU is gradient-consistent with the shim's tanh-approximation GELU paths, not exact-erf GELU. AUX dtype must be unset/default or matching `D` for GELU AUX matrices, while ReLU mask epilogues reject non-default AUX data types; runtime AUX/D, D/bias-gradient, AUX/bias-gradient for DRELU_BGRAD/DGELU_BGRAD, and reduced-source/bias-gradient overlap is rejected before output buffers are written. Row-major `D` DRELU/DGELU support is an intentional shim compatibility extension. On Darwin, all-column-major supported cases use Accelerate/vecLib CBLAS for the raw GEMM core after cuBLASLt-shaped validation, then apply cuBLASLt epilogues by CPU postprocessing; row-major or mixed-order layouts use the CPU reference-loop GEMM core. Tiled layouts, half/BF16/FP8/complex/int data types, tensor-core and TF32 modes, device/vector pointer modes, AUX scale/amax outputs, grouped batches, real CUDA async semantics, and device-resident pointer provenance remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuSPARSE CSR SpMV | Enables `libcusparse` handle creation, metadata helpers, CSR sparse-matrix descriptors, dense-vector descriptors, `cusparseSpMV_bufferSize`, and `cusparseSpMV` for FP32 CSR SpMV over host-accessible buffers. The supported subset is non-transpose CSR with matching 32-bit or 64-bit row/column indices on the CPU route, zero-based or one-based indexing, host alpha/beta scalars, `CUDA_R_32F` matrix/vector/compute types, and DEFAULT/CSR_ALG1/CSR_ALG2 algorithms. The default route is CPU reference execution with staged output copy-back; `PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=required` verifies real Metal shared-buffer dispatch for the 32-bit-index subset, and `PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=1` prefers Metal with CPU fallback only for backend-availability failures. Required-Metal 64-bit CSR indices return not-supported until the local Metal toolchain can prove an MSL 64-bit-index kernel. Device pointer-mode scalars, transpose/conjugate transpose, non-CSR formats, 16-bit indices, non-FP32 values, broader SpMV forms, SpSV/SpSM, sparse/dense conversions, batched sparse APIs, external workspace semantics, CUDA streams/graphs/async semantics, and cuSOLVER beyond the dense S/D LU/solve and Cholesky bridges remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuSPARSE CSR SpMM | Enables dense-matrix descriptors plus `cusparseSpMM_bufferSize` and `cusparseSpMM` for FP32 CSR SpMM over host-accessible buffers. The supported subset is non-transpose A/B, matching 32-bit or 64-bit row/column CSR indices, zero-based or one-based indexing, host alpha/beta scalars, `CUDA_R_32F` sparse/dense/compute types, `CUSPARSE_ORDER_COL` or `CUSPARSE_ORDER_ROW` B/C layouts with leading-dimension validation, and DEFAULT/CSR_ALG1/CSR_ALG2/CSR_ALG3 algorithms. The workspace query validates descriptor/contract metadata, applies required-Metal supportability preflight, and returns zero bytes without reading CSR row/column contents; execution validates CSR contents. The CPU reference path stages logical C and writes back only after success, beta-zero avoids reading prior C, and rejected paths leave C unchanged. `PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=required` verifies real Metal shared-buffer dispatch for the 32-bit-index subset and returns not-supported for 64-bit CSR indices without CPU fallback; preferred Metal mode falls back to the CPU reference path only for backend-availability failures. Transpose/conjugate transpose, device pointer-mode scalars, non-CSR formats, 16-bit or mixed CSR index widths, non-FP32 values, COO/Blocked-ELL/BSR SpMM algorithms, external workspace semantics, CUDA streams/graphs/async semantics, and broader/tiled/MLX SpMM beyond the bounded 32-bit CSR route remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuSOLVER dense LU/solve | Enables `libcusolver` cuSolverDN handles, stream metadata, version/property/status helpers, `cusolverDnSgetrf_bufferSize`, `cusolverDnDgetrf_bufferSize`, `cusolverDnSgetrf`, `cusolverDnDgetrf`, `cusolverDnSgetrs`, and `cusolverDnDgetrs` for dense FP32/FP64 column-major matrices over host-accessible buffers. Pivoted `getrf`/`getrs` uses Accelerate/LAPACK with 1-based pivots; singular factors return success with positive `devInfo`. Null `devIpiv` uses a deterministic no-pivot LU/solve path. No-pivot zero-diagonal `getrs` returns `CUSOLVER_STATUS_EXECUTION_FAILED` with positive `devInfo` and leaves B unchanged. Workspace queries return `m * n` elements; execution validates required workspace but stages A/B so validation, allocation, and failed no-pivot solve paths do not mutate caller buffers. Device pointers, async CUDA stream execution, CUDA graphs, batched solvers, sparse solves, QR, eigen, SVD, IRS, RF/Mg, complex/low-precision types, real GPU residency, and bitwise NVIDIA parity remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` cuSOLVER dense Cholesky | Enables `cusolverDnS/Dpotrf_bufferSize`, `cusolverDnS/Dpotrf`, `cusolverDnS/Dpotri_bufferSize`, `cusolverDnS/Dpotri`, and `cusolverDnS/Dpotrs` for dense FP32/FP64 column-major matrices over host-accessible buffers. Workspace queries return a conservative `n * n` elements; execution validates the workspace but Darwin uses Accelerate/LAPACK internal storage. The bridge explicitly translates lower/upper triangle modes, references only the requested triangle, leaves the opposite triangle untouched, returns success with positive `devInfo` for non-positive-definite `potrf` while leaving caller A unchanged, consumes an existing Cholesky factor for `potri` without refactorizing, copies back only the requested inverse triangle on successful `potri`, returns success with positive `devInfo` for exact-zero factor diagonals while leaving A unchanged, and stages B for `potrs` so failed solves do not mutate caller buffers. The exact-zero diagonal `potrs` precheck is a shim-safety behavior, not a real cuSOLVER guarantee. Non-Darwin builds use deterministic CPU reference Cholesky, inverse-from-factor, and triangular solves. Batched Cholesky/inverse, complex/low-precision types, device pointers, async CUDA stream execution, CUDA graphs, real GPU residency, and bitwise NVIDIA parity remain unsupported. |
| `PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY=1` streams/events | Enables host-side simulated stream/event APIs for `libcuda` and `libcudart`. Driver support includes stream create/destroy/query/synchronize/flags/priority plus event create/destroy/record/query/synchronize/elapsed-time and record-with-flags. Runtime support includes the matching `cudaStream*` and `cudaEvent*` object forms. Streams are registry-validated metadata handles, not command queues. Events are CPU monotonic-time markers, not GPU events. Priorities are metadata-only and normalized to `0`; interprocess events are unsupported; runtime and driver handles are not interchangeable; destroyed or foreign handles fail with invalid-handle errors. This mode still reports zero CUDA devices and does not provide real contexts, context/device-wide synchronization, device priority-range queries, properties, arbitrary module loading/kernels beyond the Psyche-native `vector_add_f32` / `saxpy_f32` / `scale_f32` / `axpby_f32` subset, graphs, or CUDA stream capture. |
| `torch.cuda.is_available()` | Not patched. Remains the true PyTorch CUDA answer. |
| `Tensor.cuda()` / `Module.cuda()` | Not patched. |
| `torch.device("cuda")` | Not patched. It remains a real CUDA device object. |
| `tensor.is_cuda` | Not patched. MPS tensors still report `False`. |
| `tensor.to("cuda")` | Not patched. |
| `torch.empty(..., device="cuda")` | Not patched. |
| Triton, PTX, CUDA extensions, FlashAttention CUDA kernels | Unsupported unless a real Metal/MPS implementation is added. |
| NCCL/FSDP/DTensor distributed CUDA | Unsupported on MPS; current MPS PythonDistributedCausalLM support is single-rank only with Gloo/CPU-staged orchestration. |

The north star is CUDA-shaped ergonomics over real Apple Silicon backends, never
fake NVIDIA parity.
