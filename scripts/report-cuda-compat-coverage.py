#!/usr/bin/env python3
"""Generate a truthful CUDA compatibility coverage ledger for Apple Silicon."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


COMMENT_RE = re.compile(r"//.*?$|/\*.*?\*/", re.DOTALL | re.MULTILINE)
EXPORT_MACRO_RE = re.compile(r"\bPSYCHE_CUDA_STUB_API\b")
EXPORT_RE = re.compile(r"\bPSYCHE_CUDA_STUB_API\s+[^;{()]*?(?:\*|\s)(\w+)\s*\(")
PROC_ADDRESS_TABLE_RE = re.compile(
    r"static\s+const\s+struct\s+Symbol\s+symbols\[\]\s*=\s*\{(?P<body>.*?)\n\s*\};",
    re.DOTALL,
)
PROC_ADDRESS_SYMBOL_RE = re.compile(r'\{\s*"([^"]+)"\s*,')

SOURCE_REFERENCES = [
    {
        "name": "NVIDIA CUDA Driver API",
        "url": "https://docs.nvidia.com/cuda/cuda-programming-guide/03-advanced/driver-api.html",
        "why": "Defines driver API objects such as devices, contexts, modules, functions, memory, streams, and events.",
    },
    {
        "name": "NVIDIA CUDA Driver API memory management",
        "url": "https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MEM.html",
        "why": "Defines driver allocation, copy, fill, async memory, and pinned host-memory functions used by CUDA-header consumers.",
    },
    {
        "name": "NVIDIA CUDA Driver API unified addressing",
        "url": "https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__UNIFIED.html",
        "why": "Defines driver pointer attributes, memory advice, prefetch, and managed-memory range-query surfaces.",
    },
    {
        "name": "NVIDIA CUDA Driver API stream ordered memory allocator",
        "url": "https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MALLOC__ASYNC.html",
        "why": "Defines driver async allocation/free and memory-pool APIs used by modern CUDA probes.",
    },
    {
        "name": "NVIDIA CUDA Driver API stream management",
        "url": "https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__STREAM.html",
        "why": "Defines driver stream creation, query, synchronization, flags, priority, and stream/event ordering surfaces.",
    },
    {
        "name": "NVIDIA CUDA Driver API event management",
        "url": "https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__EVENT.html",
        "why": "Defines driver event creation, record, query, synchronization, and elapsed-time functions.",
    },
    {
        "name": "NVIDIA CUDA Runtime API",
        "url": "https://docs.nvidia.com/cuda/cuda-runtime-api/index.html",
        "why": "Defines the runtime surface that libraries probe through libcudart.",
    },
    {
        "name": "NVIDIA CUDA Runtime API memory management",
        "url": "https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__MEMORY.html",
        "why": "Defines runtime allocation, managed-memory, memory advice, prefetch, range-query, pointer-query, and copy/fill surfaces.",
    },
    {
        "name": "NVIDIA CUDA Runtime API stream ordered memory allocator",
        "url": "https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__MEMORY__POOLS.html",
        "why": "Defines runtime async allocation/free and memory-pool APIs used by modern CUDA probes.",
    },
    {
        "name": "NVIDIA CUDA Runtime API stream management",
        "url": "https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__STREAM.html",
        "why": "Defines runtime stream creation, query, synchronization, flags, priority, and stream/event ordering surfaces.",
    },
    {
        "name": "NVIDIA CUDA Runtime API event management",
        "url": "https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__EVENT.html",
        "why": "Defines runtime event creation, record, query, synchronization, and elapsed-time functions.",
    },
    {
        "name": "NVIDIA cuBLAS API",
        "url": "https://docs.nvidia.com/cuda/cublas/",
        "why": "Defines cuBLAS handle, stream, pointer-mode, math-mode, status, and BLAS operation semantics.",
    },
    {
        "name": "NVIDIA cuSPARSE management API",
        "url": "https://docs.nvidia.com/cuda/cusparse/basic-api/management-reference.html",
        "why": "Defines cuSPARSE handle, stream, pointer-mode, version/property, status, and lifecycle semantics.",
    },
    {
        "name": "NVIDIA cuSPARSE sparse matrix descriptors",
        "url": "https://docs.nvidia.com/cuda/cusparse/generic-api/sparse-matrix.html",
        "why": "Defines CSR sparse matrix descriptor shape, index types, index base, value type, and storage pointers.",
    },
    {
        "name": "NVIDIA cuSPARSE dense vector descriptors",
        "url": "https://docs.nvidia.com/cuda/cusparse/generic-api/dense-vector.html",
        "why": "Defines dense vector descriptor size, value type, pointer, and alignment expectations.",
    },
    {
        "name": "NVIDIA cuSPARSE dense matrix descriptors",
        "url": "https://docs.nvidia.com/cuda/cusparse/generic-api/dense-matrix.html",
        "why": "Defines dense matrix descriptor rows, columns, leading dimension, value type, order, and storage pointer semantics.",
    },
    {
        "name": "NVIDIA cuSPARSE generic API functions",
        "url": "https://docs.nvidia.com/cuda/cusparse/generic-api/generic-api-functions.html",
        "why": "Defines cusparseSpMV and cusparseSpMM operation, algorithm, datatype, workspace, and deterministic-algorithm semantics.",
    },
    {
        "name": "NVIDIA cuSOLVER API",
        "url": "https://docs.nvidia.com/cuda/cusolver/index.html",
        "why": "Defines cuSolverDN dense LAPACK helpers, getrf/getrs and potrf/potri/potrs factorization, inverse, and solve semantics, workspace contracts, pivoting, and devInfo behavior.",
    },
    {
        "name": "Apple Accelerate",
        "url": "https://developer.apple.com/accelerate/",
        "why": "Documents Apple's BLAS/LAPACK support for dense linear algebra, matrix factorization, and solving systems on Apple platforms.",
    },
    {
        "name": "NVIDIA Management Library API",
        "url": "https://docs.nvidia.com/deploy/nvml-api/nvml-api-reference.html",
        "why": "Defines NVML initialization, device-count, system-version, error-string, handle lookup, and device telemetry query semantics.",
    },
    {
        "name": "NVIDIA cuDNN API",
        "url": "https://docs.nvidia.com/deeplearning/cudnn/backend/latest/api/cudnn-graph-library.html",
        "why": "Defines cuDNN status, version/property, handle creation, stream, runtime-error, and graph-version query semantics.",
    },
    {
        "name": "NVIDIA cuDNN ops API",
        "url": "https://docs.nvidia.com/deeplearning/cudnn/backend/latest/api/cudnn-ops-library.html",
        "why": "Defines cudnnActivationForward, cudnnActivationBackward, cudnnAddTensor, cudnnBatchNormalizationForwardInference, cudnnTransformTensor, cudnnPoolingForward, cudnnPoolingBackward, cudnnSoftmaxForward, cudnnSoftmaxBackward, tensor/activation/pooling/batch-normalization descriptor use, softmax algorithms/modes, alpha/beta host scaling, in-place/overlap/broadcast descriptor requirements, output-dimension formula, batch-normalization inference formula and epsilon floor, and operation error semantics.",
    },
    {
        "name": "NVIDIA cuDNN CNN API",
        "url": "https://docs.nvidia.com/deeplearning/cudnn/backend/latest/api/cudnn-cnn-library.html",
        "why": "Defines legacy convolution descriptors, filter descriptors, cudnnConvolutionForward, cudnnConvolutionBiasActivationForward, forward algorithm query helpers, forward workspace-size query, KCRS filter layout, convolution/cross-correlation modes, fused bias/add/activation semantics, and convolution output-dimension semantics.",
    },
    {
        "name": "PyTorch MPS backend",
        "url": "https://docs.pytorch.org/docs/2.12/notes/mps.html",
        "why": "Documents MPS as a Metal-backed PyTorch device, not CUDA.",
    },
    {
        "name": "PyTorch MPS environment variables",
        "url": "https://docs.pytorch.org/docs/2.12/mps_environment_variables.html",
        "why": "Documents PYTORCH_ENABLE_MPS_FALLBACK CPU fallback behavior.",
    },
    {
        "name": "Apple Metal Performance Shaders Graph",
        "url": "https://developer.apple.com/documentation/metalperformanceshadersgraph",
        "why": "Primary Apple API surface for graph-level GPU computation on Apple platforms.",
    },
]

SUPPORT_LEVELS = {
    "python_redirect_to_mps": "Psyche-owned Python device intent is redirected to a real MPS device when explicitly enabled.",
    "mps_exact_route": "Exact PyTorch ATen compatibility route registered at the MPS dispatch key.",
    "native_discovery_stub": "Native CUDA-shaped symbol exists for discovery/linkage but reports no usable CUDA device.",
    "native_hard_fail_stub": "Native CUDA-shaped symbol exists but rejects unsupported module-file loading, device/context-wide synchronization or priority-range queries, or other unsupported runtime work.",
    "native_simulated_driver_memory_op": "Native CUDA Driver API allocation/pitched-allocation/managed-allocation/advice/prefetch/range-query/pointer-query/copy/2D-copy/3D-copy/fill/2D-fill/pinned-host-memory symbol is bounded to CPU memory under explicit opt-in; async forms run synchronously on the null/zero stream or a shim-created stream, managed-memory migration is not modeled, and these symbols do not report a CUDA device, real context, module, or kernel.",
    "native_simulated_driver_mempool_op": "Native CUDA Driver API stream-ordered allocation/free and host/no-location memory-pool symbols are bounded to CPU memory under explicit opt-in. Device-default pool APIs reject absent CUDA devices, IPC/export/access-control paths are exported but unsupported, and no CUDA context, graph allocator, or GPU-resident pool is reported.",
    "native_simulated_driver_sync_op": "Native CUDA Driver API stream/event/query/synchronization symbol is a host-side shim object under explicit opt-in. Streams are registry-validated metadata handles, events are CPU monotonic-time markers, and these symbols do not report a CUDA device, real context, module, kernel, graph, or GPU timing support.",
    "native_simulated_driver_kernel_op": "Native CUDA Driver API module/function/launch symbols accept a Psyche-native module blob and run registered vector_add_f32, saxpy_f32, scale_f32, and axpby_f32 kernels over simulated driver allocations under explicit opt-in. The default path is a CPU reference kernel; on Apple Silicon, PSYCHE_CUDA_COMPAT_METAL_KERNELS=required verifies real Metal shared-buffer dispatch for those registered kernels through copy-in/copy-back staged buffers. Exact aliases are supported for tested in-place/output cases, and partial overlaps involving a mutated span fall back to CPU in preferred Metal mode or return unsupported in required Metal mode. Raw PTX/CUBIN, arbitrary kernels, multidimensional launches, dynamic shared memory, extra launch config, real CUDA streams, real contexts, and general CUDA execution are not modeled yet.",
    "native_simulated_runtime_memory_op": "Native CUDA runtime-shaped allocation/pitched-allocation/managed-allocation/advice/prefetch/range-query/pointer-query/copy/2D-copy/3D-copy/fill/2D-fill/pinned-host-memory symbol is bounded to CPU memory: no CUDA device is reported, HostToHost, HostToHost 2D, and linear HostToHost 3D copies can run by default, async forms run synchronously on the null/zero stream or a shim-created stream, managed-memory migration is not modeled, and device-direction memory operations require explicit opt-in simulated memory.",
    "native_simulated_runtime_mempool_op": "Native CUDA Runtime API stream-ordered allocation/free and host/no-location memory-pool symbols are bounded to CPU memory under explicit opt-in. Device-default pool APIs reject absent CUDA devices, IPC/export/access-control paths are exported but unsupported, and no CUDA context, graph allocator, or GPU-resident pool is reported.",
    "native_simulated_runtime_sync_op": "Native CUDA runtime stream/event/query/synchronization symbol is a host-side shim object under explicit opt-in. Streams are registry-validated metadata handles, events are CPU monotonic-time markers, and no CUDA device, kernel, graph, or GPU timing support is reported.",
    "native_simulated_runtime_kernel_op": "Native CUDA Runtime API cudaLaunchKernel accepts exported Psyche runtime kernel token functions for vector_add_f32, saxpy_f32, scale_f32, and axpby_f32 over simulated runtime allocations under explicit opt-in. The path is CPU reference execution, accepts the null/default stream, validates non-null runtime-owned stream handles, requires a CUDA-shaped void** args array with the expected parameter slots, stages mutating multi-input outputs so exact and partial allocation overlaps use original inputs deterministically, keeps scale_f32 as an intentional single-buffer element-local in-place update, rejects dynamic shared memory and multidimensional launches, and does not model CUDA fatbins, device-side function registration, PTX/CUBIN ingestion, arbitrary kernels, real CUDA streams, real contexts, or GPU-resident execution.",
    "native_simulated_cublas_op": "Native cuBLAS-shaped symbol is available under explicit simulated-memory opt-in. Calls return not-initialized while the opt-in is disabled. Handles, stream/pointer/math/atomics metadata, status helpers, vector/matrix transfer helpers, bounded FP32/FP64 real Level-1 AXPY/COPY/DOT/SCAL/ROT/ROTG/ROTM/ROTMG/SWAP/ASUM/NRM2/IAMAX/IAMIN plus bounded FP32/FP64 complex Level-1 AXPY/COPY/DOTU/DOTC/SCAL/real-SCAL/SWAP, FP32/FP64 complex GEMV/GERU/GERC/HEMV/HER/HER2/TRMV/TRSV/TRMM/TRSM, FP32/FP64 GEMV/GER/SYMV/SYR/SYR2/TRMV/TRSV/TRMM/TRSM/SYMM/SYRK/SYR2K, FP32/FP64 real GEMM plus real GEMM batched/strided-batched, and FP32/FP64 complex GEMM/HERK/HER2K/TRMM/TRSM plus complex GEMM batched/strided-batched run on host-accessible CPU pointers, including simulated runtime pointers. On Darwin, FP32/FP64 real/complex GEMM, FP32/FP64 real/complex GEMM batched/strided-batched batch entries, and FP32/FP64 real/complex TRMM/TRSM use Accelerate/vecLib CBLAS after the shim's cuBLAS-shaped validation. GEMM keeps temporary output staging plus alpha-zero and k-zero no-read fallbacks; TRMM keeps alpha-zero no-read guards and B-to-C staging. Non-Darwin builds keep the reference loops. Version/property helpers return zero. cublasSet/GetVector/Matrix and async variants copy host-accessible byte spans synchronously; stream arguments are metadata only, positive element sizes, vector strides, and matrix leading dimensions are required, matrix leading dimensions must be at least rows when rows > 0, zero-work calls may omit source/destination pointers, staged copies make overlap deterministic, and no real CUDA transfer or stream synchronization is modeled. Nonzero signed vector strides are supported for GEMV, GER, HEMV, HER, HER2, SYMV, SYR, SYR2, TRMV, and TRSV; positive vector strides are required when work is required for mutating Level-1 vector ops, DOT/DOTU/DOTC, ROT, and ROTM; GEMM strided-batched permits zero A/B strides to broadcast shared input matrices across batches, rejects negative batch strides, and rejects zero C stride when multiple output batches would overlap; zero-batch GEMM batched/strided-batched calls do not require matrices but still validate scalar pointers; DOT/DOTU/DOTC write zero for n <= 0 after result-pointer validation; ASUM/NRM2/IAMAX/IAMIN follow cuBLAS by returning zero for n <= 0 or incx <= 0. Complex DOTC conjugates the first vector; complex SCAL uses cuComplex/cuDoubleComplex ABI-compatible real/imag structs, while Csscal/Zdscal apply real scalar factors to complex vectors. Complex GEMV, TRMV, TRSV, TRMM, and TRSM honor CUBLAS_OP_C as conjugate transpose; complex GERU uses y as-is; complex GERC conjugates y; complex HEMV reads Hermitian diagonal elements as real without mutating A; complex HER/HER2/HERK/HER2K update only the requested stored triangle and force updated diagonal imaginary parts to zero; complex HERK/HER2K accept CUBLAS_OP_N, CUBLAS_OP_T, and CUBLAS_OP_C, treat OP_T as non-conjugate transpose, avoid reading C when beta == 0, and avoid reading A/B when alpha == 0 or k == 0; complex GEMM honors CUBLAS_OP_C as conjugate transpose, stages C output, and applies alpha/beta with beta == 0 no-read semantics. ROT stages original x/y inputs before writes but has no arbitrary output-overlap guarantee. ROTM applies real FP32/FP64 host-param modified Givens transforms for flags -2/-1/0/1, stages original x/y inputs before writes, rejects undefined flags, and has no arbitrary output-overlap guarantee. ROTMG constructs real FP32/FP64 host scalar modified Givens parameters using Netlib scaling rules, writes the flag plus relevant parameter entries, leaves flag-implied entries unchanged, and has no arbitrary scalar-aliasing guarantee. ROTG constructs real FP32/FP64 host scalar Givens parameters and overwrites a/b/c/s using Netlib-compatible r/z/c/s rules, but does not guarantee arbitrary aliasing among scalar output pointers. SYRK/SYR2K and complex HERK/HER2K update only the requested stored C triangle and leave the opposite triangle untouched; beta == 0 avoids reading C input storage, and alpha == 0 or k == 0 avoids reading product inputs where cuBLAS permits. TRMM supports the cuBLAS C == B in-place idiom but no other overlap guarantee; TRSM overwrites B and does not pre-test triangular singularity; SYMM/SYRK/SYR2K/HERK/HER2K have no arbitrary overlap guarantee. Device pointer-mode scalars/results and tensor/TF32 math modes are metadata-only; operations requiring host scalar/result pointers, including AXPY, DOT/DOTU/DOTC, SCAL, ROT, ROTG, ROTM, ROTMG, reductions, GEMV, GER, HEMV, HER, HER2, HERK, HER2K, SYMV, SYR, SYR2, TRMM, TRSM, SYMM, SYRK, SYR2K, GEMM, and GEMM batched/strided-batched, return not-supported for device pointer mode, while scalar-free COPY/SWAP/TRMV/TRSV are not blocked by pointer mode. GEMM paths return not-supported for tensor/TF32 math modes. Stream handles are not validated or synchronized. The shim does not validate arbitrary pointer provenance; callers must pass valid host-accessible CPU buffers large enough for the requested shapes, strides, and batches. Pointer-array batched GEMM additionally requires CPU-addressable pointer arrays and pointed-to matrices, and non-overlapping C batches are a caller precondition that is not enforced. Non-_v2 symbols use the current handle-based cuBLAS ABI shape as aliases for the matching _v2 implementations; legacy cuBLAS v1 by-value/no-handle ABI is not modeled. Arbitrary real CUDA/UVA/foreign pointers, tensor cores, CUDA kernels, cuBLASLt, complex Level-2 outside GEMV/GERU/GERC/HEMV/HER/HER2/TRMV/TRSV and complex Level-3 outside GEMM/HERK/HER2K/TRMM/TRSM paths, half/TF32 paths, bitwise NVIDIA cuBLAS parity, and real GPU execution are not modeled.",
    "native_simulated_cublaslt_op": (
        "Native cuBLASLt-shaped symbol is available under explicit simulated-memory opt-in. "
        "Calls return not-initialized while the opt-in is disabled. Handles, fixed-size public "
        "opaque descriptors, descriptor Init/Create/Destroy, descriptor Set/GetAttribute, "
        "preference Set/GetAttribute, matrix-transform descriptors, status/version/property helpers, "
        "algorithm ID/init/check, heuristic query, cublasLtMatmul, and cublasLtMatrixTransform "
        "are implemented for bounded FP32/FP64 real GEMM and transform paths "
        "with CUBLASLT_ORDER_COL and CUBLASLT_ORDER_ROW matrix layouts over host-accessible CPU "
        "pointers. On Darwin, supported all-column-major FP32/FP64 matmul batches use "
        "Accelerate/vecLib CBLAS for the raw GEMM core after cuBLASLt-shaped validation; "
        "cuBLASLt epilogues are applied afterward by CPU postprocessing. Row-major or "
        "mixed-order layouts use the reference-loop GEMM core. Non-Darwin builds keep the "
        "reference-loop implementation for all supported layouts. Supported matmuls require "
        "matching A/B/C/D data types, matching compute/scale type, host pointer mode, "
        "column-major or row-major layouts, DEFAULT, RELU, RELU_AUX, BIAS, RELU_BIAS, "
        "RELU_AUX_BIAS, DRELU, DRELU_BGRAD, GELU, GELU_BIAS, GELU_AUX, GELU_AUX_BIAS, "
        "DGELU, DGELU_BGRAD, BGRADA, or BGRADB epilogue, full fill mode, and "
        "transc == CUBLAS_OP_N. "
        "MatrixTransform supports host pointer mode, FP32 or FP64 scale type, FP32/FP64 real "
        "A/B/C layouts with scale-type conversion and output conversion, CUBLAS_OP_N/T/C "
        "where OP_C is real transpose, alpha-zero and beta-zero no-read behavior, strided "
        "batches with input batch-count-one broadcast to C's batch count, and pointer-array "
        "batches when participating A/B/C descriptors all use pointer-array mode with exact "
        "batch counts. Required pointer-array entries are preflighted before any C batch is "
        "written. Unsafe C/source byte-range overlap is rejected, while exact same-layout "
        "no-transpose in-place sources are allowed. "
        "The BIAS, RELU_BIAS, RELU_AUX_BIAS, GELU_BIAS, and GELU_AUX_BIAS epilogues "
        "use a caller-owned CPU-resident host-accessible bias pointer with bias dtype "
        "unset/default or matching D, bias length equal to D rows, bias[row] applied after "
        "alpha*A@B + beta*C and before optional ReLU or GELU, and BIAS_BATCH_STRIDE in "
        "elements for per-batch bias vectors; stride zero broadcasts the same bias vector, "
        "and positive stride must be at least D rows. RELU_AUX and RELU_AUX_BIAS write a "
        "ReLU bit-mask AUX buffer using logical bit index row + col * AUX_LD; NVIDIA "
        "documents ReLU-mask AUX_LD and AUX_BATCH_STRIDE as bits, divisible by 128, with "
        "AUX_LD >= D rows, so positive strides must cover AUX_LD * D.cols bits. Within each "
        "byte the host bridge uses an LSB-first convention covered by fixed-byte tests but "
        "still needing a real NVIDIA AUX-buffer diff before claiming bit-for-bit interchange; "
        "TODO replace this convention note once a real NVIDIA AUX byte diff is available. "
        "DRELU and DRELU_BGRAD read that same bit-mask and write raw dy where the mask bit "
        "is set, otherwise zero. ReLU-mask epilogues reject non-default AUX data types "
        "because the AUX buffer is not a typed matrix. Following cuBLASLt's independent "
        "ReLu and Bias-gradient wording, DRELU_BGRAD writes an independent raw-dy row-wise "
        "bias-gradient output, with FP32 reductions accumulated in FP64. GELU_AUX and "
        "GELU_AUX_BIAS write the "
        "pre-GELU logical output matrix to a caller-owned CPU-resident host-accessible AUX "
        "pointer after optional bias and before GELU, using column-major AUX indexing with "
        "AUX_LD in elements. DGELU and DGELU_BGRAD read that same logical column-major AUX "
        "matrix as the saved GELU preactivation input, multiply raw alpha*A@B + beta*C by "
        "the derivative of the documented tanh GELU approximation, and write D. "
        "DGELU_BGRAD also writes a bias-gradient vector of length D rows where each entry is "
        "the row-wise sum of raw alpha*A@B + beta*C before DGELU multiplication; FP32 "
        "bias-gradient reductions accumulate in FP64 before storing the FP32 output. "
        "Multi-batch DGELU_BGRAD requires a positive BIAS_BATCH_STRIDE at least D rows. "
        "BGRADA and BGRADB write operand-source bias-gradient vectors without alpha/beta "
        "scaling: BGRADA writes length D.rows with bias[row] = sum_k op(A)[row,k], and "
        "BGRADB writes length D.cols with bias[col] = sum_k op(B)[k,col]. The reduced "
        "source operand is required even when alpha is zero; the other source operand keeps "
        "the alpha-zero no-read behavior. Positive BIAS_BATCH_STRIDE must cover the selected "
        "gradient length, and multi-batch BGRADA/BGRADB reject stride-zero broadcast. FP32 "
        "operand-gradient reductions accumulate in FP64 before storing FP32 output. "
        "The DGELU derivative is gradient-consistent with this shim's tanh-approximation "
        "GELU/GELU_AUX paths, not with exact-erf GELU. AUX dtype must be unset/default or "
        "matching D, AUX_LD must be divisible by 8 elements, at least D rows, "
        "and within the backend indexing ceiling, positive AUX_BATCH_STRIDE values must cover "
        "AUX_LD * D.cols, and zero AUX stride is rejected for multi-batch AUX writes or reads "
        "that would alias per-batch state. Runtime AUX/D range overlap is rejected before "
        "either buffer is written; bias-gradient epilogues also reject D/bias-gradient "
        "overlap before writes, DRELU_BGRAD/DGELU_BGRAD reject AUX/bias-gradient overlap, "
        "and BGRADA/BGRADB reject reduced-source/bias-gradient overlap. BGRADA/BGRADB "
        "currently keep D as the raw DEFAULT matmul output on the host bridge and emit a "
        "one-time runtime warning for that unverified D-output behavior; this and reduction "
        "order still need a real NVIDIA hardware byte diff before claiming bitwise parity. "
        "ReLU epilogues clamp each logical output "
        "with CUDA-style value > 0 ? value : 0 semantics before the D write. GELU epilogues "
        "apply NVIDIA's documented tanh approximation, propagate NaN, map +Inf to +Inf, and "
        "map -Inf to 0. DGELU propagates NaN, maps +Inf derivative to 1, and maps -Inf "
        "derivative to 0. Row-major D support for DGELU epilogues is an intentional shim "
        "compatibility extension beyond NVIDIA's documented row-major restriction. Bias and "
        "AUX pointers are validated for non-null state and element-size alignment but not "
        "lifetime, bounds, or arbitrary pointer provenance beyond the overlap checks. Matrix "
        "layout order changes validate leading dimension against the selected order and failed "
        "writes do not mutate descriptor order; unsupported epilogue, bias-data-type, or "
        "aux-data-type writes fail without mutating descriptor state. CUBLAS_OP_C is accepted "
        "for real inputs and executes as non-conjugating transpose. D may be distinct from C; "
        "beta == 0 avoids reading C and allows a null C pointer with a valid C descriptor. "
        "Strided-batch layout attributes are supported when batch counts are compatible and "
        "D/AUX/bias-gradient batches do not overlap. Heuristics return one zero-workspace algo "
        "for supported configs and success with zero algos for unsupported configs. Workspace "
        "and stream arguments are accepted as metadata only. Pointer-array batch layout mode "
        "is supported for DEFAULT-only matmuls when A/B/C/D descriptors all use "
        "CUBLASLT_BATCH_MODE_POINTER_ARRAY with matching batch counts; the A/B/C/D API "
        "arguments are CPU-addressable arrays of host-accessible matrix pointers, strided "
        "batch offsets are ignored in that mode, required null entries are rejected before "
        "any D batch is written, and non-DEFAULT pointer-array epilogues return no heuristic "
        "and fail execution as not-supported. Tiled layouts, half/BF16/FP8/"
        "complex/int data types, TF32/tensor modes, device/vector pointer modes, "
        "AUX scale/amax outputs, grouped batches, real CUDA async "
        "semantics, arbitrary CUDA/UVA pointers, and "
        "bitwise NVIDIA cuBLASLt parity are not modeled."
    ),
    "native_simulated_cusparse_spmv_op": (
        "Native cuSPARSE-shaped symbol is available under explicit simulated-memory opt-in. "
        "Calls return not-initialized while the opt-in is disabled. Handles, stream and "
        "pointer-mode metadata, version/property/status helpers, CSR sparse-matrix "
        "descriptors, dense-vector descriptors, cusparseSpMV_bufferSize, and cusparseSpMV "
        "are implemented for bounded FP32 CSR SpMV over host-accessible CPU pointers. "
        "The supported execution subset is opA == NON_TRANSPOSE, CUDA_R_32F matrix/vector/"
        "compute types, matching 32-bit or 64-bit row/column indices on the CPU route, "
        "zero-based or one-based CSR, host alpha/"
        "beta scalars, DEFAULT/CSR_ALG1/CSR_ALG2 algorithms, and zero caller workspace "
        "because the shim uses internal staging. The bridge validates descriptor lifetimes, "
        "alignment, dimensions, row-offset monotonicity, row-offset endpoints, column ranges, "
        "output/source overlap, and unsupported pointer modes before mutating output. The CPU "
        "reference path computes into a temporary y buffer and copies back only after success. "
        "On Darwin, PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=required verifies a real Metal "
        "shared-buffer CSR SpMV route for the 32-bit-index subset and returns the Metal-derived status "
        "instead of falling back when unavailable; PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=1 "
        "prefers Metal and falls back only for backend-availability failures. The Metal route "
        "currently requires 32-bit row/column indices, stages CSR, x, and prior y into "
        "shared buffers, computes one output row per thread into a separate y buffer, and "
        "copies y back only after command-buffer completion, so required-mode launch failures "
        "leave y unchanged. In required-Metal mode, 64-bit CSR indices return not-supported "
        "without CPU fallback until the local Metal toolchain can prove an MSL 64-bit-index "
        "kernel. Transpose/conjugate transpose, "
        "device pointer-mode scalars, non-CSR formats, 16-bit indices, non-FP32/"
        "complex/low-precision values, preprocess/updateMatrix, broader SpMM forms, SpSV/SpSM, dense/sparse "
        "conversion, batched sparse APIs, external workspace semantics, CUDA streams, CUDA "
        "graphs, asynchronous behavior, and bitwise NVIDIA cuSPARSE parity are not modeled."
    ),
    "native_simulated_cusparse_spmm_op": (
        "Native cuSPARSE-shaped cusparseCreateDnMat, cusparseCreateConstDnMat, "
        "cusparseDestroyDnMat, cusparseSpMM_bufferSize, and cusparseSpMM are available "
        "under explicit simulated-memory opt-in for a bounded FP32 CSR SpMM subset over "
        "host-accessible CPU pointers. The supported execution subset is opA == "
        "NON_TRANSPOSE, opB == NON_TRANSPOSE, CUDA_R_32F sparse/dense/compute types, "
        "matching 32-bit or 64-bit CSR row/column indices, zero-based or one-based CSR, "
        "host alpha/beta scalars, CUSPARSE_ORDER_COL or CUSPARSE_ORDER_ROW dense B/C "
        "matrices with leading-dimension validation, and DEFAULT/CSR_ALG1/CSR_ALG2/"
        "CSR_ALG3 algorithms. The workspace query returns zero bytes because the shim "
        "uses internal staging; it validates descriptor and contract metadata, and "
        "required-Metal mode also applies the 32-bit-index/uint-limit Metal supportability "
        "preflight, but it does not read CSR row/column contents. Execution validates descriptor lifetimes, dimensions, "
        "dense layouts, CSR row-offset endpoints and monotonicity, column ranges, "
        "alignment, B/C aliasing, and C overlap with CSR storage before mutating C. "
        "The CPU path computes into a temporary logical C buffer and writes back only "
        "after validation and execution succeed; beta == 0 avoids reading prior C. "
        "On Darwin, PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=required verifies a real Metal "
        "shared-buffer CSR SpMM route for the 32-bit-index subset and returns the "
        "Metal-derived status instead of falling back when unavailable; "
        "PSYCHE_CUDA_COMPAT_CUSPARSE_METAL=1 prefers Metal and falls back only for "
        "backend-availability failures. The Metal route stages CSR, B, and prior C "
        "when beta is nonzero, computes compact logical C into a separate shared "
        "buffer, and copies C back only after command-buffer completion. Required-Metal "
        "64-bit CSR indices return not-supported without CPU fallback until the local "
        "Metal toolchain can prove an MSL 64-bit-index kernel. "
        "Transpose/conjugate transpose, device pointer-mode scalars, non-CSR formats, "
        "16-bit or mixed CSR index widths, non-FP32/complex/low-precision values, COO/"
        "Blocked-ELL/BSR SpMM algorithms, preprocess/updateMatrix, external workspace "
        "semantics, CUDA streams, CUDA graphs, asynchronous behavior, real GPU execution "
        "outside the explicit 32-bit-index Metal CSR SpMM route, and bitwise NVIDIA "
        "cuSPARSE parity are not modeled."
    ),
    "native_simulated_cusolver_dense_lu_op": (
        "Native cuSOLVER-shaped libcusolver symbols are available under explicit "
        "simulated-memory opt-in. Calls return not-initialized while the opt-in is "
        "disabled. cuSolverDN handles, stream metadata, version/property helpers, "
        "status helpers, cusolverDnSgetrf_bufferSize, cusolverDnDgetrf_bufferSize, "
        "cusolverDnSgetrf, cusolverDnDgetrf, cusolverDnSgetrs, and "
        "cusolverDnDgetrs are implemented for bounded FP32/FP64 dense "
        "column-major LU factorization and solve over host-accessible CPU pointers. "
        "On Darwin, pivoted getrf/getrs calls are Accelerate/LAPACK-backed after "
        "cuSOLVER-shaped validation, preserving LAPACK/cuSOLVER 1-based pivot "
        "indices and singular-factorization devInfo > 0 success semantics. "
        "getrf_bufferSize returns the shim's required m*n element workspace; "
        "execution validates a non-null workspace when work is required, but "
        "Accelerate itself does not consume caller workspace. devIpiv == NULL "
        "uses a deterministic no-pivot reference LU route, and matching getrs "
        "with NULL pivots treats the factors as unpivoted; a zero diagonal during "
        "that no-pivot solve returns CUSOLVER_STATUS_EXECUTION_FAILED with positive "
        "devInfo and leaves B unchanged. Mutable A/B buffers are staged and copied "
        "back only after validation and successful execution or successful getrf "
        "singular factorization, so invalid parameters, allocation failures, and "
        "failed no-pivot solves leave caller data unchanged. Stream arguments are metadata-only. "
        "Device-resident pointers, asynchronous CUDA stream ordering, CUDA graphs, "
        "batched getrf/getrs, sparse cuSOLVER, QR/eigen/SVD/IRS/RF/Mg APIs, "
        "complex/low-precision datatypes, real GPU execution, and bitwise NVIDIA "
        "cuSOLVER parity are not modeled."
    ),
    "native_simulated_cusolver_dense_cholesky_op": (
        "Native cuSOLVER-shaped Cholesky symbols are available under explicit "
        "simulated-memory opt-in for bounded FP32/FP64 dense column-major "
        "host-accessible pointers. cusolverDnS/Dpotrf_bufferSize and "
        "cusolverDnS/Dpotri_bufferSize return the shim's conservative n*n element "
        "workspace. cusolverDnS/Dpotrf and cusolverDnS/Dpotri validate that "
        "workspace, then stage A and run over the requested lower or upper "
        "triangle, leaving the opposite triangle untouched. On Darwin the "
        "factorization, inverse, and cusolverDnS/Dpotrs solve path route through "
        "Accelerate/LAPACK after explicit cublasFillMode_t-to-LAPACK uplo "
        "translation; non-Darwin builds use deterministic CPU reference "
        "Cholesky, inverse-from-factor, and triangular solves. potrf copies back "
        "only successful factors; positive-devInfo non-positive-definite failures "
        "leave caller A unchanged because partial factors are not guaranteed useful. "
        "potri consumes an existing Cholesky factor rather than refactorizing, "
        "copies back only the requested inverse triangle on success, returns "
        "success with positive devInfo for exact-zero Cholesky diagonals, and "
        "leaves A unchanged on validation/allocation/singularity failures. potrs "
        "stages B and copies back only on success; exact-zero diagonal potrs "
        "failure is a shim-safety behavior, not a real cuSOLVER guarantee. "
        "Device pointers, "
        "async CUDA streams, CUDA graphs, complex/low-precision datatypes, "
        "batched Cholesky, sparse cuSOLVER, QR/eigen/SVD/IRS/RF/Mg APIs, real "
        "GPU execution, and bitwise NVIDIA cuSOLVER parity are not modeled."
    ),
    "native_nvml_discovery_stub": "Native NVML-shaped libnvidia-ml symbol is available for dependency probes. The compatibility library initializes with NVML refcount semantics, exposes parseable stub version strings and nvmlErrorString helpers, reports zero NVIDIA devices through nvmlDeviceGetCount[_v2], keeps PyTorch's NVML-based CUDA availability check on the false/no-device path, and rejects handle lookup or device telemetry without synthesizing Apple GPU identity, clocks, power, process, or utilization as NVIDIA telemetry.",
    "native_cudnn_discovery_stub": "Native cuDNN-shaped libcudnn symbol is available for dependency probes. Version, CUDART-version, max-device, property, and error-string helpers return truthful zero/stub discovery values; by default cudnnCreate clears the output handle and returns not-initialized because no compatible CUDA/NVIDIA backend exists. Under explicit PSYCHE_CUDA_COMPAT_SIMULATED_MEMORY opt-in, cudnnCreate mints an opaque shim handle for the bounded simulated cuDNN descriptor, activation, add-tensor, batch-normalization inference, convolution-forward, convolution-backward-data, convolution-backward-filter, tensor-transform, pooling, and softmax subset, while stream and runtime-error helpers remain metadata-only and graph-version entry points hard-fail without promising batch-normalization training/backward, graph, or general cuDNN kernel execution.",
    "native_simulated_cudnn_activation_op": "Native cuDNN-shaped tensor/activation descriptor symbols, cudnnActivationForward, and cudnnActivationBackward are available under explicit simulated-memory opt-in. This bounded operation bridge supports contiguous 4D NCHW FP32 tensor descriptors, ReLU/Sigmoid/Tanh/Identity activation descriptors with ignored coefficients, CUDNN_PROPAGATE_NAN and CUDNN_NOT_PROPAGATE_NAN behavior, host alpha/beta scalars, forward y = alpha * activation(x) + beta * y, and backward dx = alpha * activation'(x) * dy + beta * prior_dx over host-accessible CPU pointers. Backward validates y/yDesc but computes derivatives from x because forward y may have been alpha/beta blended; callers must not treat this as bitwise cuDNN parity for implementations that derive sigmoid/tanh gradients from saved raw activation output y. NOT_PROPAGATE_NAN sanitizes NaN x to zero before computing the local derivative, while dy NaNs still propagate through arithmetic. Beta-zero backward does not read prior dx. By default this path runs a CPU reference loop. On Darwin, PSYCHE_CUDA_COMPAT_CUDNN_METAL=required verifies real Metal shared-buffer dispatch for the same subset and returns the Metal-derived failure without CPU fallback when the backend is unavailable; PSYCHE_CUDA_COMPAT_CUDNN_METAL=1 prefers Metal and falls back only for backend-availability failures. The Metal forward route stages x and old y into separate shared buffers, computes into a separate output buffer, and copies y back only after command-buffer completion, so required-mode launch failures leave y unchanged. The Metal backward route stages x, dy, and old dx into separate shared buffers, computes one dx element per thread into a separate output buffer, and copies dx back only after command-buffer completion, so required-mode launch failures leave dx unchanged. Exact x == y in-place forward activation is supported when descriptor values match; exact dy == dx backward activation is supported when descriptor values match, with staging to preserve dy/prior dx. Partial x/y forward overlap, partial dy/dx backward overlap, and backward x/dx or y/dx overlap are rejected. NHWC, non-FP32, non-contiguous/custom-stride, clipped ReLU, ELU, swish, device pointer-mode scalars, CUDA streams, real cuDNN async semantics, and convolution/normalization/graph execution are not modeled.",
    "native_simulated_cudnn_add_op": "Native cuDNN-shaped cudnnAddTensor is available under explicit simulated-memory opt-in. This bounded operation bridge supports contiguous 4D NCHW FP32 tensor descriptors where each source A dimension either equals the corresponding destination C dimension or is 1, including the common 1xCx1x1 bias-add broadcast into NxCxHxW and same-shape adds. It computes C = alpha * A_broadcast + beta * prior_C over host-accessible CPU pointers, rejects any A/C byte-range overlap including exact A == C, avoids reading prior C when beta is zero, and still evaluates alpha * A so alpha-zero NaN source values propagate. By default this path runs a CPU reference loop. On Darwin, PSYCHE_CUDA_COMPAT_CUDNN_METAL=required verifies real Metal shared-buffer dispatch for the same subset and returns the Metal-derived failure without CPU fallback when the backend is unavailable; PSYCHE_CUDA_COMPAT_CUDNN_METAL=1 prefers Metal and falls back only for backend-availability failures. The Metal route stages A and old C into separate shared buffers, skips old-C staging reads when beta is zero, computes one C element per thread into a separate output buffer, and copies C back only after command-buffer completion, so required-mode launch failures leave C unchanged. 5D tensors, NHWC, non-FP32, non-contiguous/custom-stride, arbitrary layout conversion, aliased A/C storage, device pointer-mode scalars, CUDA streams, and real cuDNN async semantics are not modeled.",
    "native_simulated_cudnn_batchnorm_inference_op": "Native cuDNN-shaped cudnnBatchNormalizationForwardInference is available under explicit simulated-memory opt-in. This bounded legacy operation bridge supports contiguous 4D NCHW FP32 x/y tensor descriptors with matching dimensions and FP32 batch-normalization parameter descriptors for CUDNN_BATCHNORM_SPATIAL (1xCx1x1) and CUDNN_BATCHNORM_PER_ACTIVATION (1xCxHxW). It computes y = beta * prior_y + alpha * (bnBias + bnScale * (x - estimatedMean) / sqrt(epsilon + estimatedVariance)) over host-accessible CPU pointers, rejects epsilon below CUDNN_BN_MIN_EPSILON, rejects SPATIAL_PERSISTENT, rejects wrong parameter descriptor shapes, rejects partial x/y overlap while allowing exact x == y in-place inference, rejects x/y overlap with parameter/stat buffers and parameter/stat buffer overlap, avoids reading prior y when beta is zero, and still evaluates the normalized result so alpha-zero source/parameter NaNs propagate naturally. Estimated variance is not clamped or validated beyond the formula, so negative variance can naturally yield NaN-domain outputs. By default this path runs a CPU reference loop. On Darwin, PSYCHE_CUDA_COMPAT_CUDNN_METAL=required verifies real Metal shared-buffer dispatch for the same subset and returns the Metal-derived failure without CPU fallback when the backend is unavailable; PSYCHE_CUDA_COMPAT_CUDNN_METAL=1 prefers Metal and falls back only for backend-availability failures. The Metal route stages x, old y when beta is nonzero, scale, bias, mean, and variance into shared buffers, computes one y element per thread into a separate output buffer, and copies y back only after command-buffer completion, so required-mode launch failures leave y unchanged. Training forward, backward, SPATIAL_PERSISTENT, 5D tensors, NHWC, non-FP32, non-contiguous/custom-stride, broader normalization APIs, device pointer-mode scalars, CUDA streams, and real cuDNN async semantics are not modeled.",
    "native_simulated_cudnn_convolution_forward_op": "Native cuDNN-shaped filter/convolution descriptor symbols, cudnnGetConvolution2dForwardOutputDim, cudnnGetConvolutionForwardAlgorithmMaxCount, cudnnGetConvolutionForwardAlgorithm, cudnnGetConvolutionForwardAlgorithm_v7, cudnnFindConvolutionForwardAlgorithm, cudnnGetConvolutionForwardWorkspaceSize, and cudnnConvolutionForward are available under explicit simulated-memory opt-in. This bounded legacy operation bridge supports contiguous 4D NCHW FP32 x/y tensor descriptors, contiguous FP32 NCHW/KCRS filter descriptors using cuDNN grouped semantics (groupCount > 0, full x/y descriptors, filter C = input_C/groupCount, input_C % groupCount == 0, and K % groupCount == 0), CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM, CUDNN_CROSS_CORRELATION, and CUDNN_CONVOLUTION with spatial R/S filter flipping. Depthwise and depthwise-multiplier cases are covered when they satisfy the same grouped descriptor rules. Algorithm queries validate the same bounded descriptor configuration as forward execution, report exactly one deterministic zero-workspace IMPLICIT_GEMM algorithm, and do not claim alternate algorithms. The workspace query returns zero bytes only for that same executable path. It computes y = alpha * conv_or_correlation(x, w) + beta * prior_y over host-accessible CPU pointers using cuDNN's 2D padding/stride/dilation output-dimension formula, requires yDesc to match the computed N,K,H,W dimensions, accepts but does not dereference workspace for the supported algorithm, rejects unsupported algorithms and invalid grouped descriptor shapes, rejects any x/y/w byte-range overlap including exact aliases while allowing adjacent non-overlap, avoids reading prior y when beta is zero, and still evaluates the convolution result so alpha-zero source/filter NaNs propagate under the shim's literal formula convention. By default this path runs a CPU reference loop. On Darwin, PSYCHE_CUDA_COMPAT_CUDNN_METAL=required verifies real Metal shared-buffer dispatch for the same subset and returns the Metal-derived failure without CPU fallback when the backend is unavailable; PSYCHE_CUDA_COMPAT_CUDNN_METAL=1 prefers Metal and falls back only for backend-availability failures. The Metal route stages x, w, and old y when beta is nonzero into separate shared buffers, computes one y element per thread into a separate output buffer, and copies y back only after command-buffer completion, so required-mode launch failures leave y unchanged. Broader fused convolution APIs, alternate algorithms, FindEx timing/benchmark execution, 5D/Nd tensors, NHWC, non-FP32, non-contiguous/custom-stride, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, device pointer-mode scalars, CUDA streams, and real cuDNN async semantics are outside this bounded slice.",
    "native_simulated_cudnn_convolution_bias_activation_forward_op": "Native cuDNN-shaped cudnnConvolutionBiasActivationForward is available under explicit simulated-memory opt-in. This bounded legacy fused forward bridge supports contiguous 4D NCHW FP32 x/z/y tensor descriptors, contiguous FP32 NCHW/KCRS filters using the same cuDNN grouped semantics as convolution forward, strict contiguous FP32 bias descriptors shaped 1xKx1x1, zDesc value-identical to yDesc, CUDNN_ACTIVATION_RELU with CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM, and CUDNN_ACTIVATION_IDENTITY with CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM. It computes y = act(alpha1 * conv_or_correlation(x, w) + alpha2 * z + bias[channel]) over host-accessible CPU pointers, supports exact z == y by staging the original residual before output copy-back, rejects partial z/y overlap and broader x/w/z/bias/y byte-range overlap, requires CUDNN_NOT_PROPAGATE_NAN and ignores the activation coefficient, avoids reading z when alpha2 is zero, accepts but ignores workspace, and leaves y unchanged on validation failure or required-Metal backend failure. The default path is CPU reference execution. On Darwin, PSYCHE_CUDA_COMPAT_CUDNN_METAL=required verifies a real fused MSL shared-buffer route for the same subset and returns the Metal-derived failure without CPU fallback when the backend is unavailable; PSYCHE_CUDA_COMPAT_CUDNN_METAL=1 prefers Metal and falls back only for backend-availability failures. The Metal route stages x, w, z, and bias into separate shared buffers, binds a dummy z buffer when alpha2 is zero so caller z is not read, writes into a separate output buffer, suppresses fused ReLU NaNs under NOT_PROPAGATE_NAN, and copies y back only after command-buffer completion. With PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required, that Metal route can prefer a weak-linked MPSGraph convolution2D raw-convolution fast path for the same NCHW/OIHW grouped FP32 subset, then apply alpha1, optional alpha2/z, bias, and ReLU/identity through an MSL epilogue before CPU copy-back. Required-mode prepare, graph, or epilogue failures leave caller y unchanged, while preferred mode falls back to deterministic fused MSL. No fused MLX route is claimed yet. Non-ReLU/identity activations, ReLU with alternate algorithms, identity without PRECOMP_GEMM, loose bias broadcasting, 5D/Nd tensors, NHWC, non-FP32 tensors, custom strides, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, device pointer-mode scalars, CUDA streams, broader fused-ops APIs, and real cuDNN async semantics are not modeled.",
    "native_simulated_cudnn_convolution_backward_data_op": "Native cuDNN-shaped cudnnGetConvolutionBackwardDataAlgorithmMaxCount, cudnnGetConvolutionBackwardDataAlgorithm, cudnnGetConvolutionBackwardDataAlgorithm_v7, cudnnFindConvolutionBackwardDataAlgorithm, cudnnGetConvolutionBackwardDataWorkspaceSize, and cudnnConvolutionBackwardData are available under explicit simulated-memory opt-in. This bounded legacy operation bridge supports contiguous 4D NCHW FP32 dy/dx tensor descriptors, contiguous FP32 NCHW/KCRS filter descriptors using cuDNN grouped semantics (groupCount > 0, full dy/dx descriptors, filter C = input_C/groupCount, input_C % groupCount == 0, and K % groupCount == 0), CUDNN_CONVOLUTION_BWD_DATA_ALGO_1, CUDNN_CROSS_CORRELATION, and CUDNN_CONVOLUTION with spatial R/S filter flipping. Depthwise and depthwise-multiplier cases are covered when they satisfy the same grouped descriptor rules. Algorithm queries validate the same bounded descriptor configuration as backward-data execution, report exactly one deterministic zero-workspace ALGO_1 result, and do not claim ALGO_0, FFT, Winograd, or alternate algorithms for this FP32 NCHW path. The workspace query returns zero bytes only for that executable path. It computes dx = alpha * dconv_or_dcorrelation_data(w, dy) + beta * prior_dx over host-accessible CPU pointers by treating dxDesc as the corresponding forward input descriptor and requiring dyDesc to match the forward output shape computed from dx, w, and convDesc. It accepts but does not dereference workspace for the supported algorithm, rejects unsupported algorithms and invalid grouped descriptor shapes, rejects any w/dy/dx byte-range overlap including exact aliases while allowing adjacent non-overlap, avoids reading prior dx when beta is zero, and still evaluates the gradient sum so alpha-zero dy/filter NaNs propagate under the shim's literal formula convention. By default this path runs a CPU reference loop. On Darwin, PSYCHE_CUDA_COMPAT_CUDNN_METAL=required verifies real Metal shared-buffer dispatch for the same FP32 contiguous 4D NCHW/KCRS grouped subset and returns the Metal-derived failure without CPU fallback when the backend is unavailable; PSYCHE_CUDA_COMPAT_CUDNN_METAL=1 prefers Metal and falls back only for backend-availability failures. The Metal route stages w, dy, and old dx when beta is nonzero into separate shared buffers, computes one dx element per thread into a separate output buffer, and copies dx back only after command-buffer completion, so required-mode launch failures leave dx unchanged. Broader fused convolution APIs, alternate algorithms, FindEx timing/benchmark execution, 5D/Nd tensors, NHWC, non-FP32, non-contiguous/custom-stride, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, device pointer-mode scalars, CUDA streams, and real cuDNN async semantics are outside this bounded slice.",
    "native_simulated_cudnn_convolution_backward_filter_op": "Native cuDNN-shaped cudnnGetConvolutionBackwardFilterAlgorithmMaxCount, cudnnGetConvolutionBackwardFilterAlgorithm, cudnnGetConvolutionBackwardFilterAlgorithm_v7, cudnnFindConvolutionBackwardFilterAlgorithm, cudnnGetConvolutionBackwardFilterWorkspaceSize, and cudnnConvolutionBackwardFilter are available under explicit simulated-memory opt-in. This bounded legacy operation bridge supports contiguous 4D NCHW FP32 x/dy tensor descriptors, contiguous FP32 NCHW/KCRS dw filter descriptors using cuDNN grouped semantics (groupCount > 0, full x/dy descriptors, filter C = input_C/groupCount, input_C % groupCount == 0, and K % groupCount == 0), CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1, CUDNN_CROSS_CORRELATION, and CUDNN_CONVOLUTION. Depthwise and depthwise-multiplier cases are covered when they satisfy the same grouped descriptor rules. Algorithm queries validate the same bounded descriptor configuration as backward-filter execution, report exactly one deterministic zero-workspace ALGO_1 result, and do not claim ALGO_0, ALGO_3, FFT, Winograd, or alternate algorithms for this FP32 NCHW path. The workspace query returns zero bytes only for that executable path. It computes dw = alpha * dconv_or_dcorrelation_filter(x, dy) + beta * prior_dw over host-accessible CPU pointers by treating dwDesc as the corresponding forward filter descriptor and requiring dyDesc to match the forward output shape computed from x, dw, and convDesc. In true-convolution mode, the physical dw KCRS write slot remains unchanged while the input tap sampled for that physical slot is spatially flipped to match the forward and backward-data convention. It accepts but does not dereference workspace for the supported algorithm, rejects unsupported algorithms and invalid grouped descriptor shapes, rejects any x/dy/dw byte-range overlap including exact aliases while allowing adjacent non-overlap, avoids reading prior dw when beta is zero, and still evaluates the gradient sum so alpha-zero source/dy NaNs propagate under the shim's literal formula convention. By default this path runs a CPU reference loop. On Darwin, PSYCHE_CUDA_COMPAT_CUDNN_METAL=required verifies real Metal shared-buffer dispatch for the same FP32 contiguous 4D NCHW/KCRS grouped subset and returns the Metal-derived failure without CPU fallback when the backend is unavailable; PSYCHE_CUDA_COMPAT_CUDNN_METAL=1 prefers Metal and falls back only for backend-availability failures. The Metal route stages x, dy, and old dw when beta is nonzero into separate shared buffers and copies dw back with a CPU memcpy only after command-buffer completion, so required-mode launch failures leave dw unchanged. It keeps a deterministic one-thread-per-dw serial kernel as fallback/oracle, and for larger N*outH*outW spans uses a private-scratch two-pass tiled reduction with fixed-order threadgroup partial sums, fixed chunk-order final reduction, guarded beta-zero prior reads, and zero public caller workspace. With PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required, the same Metal route can prefer a weak-linked MPSGraph convolution2DWeightsGradient backend for NCHW/OIHW grouped FP32 backward-filter work. MPSGraph writes raw dW into an internal shared buffer, then an MSL epilogue writes alpha/beta and true-convolution R/S flip results into a second internal buffer before CPU copy-back; required-mode MPSGraph or epilogue failures leave caller dw unchanged, while preferred mode falls back to deterministic MSL. MPSGraph numerical checks are tolerance-based; MSL remains the bitwise deterministic oracle. Broader fused convolution APIs, alternate algorithms, FindEx timing/benchmark execution, 5D/Nd tensors, NHWC, non-FP32, non-contiguous/custom-stride, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, device pointer-mode scalars, CUDA streams, and real cuDNN async semantics are outside this bounded slice.",
    "native_simulated_cudnn_transform_op": "Native cuDNN-shaped cudnnTransformTensor is available under explicit simulated-memory opt-in. This bounded operation bridge supports the shim's current contiguous 4D NCHW FP32 tensor descriptors only when source and destination dimensions, data type, and descriptor values match, and computes y = alpha * x + beta * prior_y over host-accessible CPU pointers. It intentionally implements the scaled-copy subset rather than arbitrary-stride layout conversion; valid same-dimension transforms with different strides/layouts still return not-supported until the descriptor layer models those layouts. Any source/destination byte-range overlap is rejected, including exact x == y, because cuDNN documents no in-place transform. Beta-zero transform does not read prior y; alpha-zero still follows IEEE arithmetic, so alpha * NaN source remains NaN. By default this path runs a CPU reference loop. On Darwin, PSYCHE_CUDA_COMPAT_CUDNN_METAL=required verifies real Metal shared-buffer dispatch for the same subset and returns the Metal-derived failure without CPU fallback when the backend is unavailable; PSYCHE_CUDA_COMPAT_CUDNN_METAL=1 prefers Metal and falls back only for backend-availability failures. The Metal route stages x and old y into separate shared buffers, computes one output element per thread into a separate output buffer, and copies y back only after command-buffer completion, so required-mode launch failures leave y unchanged. NHWC, non-FP32, non-contiguous/custom-stride, arbitrary layout conversion, tensor transform descriptors, TransformTensorEx, device pointer-mode scalars, CUDA streams, and real cuDNN async semantics are not modeled.",
    "native_simulated_cudnn_pooling_op": "Native cuDNN-shaped pooling descriptor symbols, cudnnGetPooling2dForwardOutputDim, cudnnPoolingForward, and cudnnPoolingBackward are available under explicit simulated-memory opt-in. This bounded operation bridge supports contiguous 4D NCHW FP32 tensor descriptors, CUDNN_POOLING_MAX, CUDNN_POOLING_MAX_DETERMINISTIC, CUDNN_POOLING_AVERAGE_COUNT_INCLUDE_PADDING, and CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING 2D pooling descriptors, CUDNN_PROPAGATE_NAN and CUDNN_NOT_PROPAGATE_NAN behavior, cuDNN's floor output-dimension formula, host alpha/beta scalars, forward y = alpha * pool(x) + beta * y, and backward dx = alpha * grad + beta * prior_dx over host-accessible CPU pointers. Average include-padding uses a fixed window denominator and zero-valued padding; average exclude-padding uses the per-output in-bounds denominator; under NOT_PROPAGATE_NAN, forward NaN input elements contribute nothing to the sum while padding alone controls the denominator. Average backward is geometry-based, permits null x/y descriptors and data as cuDNN documents, and distributes dy over valid participating inputs using the same include/exclude padding denominator rather than reading input values. Max backward requires x/y descriptors and data, validates y/dy and x/dx descriptor compatibility, but recomputes deterministic max selection from x using the same row-major window scan and NaN policy as forward; both max modes route ties to the first selected element. Beta-zero backward does not read prior dx. By default this path runs a CPU reference loop. On Darwin, PSYCHE_CUDA_COMPAT_CUDNN_METAL=required verifies real Metal shared-buffer dispatch for the same subset and returns the Metal-derived failure without CPU fallback when the backend is unavailable; PSYCHE_CUDA_COMPAT_CUDNN_METAL=1 prefers Metal and falls back only for backend-availability failures. The Metal forward route stages x and old y into separate shared buffers, computes into a separate output buffer, explicitly branches for NaN propagation instead of relying on Metal max() NaN behavior, and copies y back only after command-buffer completion, so required-mode launch failures leave y unchanged. The Metal backward route computes one dx element per thread, scans contributing output windows, avoids FP32 atomics for deterministic Apple-family portability, and copies dx back only after command-buffer completion, so required-mode launch failures leave dx unchanged. Exact x == y in-place forward pooling is supported only when tensor descriptor values match, with CPU staging to preserve the original input/output values; exact dy == dx backward is supported only when descriptors match, with staging to preserve dy/prior dx. Partial x/y forward overlap, partial dy/dx backward overlap, and max-backward x/dx or y/dx overlap are rejected. Nd pooling descriptors, NHWC, non-FP32, non-contiguous/custom-stride, device pointer-mode scalars, CUDA streams, real cuDNN async semantics, and convolution/normalization/graph execution are not modeled.",
    "native_simulated_cudnn_softmax_op": "Native cuDNN-shaped cudnnSoftmaxForward and cudnnSoftmaxBackward are available under explicit simulated-memory opt-in. This bounded operation bridge supports contiguous 4D NCHW FP32 tensor descriptors, CUDNN_SOFTMAX_FAST, CUDNN_SOFTMAX_ACCURATE, and CUDNN_SOFTMAX_LOG algorithms, plus CUDNN_SOFTMAX_MODE_CHANNEL and CUDNN_SOFTMAX_MODE_INSTANCE. Forward FAST remains a straightforward overflow-prone softmax; ACCURATE and LOG use max-scaled softmax/log-softmax. Backward uses y * (dy - sum(y * dy)) for FAST/ACCURATE and dy - exp(y) * sum(dy) for LOG. The shim defines deterministic whole-vector NaN propagation and stable forward +Inf handling for ACCURATE/LOG, applies host alpha/beta scalars as dst = alpha * result + beta * prior, and avoids reading prior output storage when beta is zero. By default this path runs a CPU reference loop. On Darwin, PSYCHE_CUDA_COMPAT_CUDNN_METAL=required verifies a real Metal shared-buffer dispatch for the same subset using one cooperative 256-lane threadgroup per softmax vector and returns the Metal-derived failure without CPU fallback when the backend is unavailable; PSYCHE_CUDA_COMPAT_CUDNN_METAL=1 prefers Metal and falls back only for backend-availability failures. Both CPU and Metal paths stage exact x == y forward execution and exact dy == dx backward execution so original input and prior output values are preserved; partial x/y, dy/dx, and y/dx overlaps are rejected. 5D tensors, NHWC, non-FP32, non-contiguous/custom-stride, device pointer-mode scalars, CUDA streams, real cuDNN async semantics, and convolution/normalization/graph execution are not modeled.",
    "native_proc_address_stub": "cuGetProcAddress exposes only safe discovery symbols and withholds simulated or execution symbols with a null function pointer and not-found status.",
    "unsupported_requires_bridge": "Not implemented; requires a real Metal/MPS/MLX compiler, runtime, or library bridge.",
}

SUPPORT_LEVELS["native_simulated_cublas_op"] = (
    SUPPORT_LEVELS["native_simulated_cublas_op"]
    .replace("CUDA kernels, cuBLASLt, complex Level-2", "CUDA kernels, complex Level-2")
    .replace(
        "including simulated runtime pointers. Version/property helpers return zero.",
        "including simulated runtime pointers. On Apple Silicon, "
        "PSYCHE_CUDA_COMPAT_CUBLAS_METAL=required verifies real Metal "
        "shared-buffer dispatch for contiguous FP32 cublasSaxpy[_v2] and "
        "cublasSscal[_v2], plus contiguous FP32 cublasScopy[_v2] and "
        "cublasSdot[_v2], cublasSasum[_v2], cublasSnrm2[_v2], "
        "nonzero signed-stride FP32 cublasSgemv[_v2], and nonzero signed-stride FP32 "
        "cublasSger[_v2]; "
        "PSYCHE_CUDA_COMPAT_CUBLAS_METAL=1 prefers Metal and falls "
        "back to the CPU reference path for fallback-eligible backend errors; "
        "PSYCHE_CUDA_COMPAT_CUBLAS_METAL=required returns the Metal-derived status "
        "instead of falling back. The cuBLAS Metal routes copy host-accessible "
        "inputs and mutable spans into Metal shared buffers, then copy mutated outputs "
        "or scalar results back after command-buffer completion. Metal SGEMV stages A, compact x, and compact old y, computes into a separate compact output buffer, and copies or scatters y back only after completion; it stages signed-strided x/y through compact host buffers when needed, follows Netlib/cuBLAS negative-increment logical indexing for nonzero signed increments, and is a baseline one-thread-per-output kernel, not a tuned/tiled cuBLAS implementation. Metal SGER stages A, x, and y, initializes a separate A output buffer from A so padded leading-dimension rows are preserved, stages signed-strided x/y into compact host buffers when needed, follows Netlib/cuBLAS negative-increment logical indexing for nonzero signed increments, updates one logical matrix element per thread, and copies A back only after completion; it is a baseline rank-1 update kernel, not a tuned/tiled cuBLAS implementation, and is bounded to 32-bit logical update and staged-A element counts. Metal SDOT, SASUM, and SNRM2 use parallel reduction order, and Metal SNRM2 uses a stable scale/ssq pair reduction instead of naive sum-of-squares. Contiguous Metal SAXPY and SCOPY allow exact x == y aliasing but reject partial overlap in required mode. Strided FP32 AXPY, SSCAL, SCOPY, SDOT, SASUM, and SNRM2 remain "
        "CPU-backed unless required Metal mode is set, in which case they return "
        "not-supported instead of falling back. GEMV, GER, HEMV, HER, HER2, SYMV, SYR, and SYR2 reject zero increments, but nonzero negative increments use signed logical vector order on their CPU-backed paths. Version/property helpers return zero.",
    )
    .replace(
        "and real GPU execution are not modeled.",
        "and real GPU execution outside the opt-in contiguous FP32 cublasSaxpy, "
        "cublasSscal, cublasScopy, cublasSdot, cublasSasum, cublasSnrm2, signed-stride cublasSgemv, and signed-stride cublasSger Metal routes are not modeled.",
    )
)

SUPPORT_LEVELS["native_simulated_cudnn_convolution_forward_op"] = (
    SUPPORT_LEVELS["native_simulated_cudnn_convolution_forward_op"]
    .replace(
        "The Metal route stages x, w, and old y when beta is nonzero into separate shared buffers, computes one y element per thread into a separate output buffer, and copies y back only after command-buffer completion, so required-mode launch failures leave y unchanged. Broader fused convolution APIs, alternate algorithms, FindEx timing/benchmark execution, 5D/Nd tensors, NHWC, non-FP32, non-contiguous/custom-stride, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, device pointer-mode scalars, CUDA streams, and real cuDNN async semantics are outside this bounded slice.",
        "The Metal route stages x, w, and old y when beta is nonzero into separate shared buffers, computes one y element per thread into a separate output buffer, and copies y back only after command-buffer completion, so required-mode launch failures leave y unchanged. With PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required, the Metal route can prefer a weak-linked MPSGraph convolution2D backend for the same NCHW/OIHW grouped FP32 forward work. An MSL prepare kernel copies cross-correlation weights or flips only the logical R/S axes for true convolution while leaving dilation to the MPSGraph descriptor; MPSGraph writes raw y into an internal shared buffer, then an MSL epilogue applies alpha/beta and beta-zero no-read behavior before CPU copy-back. Required-mode prepare, MPSGraph, or epilogue failures leave caller y unchanged, while preferred mode falls back to deterministic MSL. MPSGraph numerical checks are tolerance-based; MSL remains the bitwise deterministic oracle. Broader fused convolution APIs, alternate algorithms, FindEx timing/benchmark execution, 5D/Nd tensors, NHWC, non-FP32, non-contiguous/custom-stride, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, device pointer-mode scalars, CUDA streams, and real cuDNN async semantics are outside this bounded slice.",
    )
)

SUPPORT_LEVELS["native_simulated_cudnn_convolution_backward_data_op"] = (
    SUPPORT_LEVELS["native_simulated_cudnn_convolution_backward_data_op"]
    .replace(
        "The Metal route stages w, dy, and old dx when beta is nonzero into separate shared buffers, computes one dx element per thread into a separate output buffer, and copies dx back only after command-buffer completion, so required-mode launch failures leave dx unchanged. Broader fused convolution APIs, alternate algorithms, FindEx timing/benchmark execution, 5D/Nd tensors, NHWC, non-FP32, non-contiguous/custom-stride, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, device pointer-mode scalars, CUDA streams, and real cuDNN async semantics are outside this bounded slice.",
        "The Metal route stages w, dy, and old dx when beta is nonzero into separate shared buffers, computes one dx element per thread into a separate output buffer, and copies dx back only after command-buffer completion, so required-mode launch failures leave dx unchanged. With PSYCHE_CUDA_COMPAT_CUDNN_MPSGRAPH=1|required, the Metal route can prefer a weak-linked MPSGraph convolution2DDataGradient backend for the same NCHW/OIHW grouped FP32 backward-data work. It reuses the MSL weight-prepare rule from forward, lets MPSGraph write raw dx into an internal shared buffer, and uses an MSL epilogue for alpha/beta and beta-zero no-read behavior before CPU copy-back. Required-mode prepare, MPSGraph, or epilogue failures leave caller dx unchanged, while preferred mode falls back to deterministic MSL. MPSGraph numerical checks are tolerance-based; MSL remains the bitwise deterministic oracle. Broader fused convolution APIs, alternate algorithms, FindEx timing/benchmark execution, 5D/Nd tensors, NHWC, non-FP32, non-contiguous/custom-stride, broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites, device pointer-mode scalars, CUDA streams, and real cuDNN async semantics are outside this bounded slice.",
    )
)

SUPPORT_LEVELS["native_simulated_cudnn_convolution_backward_filter_op"] = (
    SUPPORT_LEVELS["native_simulated_cudnn_convolution_backward_filter_op"]
    .replace(
        "broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites",
        "broader MPSGraph layouts/dtypes and descriptor semantics, MLX rewrites",
    )
)

PYTHON_SURFACES = [
    {
        "name": "resolve_device(0 / 'cuda' / 'cuda:0')",
        "level": "python_redirect_to_mps",
        "claim": "Redirects to MPS only when PSYCHE_CUDA_COMPAT=1, CUDA is absent, and MPS is available.",
        "verified_by": ["PSYCHE_CUDA_COMPAT=1 scripts/check-cuda-compat.py"],
    },
    {
        "name": "torch.cuda / Tensor.cuda / Module.cuda / tensor.is_cuda",
        "level": "unsupported_requires_bridge",
        "claim": "Deliberately unpatched so PyTorch CUDA identity remains honest.",
        "verified_by": ["PSYCHE_CUDA_COMPAT=1 scripts/check-cuda-compat.py"],
    },
    {
        "name": "HfAuto CUDA-shaped model request",
        "level": "python_redirect_to_mps",
        "claim": "Validated for fallback-disabled tiny causal-LM forward/backward on MPS; non-allowlisted architectures are rejected.",
        "verified_by": [
            "PYTORCH_ENABLE_MPS_FALLBACK=0 PSYCHE_CUDA_COMPAT=1 scripts/check-hfauto-mps-redirect.py"
        ],
    },
    {
        "name": "Python sidecar CUDA-shaped device argument",
        "level": "python_redirect_to_mps",
        "claim": "Single-rank MPS path with Gloo/CPU-staged collectives; NCCL and multi-rank MPS are rejected.",
        "verified_by": ["PSYCHE_CUDA_COMPAT=1 scripts/check-sidecar-mps-device.py"],
    },
    {
        "name": "Exact MPS ATen fallback fixes under CUDA compat",
        "level": "mps_exact_route",
        "claim": "Exact MPS routes install for MPS contexts when PSYCHE_CUDA_COMPAT=1; opt out with PSYCHE_CUDA_COMPAT_MPS_ROUTES=0.",
        "verified_by": ["PSYCHE_CUDA_COMPAT=1 scripts/check-cuda-compat.py"],
    },
]

DRIVER_SYMBOL_LEVELS = {
    "native_discovery_stub": [
        "cuInit",
        "cuDriverGetVersion",
        "cuDeviceGetCount",
        "cuDeviceGet",
        "cuDeviceGetName",
        "cuDeviceTotalMem",
        "cuDeviceGetAttribute",
        "cuCtxGetCurrent",
        "cuMemGetInfo",
        "cuGetErrorName",
        "cuGetErrorString",
        "psyche_cuda_compat_stub_is_stub",
        "psyche_cuda_compat_stub_version",
    ],
    "native_proc_address_stub": [
        "cuGetProcAddress",
    ],
    "native_hard_fail_stub": [
        "cuCtxGetStreamPriorityRange",
        "cuCtxSynchronize",
        "cuModuleLoad",
    ],
    "native_simulated_driver_kernel_op": [
        "cuModuleLoadData",
        "cuModuleGetFunction",
        "cuLaunchKernel",
    ],
    "native_simulated_driver_memory_op": [
        "cuMemAlloc",
        "cuMemAllocHost",
        "cuMemAllocManaged",
        "cuMemAllocPitch",
        "cuMemAllocPitch_v2",
        "cuMemAlloc_v2",
        "cuMemAdvise",
        "cuMemFree",
        "cuMemFreeHost",
        "cuMemFree_v2",
        "cuMemHostAlloc",
        "cuMemHostGetDevicePointer",
        "cuMemHostGetFlags",
        "cuMemHostRegister",
        "cuMemHostUnregister",
        "cuMemPrefetchAsync",
        "cuMemRangeGetAttribute",
        "cuMemRangeGetAttributes",
        "cuMemcpy",
        "cuMemcpy2D",
        "cuMemcpy2DAsync",
        "cuMemcpy2DAsync_v2",
        "cuMemcpy2DUnaligned",
        "cuMemcpy2DUnaligned_v2",
        "cuMemcpy2D_v2",
        "cuMemcpy3D",
        "cuMemcpy3DAsync",
        "cuMemcpy3DAsync_v2",
        "cuMemcpy3D_v2",
        "cuMemcpyAsync",
        "cuMemcpyAsync_v2",
        "cuMemcpyDtoD",
        "cuMemcpyDtoDAsync",
        "cuMemcpyDtoDAsync_v2",
        "cuMemcpyDtoD_v2",
        "cuMemcpyDtoH",
        "cuMemcpyDtoHAsync",
        "cuMemcpyDtoHAsync_v2",
        "cuMemcpyDtoH_v2",
        "cuMemcpyHtoD",
        "cuMemcpyHtoDAsync",
        "cuMemcpyHtoDAsync_v2",
        "cuMemcpyHtoD_v2",
        "cuMemcpy_v2",
        "cuPointerGetAttribute",
        "cuPointerGetAttributes",
        "cuPointerSetAttribute",
        "cuMemsetD16",
        "cuMemsetD16Async",
        "cuMemsetD2D16",
        "cuMemsetD2D16Async",
        "cuMemsetD2D32",
        "cuMemsetD2D32Async",
        "cuMemsetD2D8",
        "cuMemsetD2D8Async",
        "cuMemsetD32",
        "cuMemsetD32Async",
        "cuMemsetD8",
        "cuMemsetD8Async",
    ],
    "native_simulated_driver_mempool_op": [
        "cuDeviceGetDefaultMemPool",
        "cuDeviceGetMemPool",
        "cuDeviceSetMemPool",
        "cuMemAllocAsync",
        "cuMemAllocFromPoolAsync",
        "cuMemFreeAsync",
        "cuMemGetDefaultMemPool",
        "cuMemGetMemPool",
        "cuMemSetMemPool",
        "cuMemPoolCreate",
        "cuMemPoolDestroy",
        "cuMemPoolExportPointer",
        "cuMemPoolExportToShareableHandle",
        "cuMemPoolGetAccess",
        "cuMemPoolGetAttribute",
        "cuMemPoolImportFromShareableHandle",
        "cuMemPoolImportPointer",
        "cuMemPoolSetAccess",
        "cuMemPoolSetAttribute",
        "cuMemPoolTrimTo",
    ],
    "native_simulated_driver_sync_op": [
        "cuEventCreate",
        "cuEventDestroy",
        "cuEventDestroy_v2",
        "cuEventElapsedTime",
        "cuEventQuery",
        "cuEventRecord",
        "cuEventRecordWithFlags",
        "cuEventSynchronize",
        "cuStreamCreate",
        "cuStreamCreateWithPriority",
        "cuStreamDestroy",
        "cuStreamDestroy_v2",
        "cuStreamGetFlags",
        "cuStreamGetPriority",
        "cuStreamQuery",
        "cuStreamSynchronize",
    ],
}

RUNTIME_SYMBOL_LEVELS = {
    "native_discovery_stub": [
        "cudaGetDeviceCount",
        "cudaGetDevice",
        "cudaSetDevice",
        "cudaGetDeviceProperties",
        "cudaDeviceGetAttribute",
        "cudaMemGetInfo",
        "cudaDriverGetVersion",
        "cudaRuntimeGetVersion",
        "cudaDeviceReset",
        "cudaPeekAtLastError",
        "cudaGetLastError",
        "cudaGetErrorName",
        "cudaGetErrorString",
        "psyche_cuda_compat_stub_is_stub",
        "psyche_cuda_compat_stub_version",
    ],
    "native_hard_fail_stub": [
        "cudaDeviceGetStreamPriorityRange",
        "cudaDeviceSynchronize",
    ],
    "native_simulated_runtime_memory_op": [
        "cudaFree",
        "cudaFreeHost",
        "cudaHostAlloc",
        "cudaHostGetDevicePointer",
        "cudaHostGetFlags",
        "cudaHostRegister",
        "cudaHostUnregister",
        "cudaMalloc",
        "cudaMallocManaged",
        "cudaMallocPitch",
        "cudaMallocHost",
        "cudaMemAdvise",
        "cudaMemcpy",
        "cudaMemcpy2D",
        "cudaMemcpy2DAsync",
        "cudaMemcpy3D",
        "cudaMemcpy3DAsync",
        "cudaMemcpyAsync",
        "cudaMemPrefetchAsync",
        "cudaMemRangeGetAttribute",
        "cudaMemRangeGetAttributes",
        "cudaMemset",
        "cudaMemset2D",
        "cudaMemset2DAsync",
        "cudaMemsetAsync",
        "cudaPointerGetAttributes",
    ],
    "native_simulated_runtime_mempool_op": [
        "cudaDeviceGetDefaultMemPool",
        "cudaDeviceGetMemPool",
        "cudaDeviceSetMemPool",
        "cudaFreeAsync",
        "cudaMallocAsync",
        "cudaMallocFromPoolAsync",
        "cudaMemGetDefaultMemPool",
        "cudaMemGetMemPool",
        "cudaMemSetMemPool",
        "cudaMemPoolCreate",
        "cudaMemPoolDestroy",
        "cudaMemPoolExportPointer",
        "cudaMemPoolExportToShareableHandle",
        "cudaMemPoolGetAccess",
        "cudaMemPoolGetAttribute",
        "cudaMemPoolImportFromShareableHandle",
        "cudaMemPoolImportPointer",
        "cudaMemPoolSetAccess",
        "cudaMemPoolSetAttribute",
        "cudaMemPoolTrimTo",
    ],
    "native_simulated_runtime_sync_op": [
        "cudaEventCreate",
        "cudaEventCreateWithFlags",
        "cudaEventDestroy",
        "cudaEventElapsedTime",
        "cudaEventQuery",
        "cudaEventRecord",
        "cudaEventRecordWithFlags",
        "cudaEventSynchronize",
        "cudaStreamCreate",
        "cudaStreamCreateWithFlags",
        "cudaStreamCreateWithPriority",
        "cudaStreamDestroy",
        "cudaStreamGetFlags",
        "cudaStreamGetPriority",
        "cudaStreamQuery",
        "cudaStreamSynchronize",
    ],
    "native_simulated_runtime_kernel_op": [
        "cudaLaunchKernel",
        "psyche_cuda_runtime_kernel_axpby_f32",
        "psyche_cuda_runtime_kernel_saxpy_f32",
        "psyche_cuda_runtime_kernel_scale_f32",
        "psyche_cuda_runtime_kernel_vector_add_f32",
    ],
}

CUBLAS_SYMBOL_LEVELS = {
    "native_simulated_cublas_op": [
        "cublasCaxpy",
        "cublasCaxpy_v2",
        "cublasCcopy",
        "cublasCcopy_v2",
        "cublasCreate",
        "cublasCreate_v2",
        "cublasCdotc",
        "cublasCdotc_v2",
        "cublasCdotu",
        "cublasCdotu_v2",
        "cublasCgemm",
        "cublasCgemmBatched",
        "cublasCgemmStridedBatched",
        "cublasCgemm_v2",
        "cublasCgemv",
        "cublasCgemv_v2",
        "cublasCgerc",
        "cublasCgerc_v2",
        "cublasCgeru",
        "cublasCgeru_v2",
        "cublasChemv",
        "cublasChemv_v2",
        "cublasCher",
        "cublasCher_v2",
        "cublasCher2",
        "cublasCher2_v2",
        "cublasCscal",
        "cublasCscal_v2",
        "cublasCsscal",
        "cublasCsscal_v2",
        "cublasCswap",
        "cublasCswap_v2",
        "cublasCtrmm",
        "cublasCtrmm_v2",
        "cublasCtrmv",
        "cublasCtrmv_v2",
        "cublasCtrsm",
        "cublasCtrsm_v2",
        "cublasCtrsv",
        "cublasCtrsv_v2",
        "cublasDasum",
        "cublasDasum_v2",
        "cublasDaxpy",
        "cublasDaxpy_v2",
        "cublasDcopy",
        "cublasDcopy_v2",
        "cublasDestroy",
        "cublasDestroy_v2",
        "cublasDdot",
        "cublasDdot_v2",
        "cublasDger",
        "cublasDger_v2",
        "cublasDgemm",
        "cublasDgemmBatched",
        "cublasDgemmStridedBatched",
        "cublasDgemm_v2",
        "cublasDgemv",
        "cublasDgemv_v2",
        "cublasDnrm2",
        "cublasDnrm2_v2",
        "cublasDrot",
        "cublasDrotg",
        "cublasDrotg_v2",
        "cublasDrotm",
        "cublasDrotm_v2",
        "cublasDrotmg",
        "cublasDrotmg_v2",
        "cublasDrot_v2",
        "cublasDscal",
        "cublasDscal_v2",
        "cublasDswap",
        "cublasDswap_v2",
        "cublasDsymm",
        "cublasDsymm_v2",
        "cublasDsyr",
        "cublasDsyr2",
        "cublasDsyr2k",
        "cublasDsyr2k_v2",
        "cublasDsyr2_v2",
        "cublasDsyrk",
        "cublasDsyrk_v2",
        "cublasDsyr_v2",
        "cublasDsymv",
        "cublasDsymv_v2",
        "cublasDtrmm",
        "cublasDtrmm_v2",
        "cublasDtrmv",
        "cublasDtrmv_v2",
        "cublasDtrsm",
        "cublasDtrsm_v2",
        "cublasDtrsv",
        "cublasDtrsv_v2",
        "cublasCher2k",
        "cublasCher2k_v2",
        "cublasCherk",
        "cublasCherk_v2",
        "cublasGetAtomicsMode",
        "cublasGetMathMode",
        "cublasGetMatrix",
        "cublasGetMatrixAsync",
        "cublasGetPointerMode",
        "cublasGetPointerMode_v2",
        "cublasGetProperty",
        "cublasGetStatusName",
        "cublasGetStatusString",
        "cublasGetStream",
        "cublasGetStream_v2",
        "cublasGetVector",
        "cublasGetVectorAsync",
        "cublasGetVersion",
        "cublasGetVersion_v2",
        "cublasIdamax",
        "cublasIdamax_v2",
        "cublasIdamin",
        "cublasIdamin_v2",
        "cublasIsamax",
        "cublasIsamax_v2",
        "cublasIsamin",
        "cublasIsamin_v2",
        "cublasSasum",
        "cublasSasum_v2",
        "cublasSaxpy",
        "cublasSaxpy_v2",
        "cublasScopy",
        "cublasScopy_v2",
        "cublasSdot",
        "cublasSdot_v2",
        "cublasSger",
        "cublasSger_v2",
        "cublasSrot",
        "cublasSrotg",
        "cublasSrotg_v2",
        "cublasSrotm",
        "cublasSrotm_v2",
        "cublasSrotmg",
        "cublasSrotmg_v2",
        "cublasSrot_v2",
        "cublasSetAtomicsMode",
        "cublasSetMathMode",
        "cublasSetMatrix",
        "cublasSetMatrixAsync",
        "cublasSetPointerMode",
        "cublasSetPointerMode_v2",
        "cublasSetStream",
        "cublasSetStream_v2",
        "cublasSetVector",
        "cublasSetVectorAsync",
        "cublasSnrm2",
        "cublasSnrm2_v2",
        "cublasSscal",
        "cublasSscal_v2",
        "cublasSgemm",
        "cublasSgemmBatched",
        "cublasSgemmStridedBatched",
        "cublasSgemm_v2",
        "cublasSgemv",
        "cublasSgemv_v2",
        "cublasSswap",
        "cublasSswap_v2",
        "cublasSsymm",
        "cublasSsymm_v2",
        "cublasSsyr",
        "cublasSsyr2",
        "cublasSsyr2k",
        "cublasSsyr2k_v2",
        "cublasSsyr2_v2",
        "cublasSsyrk",
        "cublasSsyrk_v2",
        "cublasSsyr_v2",
        "cublasSsymv",
        "cublasSsymv_v2",
        "cublasStrmm",
        "cublasStrmm_v2",
        "cublasStrmv",
        "cublasStrmv_v2",
        "cublasStrsm",
        "cublasStrsm_v2",
        "cublasStrsv",
        "cublasStrsv_v2",
        "cublasZaxpy",
        "cublasZaxpy_v2",
        "cublasZcopy",
        "cublasZcopy_v2",
        "cublasZdotc",
        "cublasZdotc_v2",
        "cublasZdotu",
        "cublasZdotu_v2",
        "cublasZdscal",
        "cublasZdscal_v2",
        "cublasZgemm",
        "cublasZgemmBatched",
        "cublasZgemmStridedBatched",
        "cublasZgemm_v2",
        "cublasZgemv",
        "cublasZgemv_v2",
        "cublasZgerc",
        "cublasZgerc_v2",
        "cublasZgeru",
        "cublasZgeru_v2",
        "cublasZher2k",
        "cublasZher2k_v2",
        "cublasZherk",
        "cublasZherk_v2",
        "cublasZhemv",
        "cublasZhemv_v2",
        "cublasZher",
        "cublasZher_v2",
        "cublasZher2",
        "cublasZher2_v2",
        "cublasZscal",
        "cublasZscal_v2",
        "cublasZswap",
        "cublasZswap_v2",
        "cublasZtrmm",
        "cublasZtrmm_v2",
        "cublasZtrmv",
        "cublasZtrmv_v2",
        "cublasZtrsm",
        "cublasZtrsm_v2",
        "cublasZtrsv",
        "cublasZtrsv_v2",
    ],
}

CUBLASLT_SYMBOL_LEVELS = {
    "native_simulated_cublaslt_op": [
        "cublasLtCreate",
        "cublasLtDestroy",
        "cublasLtDisableCpuInstructionsSetMask",
        "cublasLtGetCudartVersion",
        "cublasLtGetProperty",
        "cublasLtGetStatusName",
        "cublasLtGetStatusString",
        "cublasLtGetVersion",
        "cublasLtHeuristicsCacheGetCapacity",
        "cublasLtHeuristicsCacheSetCapacity",
        "cublasLtMatmul",
        "cublasLtMatmulAlgoCheck",
        "cublasLtMatmulAlgoGetHeuristic",
        "cublasLtMatmulAlgoGetIds",
        "cublasLtMatmulAlgoInit",
        "cublasLtMatmulDescCreate",
        "cublasLtMatmulDescDestroy",
        "cublasLtMatmulDescGetAttribute",
        "cublasLtMatmulDescInit_internal",
        "cublasLtMatmulDescSetAttribute",
        "cublasLtMatmulPreferenceCreate",
        "cublasLtMatmulPreferenceDestroy",
        "cublasLtMatmulPreferenceGetAttribute",
        "cublasLtMatmulPreferenceInit_internal",
        "cublasLtMatmulPreferenceSetAttribute",
        "cublasLtMatrixLayoutCreate",
        "cublasLtMatrixLayoutDestroy",
        "cublasLtMatrixLayoutGetAttribute",
        "cublasLtMatrixLayoutInit_internal",
        "cublasLtMatrixLayoutSetAttribute",
        "cublasLtMatrixTransform",
        "cublasLtMatrixTransformDescCreate",
        "cublasLtMatrixTransformDescDestroy",
        "cublasLtMatrixTransformDescGetAttribute",
        "cublasLtMatrixTransformDescInit_internal",
        "cublasLtMatrixTransformDescSetAttribute",
    ],
}

CUSPARSE_SYMBOL_LEVELS = {
    "native_simulated_cusparse_spmv_op": [
        "cusparseCreate",
        "cusparseCreateCsr",
        "cusparseCreateDnVec",
        "cusparseDestroy",
        "cusparseDestroyDnVec",
        "cusparseDestroySpMat",
        "cusparseGetErrorName",
        "cusparseGetErrorString",
        "cusparseGetPointerMode",
        "cusparseGetProperty",
        "cusparseGetStream",
        "cusparseGetVersion",
        "cusparseSetPointerMode",
        "cusparseSetStream",
        "cusparseSpMV",
        "cusparseSpMV_bufferSize",
        "psyche_cuda_compat_stub_is_stub",
        "psyche_cuda_compat_stub_version",
    ],
    "native_simulated_cusparse_spmm_op": [
        "cusparseCreateConstDnMat",
        "cusparseCreateDnMat",
        "cusparseDestroyDnMat",
        "cusparseSpMM",
        "cusparseSpMM_bufferSize",
    ],
}

CUSOLVER_SYMBOL_LEVELS = {
    "native_simulated_cusolver_dense_lu_op": [
        "cusolverDnCreate",
        "cusolverDnDestroy",
        "cusolverDnDgetrf",
        "cusolverDnDgetrf_bufferSize",
        "cusolverDnDgetrs",
        "cusolverDnGetProperty",
        "cusolverDnGetStream",
        "cusolverDnGetVersion",
        "cusolverDnSetStream",
        "cusolverDnSgetrf",
        "cusolverDnSgetrf_bufferSize",
        "cusolverDnSgetrs",
        "cusolverGetErrorName",
        "cusolverGetErrorString",
        "psyche_cuda_compat_stub_is_stub",
        "psyche_cuda_compat_stub_version",
    ],
    "native_simulated_cusolver_dense_cholesky_op": [
        "cusolverDnDpotrf",
        "cusolverDnDpotrf_bufferSize",
        "cusolverDnDpotri",
        "cusolverDnDpotri_bufferSize",
        "cusolverDnDpotrs",
        "cusolverDnSpotrf",
        "cusolverDnSpotrf_bufferSize",
        "cusolverDnSpotri",
        "cusolverDnSpotri_bufferSize",
        "cusolverDnSpotrs",
    ],
}

NVML_SYMBOL_LEVELS = {
    "native_nvml_discovery_stub": [
        "nvmlDeviceGetCount",
        "nvmlDeviceGetCount_v2",
        "nvmlDeviceGetCudaComputeCapability",
        "nvmlDeviceGetHandleByIndex",
        "nvmlDeviceGetHandleByIndex_v2",
        "nvmlDeviceGetHandleByPciBusId",
        "nvmlDeviceGetHandleByPciBusId_v2",
        "nvmlDeviceGetHandleByUUID",
        "nvmlDeviceGetMemoryInfo",
        "nvmlDeviceGetName",
        "nvmlDeviceGetPowerUsage",
        "nvmlDeviceGetTemperature",
        "nvmlDeviceGetUUID",
        "nvmlDeviceGetUtilizationRates",
        "nvmlErrorString",
        "nvmlInit",
        "nvmlInitWithFlags",
        "nvmlInit_v2",
        "nvmlShutdown",
        "nvmlSystemGetCudaDriverVersion",
        "nvmlSystemGetCudaDriverVersion_v2",
        "nvmlSystemGetDriverVersion",
        "nvmlSystemGetNVMLVersion",
        "psyche_cuda_compat_stub_is_stub",
        "psyche_cuda_compat_stub_version",
    ],
}

CUDNN_SYMBOL_LEVELS = {
    "native_cudnn_discovery_stub": [
        "cudnnCreate",
        "cudnnDestroy",
        "cudnnGetCudartVersion",
        "cudnnGetErrorString",
        "cudnnGetLastErrorString",
        "cudnnGetMaxDeviceVersion",
        "cudnnGetProperty",
        "cudnnGetStream",
        "cudnnGetVersion",
        "cudnnGraphVersionCheck",
        "cudnnQueryRuntimeError",
        "cudnnSetStream",
        "psyche_cuda_compat_stub_is_stub",
        "psyche_cuda_compat_stub_version",
    ],
    "native_simulated_cudnn_activation_op": [
        "cudnnActivationBackward",
        "cudnnActivationForward",
        "cudnnCreateActivationDescriptor",
        "cudnnCreateTensorDescriptor",
        "cudnnDestroyActivationDescriptor",
        "cudnnDestroyTensorDescriptor",
        "cudnnSetActivationDescriptor",
        "cudnnSetTensor4dDescriptor",
    ],
    "native_simulated_cudnn_add_op": [
        "cudnnAddTensor",
    ],
    "native_simulated_cudnn_batchnorm_inference_op": [
        "cudnnBatchNormalizationForwardInference",
    ],
    "native_simulated_cudnn_convolution_forward_op": [
        "cudnnConvolutionForward",
        "cudnnCreateConvolutionDescriptor",
        "cudnnCreateFilterDescriptor",
        "cudnnDestroyConvolutionDescriptor",
        "cudnnDestroyFilterDescriptor",
        "cudnnFindConvolutionForwardAlgorithm",
        "cudnnGetConvolution2dDescriptor",
        "cudnnGetConvolution2dForwardOutputDim",
        "cudnnGetConvolutionForwardAlgorithm",
        "cudnnGetConvolutionForwardAlgorithmMaxCount",
        "cudnnGetConvolutionForwardAlgorithm_v7",
        "cudnnGetConvolutionForwardWorkspaceSize",
        "cudnnGetConvolutionGroupCount",
        "cudnnGetFilter4dDescriptor",
        "cudnnSetConvolution2dDescriptor",
        "cudnnSetConvolutionGroupCount",
        "cudnnSetFilter4dDescriptor",
    ],
    "native_simulated_cudnn_convolution_bias_activation_forward_op": [
        "cudnnConvolutionBiasActivationForward",
    ],
    "native_simulated_cudnn_convolution_backward_data_op": [
        "cudnnConvolutionBackwardData",
        "cudnnCreateConvolutionDescriptor",
        "cudnnCreateFilterDescriptor",
        "cudnnDestroyConvolutionDescriptor",
        "cudnnDestroyFilterDescriptor",
        "cudnnFindConvolutionBackwardDataAlgorithm",
        "cudnnGetConvolution2dDescriptor",
        "cudnnGetConvolutionBackwardDataAlgorithm",
        "cudnnGetConvolutionBackwardDataAlgorithmMaxCount",
        "cudnnGetConvolutionBackwardDataAlgorithm_v7",
        "cudnnGetConvolutionBackwardDataWorkspaceSize",
        "cudnnGetConvolutionGroupCount",
        "cudnnGetFilter4dDescriptor",
        "cudnnSetConvolution2dDescriptor",
        "cudnnSetConvolutionGroupCount",
        "cudnnSetFilter4dDescriptor",
    ],
    "native_simulated_cudnn_convolution_backward_filter_op": [
        "cudnnConvolutionBackwardFilter",
        "cudnnCreateConvolutionDescriptor",
        "cudnnCreateFilterDescriptor",
        "cudnnDestroyConvolutionDescriptor",
        "cudnnDestroyFilterDescriptor",
        "cudnnFindConvolutionBackwardFilterAlgorithm",
        "cudnnGetConvolution2dDescriptor",
        "cudnnGetConvolutionBackwardFilterAlgorithm",
        "cudnnGetConvolutionBackwardFilterAlgorithmMaxCount",
        "cudnnGetConvolutionBackwardFilterAlgorithm_v7",
        "cudnnGetConvolutionBackwardFilterWorkspaceSize",
        "cudnnGetConvolutionGroupCount",
        "cudnnGetFilter4dDescriptor",
        "cudnnSetConvolution2dDescriptor",
        "cudnnSetConvolutionGroupCount",
        "cudnnSetFilter4dDescriptor",
    ],
    "native_simulated_cudnn_pooling_op": [
        "cudnnCreatePoolingDescriptor",
        "cudnnDestroyPoolingDescriptor",
        "cudnnGetPooling2dDescriptor",
        "cudnnGetPooling2dForwardOutputDim",
        "cudnnPoolingBackward",
        "cudnnPoolingForward",
        "cudnnSetPooling2dDescriptor",
    ],
    "native_simulated_cudnn_softmax_op": [
        "cudnnSoftmaxBackward",
        "cudnnSoftmaxForward",
    ],
    "native_simulated_cudnn_transform_op": [
        "cudnnTransformTensor",
    ],
}

UNSUPPORTED_SURFACES = [
    ("PTX/CUBIN execution", "Needs PTX/cubin ingestion, compilation or translation, arbitrary module loading, CUDA runtime function registration, and Metal-backed kernel launch beyond the current registered Psyche-native driver/module and runtime-token vector_add_f32/saxpy_f32/scale_f32/axpby_f32 paths."),
    ("NVRTC / ptxas / Triton CUDA compilation", "Needs a compiler path from CUDA/Triton IR to Metal Shading Language, MPSGraph, MLX, or precompiled kernels."),
    ("cuBLASLt advanced layouts, epilogues, tensor modes, and low precision", "Needs MLX/MPSGraph/Metal library bridges for tiled layouts, AUX scale/amax outputs, grouped batches, pointer-array non-DEFAULT epilogues, half/BF16/FP8/complex/int data types, TF32/tensor modes, and NVIDIA-like algorithm tuning beyond the current descriptor-backed FP32/FP64 real ORDER_COL/ORDER_ROW DEFAULT/RELU/RELU_AUX/BIAS/RELU_BIAS/RELU_AUX_BIAS/DRELU/DRELU_BGRAD/GELU/GELU_BIAS/GELU_AUX/GELU_AUX_BIAS/DGELU/DGELU_BGRAD/BGRADA/BGRADB GEMM bridge, DEFAULT-only pointer-array batch bridge, and bounded FP32/FP64 cublasLtMatrixTransform bridge."),
    ("Broader cuDNN operation bridge", "ActivationForward/ActivationBackward, AddTensor broadcast/bias-add, BatchNormalizationForwardInference, ConvolutionForward including grouped/depthwise FP32 NCHW/KCRS forward, ConvolutionBiasActivationForward for the bounded FP32 NCHW/KCRS ReLU/identity fused forward subset, ConvolutionBackwardData including grouped/depthwise FP32 NCHW/KCRS ALGO_1 data gradients, ConvolutionBackwardFilter including grouped/depthwise FP32 NCHW/KCRS ALGO_1 filter gradients, TransformTensor scaled-copy, SoftmaxForward/SoftmaxBackward, and max/average PoolingForward/PoolingBackward now have bounded simulated-memory bridges for contiguous 4D NCHW FP32 tensors, with opt-in Metal bridges for the tensor/convolution pieces. Forward, backward-data, backward-filter, and bounded fused convolution forward additionally have opt-in MPSGraph fast paths with deterministic MSL fallback/oracles for the same bounded grouped FP32 NCHW/OIHW subset. Alternate convolution algorithms, batch-normalization training/backward, arbitrary-stride layout transform, graph, broader fused ops, non-FP32, custom-stride, NHWC, 5D tensors, Nd pooling descriptors, broader descriptor semantics, and MLX rewrites still need MPSGraph, MLX, or custom Metal bridges before being claimed compatible."),
    ("Broader cuSPARSE / cuSOLVER", "cuSPARSE now has bounded FP32 CSR SpMV and CSR SpMM bridges with matching 32-bit/64-bit CPU index support, optional Metal execution for 32-bit SpMV and SpMM indices, and honest required-Metal rejection for 64-bit sparse indices until the local Metal toolchain can prove those kernels. cuSOLVER now has explicit simulated-memory dense FP32/FP64 getrf/getrs and potrf/potri/potrs bridges: Darwin LU/Cholesky/inverse paths route through Accelerate/LAPACK, non-Darwin builds keep deterministic CPU reference LU/Cholesky/inverse/solve routes, and null-pivot LU remains supported. Remaining sparse formats, Metal-backed 64-bit sparse indices, broader/tiled/MLX SpMM beyond the bounded 32-bit CSR route, SpSV/SpSM, sparse/dense conversions, preprocessing/update APIs, low-precision/complex datatypes, batched sparse APIs, external workspace semantics, CUDA graph/async behavior, cuSOLVER sparse APIs, batched LU/Cholesky/inverse, QR, eigen, SVD, IRS/RF/Mg APIs, device-resident pointers, and real CUDA async semantics still need Accelerate, MLX, MPSGraph, or custom Metal bridges before being claimed compatible."),
    ("NCCL / GPUDirect / peer access / IPC", "Needs distributed and interprocess communication semantics that do not exist in the current MPS path."),
    ("CUDA Graphs and stream capture", "Needs command-buffer graph capture/replay semantics and stream compatibility."),
    ("Textures, surfaces, arrays, and samplers", "Needs CUDA memory object semantics mapped to Metal resources."),
    ("GPU-resident memory pools and real managed-memory migration", "Needs CUDA pool residency, graph allocator ownership, migration, and Apple unified-memory/Metal-buffer semantics beyond the current host-side pool metadata shim."),
    ("Real NVML per-device telemetry and Apple hardware mapping", "Needs a truthful Apple telemetry bridge for utilization, memory pressure, power, clocks, thermals, process accounting, and possibly a separate Apple-native API surface; the discovery shim must not report Apple GPU identity as NVIDIA hardware."),
]


def uncommented_c_source(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    return COMMENT_RE.sub("", text)


def exported_symbols_from_source(path: Path) -> tuple[set[str], dict[str, Any] | None]:
    text = uncommented_c_source(path)
    macro_count = len(EXPORT_MACRO_RE.findall(text))
    matches = EXPORT_RE.findall(text)
    parse_error = None
    if len(matches) != macro_count:
        parse_error = {
            "file": str(path),
            "macro_count": macro_count,
            "parsed_export_declarations": len(matches),
        }
    return set(matches), parse_error


def driver_proc_address_symbols_from_source(path: Path) -> tuple[set[str], dict[str, Any] | None]:
    text = uncommented_c_source(path)
    matches = PROC_ADDRESS_TABLE_RE.findall(text)
    if len(matches) != 1:
        return set(), {
            "file": str(path),
            "proc_address_tables_found": len(matches),
        }
    symbols = set(PROC_ADDRESS_SYMBOL_RE.findall(matches[0]))
    if not symbols:
        return symbols, {
            "file": str(path),
            "proc_address_symbols_found": 0,
        }
    return symbols, None


def load_mps_compat_module(repo_root: Path) -> Any:
    module_path = repo_root / "python/python/psyche/mps_compat.py"
    spec = importlib.util.spec_from_file_location("psyche_cuda_compat_ledger_mps", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"unable to import {module_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def active_mps_routes(install_result: Any) -> set[str]:
    return (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )


def load_mps_route_manifest(repo_root: Path) -> tuple[dict[str, list[str]], dict[str, Any]]:
    previous_env = {
        "PSYCHE_MPS_COMPAT_ADDMM_ACTIVATION_GRAD": os.environ.get(
            "PSYCHE_MPS_COMPAT_ADDMM_ACTIVATION_GRAD"
        ),
        "PSYCHE_MPS_COMPAT_MATRIX_EXP": os.environ.get("PSYCHE_MPS_COMPAT_MATRIX_EXP"),
        "PSYCHE_MPS_COMPAT_QR": os.environ.get("PSYCHE_MPS_COMPAT_QR"),
    }
    try:
        for name in previous_env:
            os.environ.pop(name, None)
        module = load_mps_compat_module(repo_root)
        manifest = {
            key: list(value)
            for key, value in module.mps_compat_route_manifest().items()
        }
        default_expected = set(manifest["default_exact"])
        experimental_expected = set(manifest["experimental_exact"])

        default_result = module.install_mps_compat_kernels()
        default_active = active_mps_routes(default_result)

        os.environ["PSYCHE_MPS_COMPAT_MATRIX_EXP"] = "1"
        os.environ["PSYCHE_MPS_COMPAT_QR"] = "1"
        experimental_result = module.install_mps_compat_kernels()
        experimental_active = active_mps_routes(experimental_result)

        validation_errors = []
        if default_active != default_expected:
            validation_errors.append(
                {
                    "kind": "default_mps_route_manifest_mismatch",
                    "missing_from_installer": sorted(default_expected - default_active),
                    "missing_from_manifest": sorted(default_active - default_expected),
                }
            )
        expected_experimental_active = default_expected | experimental_expected
        if not expected_experimental_active <= experimental_active:
            validation_errors.append(
                {
                    "kind": "experimental_mps_route_manifest_mismatch",
                    "missing_from_installer": sorted(
                        expected_experimental_active - experimental_active
                    ),
                    "unexpected_active_routes": sorted(
                        experimental_active - expected_experimental_active
                    ),
                }
            )

        validation = {
            "mps_route_manifest_errors": validation_errors,
            "default_active_routes": sorted(default_active),
            "experimental_active_routes": sorted(experimental_active),
            "default_install_result": {
                "installed": list(default_result.installed),
                "already_registered": list(default_result.already_registered),
                "skipped_existing_mps": list(default_result.skipped_existing_mps),
                "disabled_by_env": list(default_result.disabled_by_env),
            },
            "experimental_install_result": {
                "installed": list(experimental_result.installed),
                "already_registered": list(experimental_result.already_registered),
                "skipped_existing_mps": list(experimental_result.skipped_existing_mps),
                "disabled_by_env": list(experimental_result.disabled_by_env),
            },
        }
        return manifest, validation
    finally:
        for name, value in previous_env.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


def symbol_entries(library: str, grouped_symbols: dict[str, list[str]]) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for level, symbols in grouped_symbols.items():
        for symbol in symbols:
            entries.append(
                {
                    "surface": "native-symbol",
                    "library": library,
                    "name": symbol,
                    "level": level,
                    "claim": SUPPORT_LEVELS[level],
                }
            )
    return sorted(entries, key=lambda item: item["name"])


def build_report(repo_root: Path) -> dict[str, Any]:
    driver_source = repo_root / "tools/cuda-compat-shim/libcuda_stub.c"
    runtime_source = repo_root / "tools/cuda-compat-shim/libcudart_stub.c"
    cublas_source = repo_root / "tools/cuda-compat-shim/libcublas_stub.c"
    cublaslt_source = repo_root / "tools/cuda-compat-shim/libcublasLt_stub.c"
    cusparse_source = repo_root / "tools/cuda-compat-shim/libcusparse_stub.c"
    cusolver_source = repo_root / "tools/cuda-compat-shim/libcusolver_stub.c"
    nvml_source = repo_root / "tools/cuda-compat-shim/libnvidia_ml_stub.c"
    cudnn_source = repo_root / "tools/cuda-compat-shim/libcudnn_stub.c"
    mps_route_manifest, mps_route_validation = load_mps_route_manifest(repo_root)
    driver_exports, driver_parse_error = exported_symbols_from_source(driver_source)
    runtime_exports, runtime_parse_error = exported_symbols_from_source(runtime_source)
    cublas_exports, cublas_parse_error = exported_symbols_from_source(cublas_source)
    cublaslt_exports, cublaslt_parse_error = exported_symbols_from_source(cublaslt_source)
    cusparse_exports, cusparse_parse_error = exported_symbols_from_source(cusparse_source)
    cusolver_exports, cusolver_parse_error = exported_symbols_from_source(cusolver_source)
    nvml_exports, nvml_parse_error = exported_symbols_from_source(nvml_source)
    cudnn_exports, cudnn_parse_error = exported_symbols_from_source(cudnn_source)
    driver_proc_symbols, driver_proc_parse_error = driver_proc_address_symbols_from_source(driver_source)

    driver_manifest = {
        symbol
        for symbols in DRIVER_SYMBOL_LEVELS.values()
        for symbol in symbols
    }
    runtime_manifest = {
        symbol
        for symbols in RUNTIME_SYMBOL_LEVELS.values()
        for symbol in symbols
    }
    cublas_manifest = {
        symbol
        for symbols in CUBLAS_SYMBOL_LEVELS.values()
        for symbol in symbols
    }
    cublaslt_manifest = {
        symbol
        for symbols in CUBLASLT_SYMBOL_LEVELS.values()
        for symbol in symbols
    }
    cusparse_manifest = {
        symbol
        for symbols in CUSPARSE_SYMBOL_LEVELS.values()
        for symbol in symbols
    }
    cusolver_manifest = {
        symbol
        for symbols in CUSOLVER_SYMBOL_LEVELS.values()
        for symbol in symbols
    }
    nvml_manifest = {
        symbol
        for symbols in NVML_SYMBOL_LEVELS.values()
        for symbol in symbols
    }
    cudnn_manifest = {
        symbol
        for symbols in CUDNN_SYMBOL_LEVELS.values()
        for symbol in symbols
    }

    export_parse_errors = [
        error
        for error in (
            driver_parse_error,
            runtime_parse_error,
            cublas_parse_error,
            cublaslt_parse_error,
            cusparse_parse_error,
            cusolver_parse_error,
            nvml_parse_error,
            cudnn_parse_error,
        )
        if error is not None
    ]
    errors = []
    if driver_exports != driver_manifest:
        errors.append(
            {
                "library": "libcuda",
                "missing_from_manifest": sorted(driver_exports - driver_manifest),
                "missing_from_source": sorted(driver_manifest - driver_exports),
            }
        )
    if runtime_exports != runtime_manifest:
        errors.append(
            {
                "library": "libcudart",
                "missing_from_manifest": sorted(runtime_exports - runtime_manifest),
                "missing_from_source": sorted(runtime_manifest - runtime_exports),
            }
        )
    if cublas_exports != cublas_manifest:
        errors.append(
            {
                "library": "libcublas",
                "missing_from_manifest": sorted(cublas_exports - cublas_manifest),
                "missing_from_source": sorted(cublas_manifest - cublas_exports),
            }
        )
    if cublaslt_exports != cublaslt_manifest:
        errors.append(
            {
                "library": "libcublasLt",
                "missing_from_manifest": sorted(cublaslt_exports - cublaslt_manifest),
                "missing_from_source": sorted(cublaslt_manifest - cublaslt_exports),
            }
        )
    if cusparse_exports != cusparse_manifest:
        errors.append(
            {
                "library": "libcusparse",
                "missing_from_manifest": sorted(cusparse_exports - cusparse_manifest),
                "missing_from_source": sorted(cusparse_manifest - cusparse_exports),
            }
        )
    if cusolver_exports != cusolver_manifest:
        errors.append(
            {
                "library": "libcusolver",
                "missing_from_manifest": sorted(cusolver_exports - cusolver_manifest),
                "missing_from_source": sorted(cusolver_manifest - cusolver_exports),
            }
        )
    if nvml_exports != nvml_manifest:
        errors.append(
            {
                "library": "libnvidia-ml",
                "missing_from_manifest": sorted(nvml_exports - nvml_manifest),
                "missing_from_source": sorted(nvml_manifest - nvml_exports),
            }
        )
    if cudnn_exports != cudnn_manifest:
        errors.append(
            {
                "library": "libcudnn",
                "missing_from_manifest": sorted(cudnn_exports - cudnn_manifest),
                "missing_from_source": sorted(cudnn_manifest - cudnn_exports),
            }
        )

    driver_proc_allowed = (
        set(DRIVER_SYMBOL_LEVELS["native_discovery_stub"])
        | set(DRIVER_SYMBOL_LEVELS["native_proc_address_stub"])
    )
    driver_proc_rejected = (
        set(DRIVER_SYMBOL_LEVELS["native_hard_fail_stub"])
        | set(DRIVER_SYMBOL_LEVELS["native_simulated_driver_memory_op"])
        | set(DRIVER_SYMBOL_LEVELS["native_simulated_driver_mempool_op"])
        | set(DRIVER_SYMBOL_LEVELS["native_simulated_driver_sync_op"])
        | set(DRIVER_SYMBOL_LEVELS["native_simulated_driver_kernel_op"])
    )
    driver_proc_policy_errors = [
        error for error in (driver_proc_parse_error,) if error is not None
    ]
    if driver_proc_symbols != driver_proc_allowed:
        driver_proc_policy_errors.append(
            {
                "library": "libcuda",
                "missing_allowed_symbols": sorted(driver_proc_allowed - driver_proc_symbols),
                "unexpected_proc_address_symbols": sorted(driver_proc_symbols - driver_proc_allowed),
                "hard_fail_symbols_exposed": sorted(driver_proc_symbols & driver_proc_rejected),
            }
        )

    python_entries = [
        {
            "surface": "python-boundary",
            **entry,
        }
        for entry in PYTHON_SURFACES
    ]
    mps_entries = [
        {
            "surface": "aten-mps-route",
            "name": name,
            "level": "mps_exact_route",
            "claim": "Exact MPS route installed by default under PSYCHE_MPS_COMPAT=1 or redirected PSYCHE_CUDA_COMPAT=1 MPS contexts.",
            "verified_by": ["PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/check-mps-compat.py"],
        }
        for name in mps_route_manifest["default_exact"]
    ]
    mps_entries.extend(
        {
            "surface": "aten-mps-route",
            "name": name,
            "level": "mps_exact_route",
            "claim": "Gated exact-intent numerical route; enabled only by its PSYCHE_MPS_COMPAT_* experimental flag.",
            "verified_by": [
                "PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/check-mps-compat.py --matrix-exp --qr"
            ],
        }
        for name in mps_route_manifest["experimental_exact"]
    )
    unsupported_entries = [
        {
            "surface": "future-bridge",
            "name": name,
            "level": "unsupported_requires_bridge",
            "claim": claim,
        }
        for name, claim in UNSUPPORTED_SURFACES
    ]

    entries = [
        *python_entries,
        *mps_entries,
        *symbol_entries("libcuda", DRIVER_SYMBOL_LEVELS),
        *symbol_entries("libcudart", RUNTIME_SYMBOL_LEVELS),
        *symbol_entries("libcublas", CUBLAS_SYMBOL_LEVELS),
        *symbol_entries("libcublasLt", CUBLASLT_SYMBOL_LEVELS),
        *symbol_entries("libcusparse", CUSPARSE_SYMBOL_LEVELS),
        *symbol_entries("libcusolver", CUSOLVER_SYMBOL_LEVELS),
        *symbol_entries("libnvidia-ml", NVML_SYMBOL_LEVELS),
        *symbol_entries("libcudnn", CUDNN_SYMBOL_LEVELS),
        *unsupported_entries,
    ]
    counts = Counter(entry["level"] for entry in entries)
    counts_by_surface = defaultdict(Counter)
    for entry in entries:
        counts_by_surface[entry["surface"]][entry["level"]] += 1

    return {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "support_levels": SUPPORT_LEVELS,
        "source_references": SOURCE_REFERENCES,
        "mps_route_manifest": mps_route_manifest,
        "validation": {
            "source_export_manifest_errors": errors,
            "source_export_parse_errors": export_parse_errors,
            **mps_route_validation,
            "driver_source": str(driver_source.relative_to(repo_root)),
            "runtime_source": str(runtime_source.relative_to(repo_root)),
            "cublas_source": str(cublas_source.relative_to(repo_root)),
            "cublaslt_source": str(cublaslt_source.relative_to(repo_root)),
            "cusparse_source": str(cusparse_source.relative_to(repo_root)),
            "cusolver_source": str(cusolver_source.relative_to(repo_root)),
            "nvml_source": str(nvml_source.relative_to(repo_root)),
            "cudnn_source": str(cudnn_source.relative_to(repo_root)),
            "driver_exported_symbols": sorted(driver_exports),
            "runtime_exported_symbols": sorted(runtime_exports),
            "cublas_exported_symbols": sorted(cublas_exports),
            "cublaslt_exported_symbols": sorted(cublaslt_exports),
            "cusparse_exported_symbols": sorted(cusparse_exports),
            "cusolver_exported_symbols": sorted(cusolver_exports),
            "nvml_exported_symbols": sorted(nvml_exports),
            "cudnn_exported_symbols": sorted(cudnn_exports),
            "driver_proc_address_symbols": sorted(driver_proc_symbols),
            "driver_proc_address_allowed_symbols": sorted(driver_proc_allowed),
            "driver_proc_address_rejected_symbols": sorted(driver_proc_rejected),
            "driver_proc_address_policy_errors": driver_proc_policy_errors,
        },
        "counts_by_level": dict(sorted(counts.items())),
        "counts_by_surface": {
            surface: dict(sorted(counter.items()))
            for surface, counter in sorted(counts_by_surface.items())
        },
        "entries": entries,
    }


def write_markdown(report: dict[str, Any], path: Path) -> None:
    lines = [
        "# Psyche CUDA Compatibility Truth Ledger",
        "",
        f"- Generated: `{report['generated_at']}`",
        "- This is a truth ledger, not a support claim. CUDA identity remains unpatched unless an entry explicitly says otherwise.",
        "",
        "## Support Levels",
        "",
        "| Level | Meaning | Count |",
        "| --- | --- | ---: |",
    ]
    counts = report["counts_by_level"]
    for level, meaning in report["support_levels"].items():
        lines.append(f"| `{level}` | {meaning} | {counts.get(level, 0)} |")

    lines.extend(["", "## Source References", "", "| Source | Use |", "| --- | --- |"])
    for ref in report["source_references"]:
        lines.append(f"| [{ref['name']}]({ref['url']}) | {ref['why']} |")

    lines.extend(["", "## Coverage By Surface", "", "| Surface | Counts |", "| --- | --- |"])
    for surface, counter in report["counts_by_surface"].items():
        rendered = ", ".join(f"`{level}`: {count}" for level, count in counter.items())
        lines.append(f"| `{surface}` | {rendered} |")

    validation = report["validation"]
    validation_errors = [
        *validation["source_export_parse_errors"],
        *validation["source_export_manifest_errors"],
        *validation["driver_proc_address_policy_errors"],
        *validation["mps_route_manifest_errors"],
    ]
    lines.extend(["", "## Source Export Validation", ""])
    if validation_errors:
        lines.append("Validation mismatch detected:")
        for error in validation_errors:
            library = error.get("library", "source")
            lines.append(f"- `{library}`: {error}")
    else:
        lines.append("- MPS route manifest matches the actual compatibility installer.")
        lines.append("- Source exports match the manifest for `libcuda`, `libcudart`, `libcublas`, `libcublasLt`, `libcusparse`, `libcusolver`, `libnvidia-ml`, and `libcudnn`.")
        lines.append("- `cuGetProcAddress` exposes only the manifest's discovery/proc-policy symbols.")

    lines.extend(
        [
            "",
            "## Entries",
            "",
            "| Surface | Name | Level | Claim |",
            "| --- | --- | --- | --- |",
        ]
    )
    for entry in report["entries"]:
        claim = entry["claim"].replace("|", "\\|")
        lines.append(
            f"| `{entry['surface']}` | `{entry['name']}` | `{entry['level']}` | {claim} |"
        )

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json-out", type=Path, help="Write JSON report.")
    parser.add_argument("--markdown-out", type=Path, help="Write Markdown report.")
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if the native source exports do not match the manifest.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]
    report = build_report(repo_root)

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.markdown_out:
        args.markdown_out.parent.mkdir(parents=True, exist_ok=True)
        write_markdown(report, args.markdown_out)

    print(json.dumps(report, indent=2, sort_keys=True))
    validation = report["validation"]
    validation_errors = (
        validation["source_export_parse_errors"]
        or validation["source_export_manifest_errors"]
        or validation["driver_proc_address_policy_errors"]
        or validation["mps_route_manifest_errors"]
    )
    if args.check and validation_errors:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
