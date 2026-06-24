# Apple Silicon MPS Compatibility Layer

This page tracks the native Apple Silicon effort to turn PyTorch MPS CPU fallbacks into MPS-resident routes.

The goal is not to hide MPS gaps with `PYTORCH_ENABLE_MPS_FALLBACK=1`. The goal is to find unsupported `aten` operators, prove which ones actually leave the GPU, and add exact decompositions or Apple Silicon-specific kernels so Psyche can keep selected work on MPS.

## Source of Truth

PyTorch's MPS environment documentation says `PYTORCH_ENABLE_MPS_FALLBACK=1` runs unsupported MPS operators on CPU. PyTorch's MPS backend notes describe MPS as a backend that maps PyTorch graphs and primitives to MPSGraph and tuned Metal Performance Shaders kernels. The PyTorch MPS backend wiki gives the implementation ladder we follow here:

1. Bridge the PyTorch op to an existing MPS/MPSGraph primitive.
2. Write a custom Metal kernel for high-value ops.
3. Use CPU fallback only as a last resort.

Relevant upstream references:

- <https://docs.pytorch.org/docs/stable/mps_environment_variables.html>
- <https://docs.pytorch.org/docs/stable/notes/mps.html>
- <https://docs.pytorch.org/docs/stable/generated/torch.heaviside.html>
- <https://docs.pytorch.org/docs/stable/generated/torch.gcd.html>
- <https://docs.pytorch.org/docs/stable/generated/torch.lcm.html>
- <https://docs.pytorch.org/docs/stable/generated/torch.frexp.html>
- <https://github.com/pytorch/pytorch/wiki/MPS-Backend>

## Probe the Current Torch Build

Run the dispatcher and runtime probe from the repository root:

```bash
scripts/probe-mps-unsupported-ops.py \
  --runtime-probes \
  --json-out target/mps-unsupported-ops/local.json \
  --markdown-out target/mps-unsupported-ops/local.md
```

Then run the same probe with CPU fallback disabled to find hard failures that do not reliably emit fallback warnings:

```bash
PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/probe-mps-unsupported-ops.py \
  --runtime-probes \
  --no-auto-fallback-env \
  --json-out target/mps-unsupported-ops/local-no-fallback.json \
  --markdown-out target/mps-unsupported-ops/local-no-fallback.md
```

For compatibility-layer evidence, prefer isolated runtime probes. PyTorch can
emit some MPS fallback warnings only once per process, so a single-process probe
can undercount later fallbacks. Isolated mode runs each probe in a fresh Python
process:

```bash
PYTORCH_ENABLE_MPS_FALLBACK=1 scripts/probe-mps-unsupported-ops.py \
  --runtime-probes \
  --isolated-runtime-probes \
  --json-out target/mps-unsupported-ops/raw-isolated.json \
  --markdown-out target/mps-unsupported-ops/raw-isolated.md

PYTORCH_ENABLE_MPS_FALLBACK=1 scripts/probe-mps-unsupported-ops.py \
  --runtime-probes \
  --isolated-runtime-probes \
  --install-psyche-compat \
  --json-out target/mps-unsupported-ops/psyche-isolated.json \
  --markdown-out target/mps-unsupported-ops/psyche-isolated.md

PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/probe-mps-unsupported-ops.py \
  --runtime-probes \
  --isolated-runtime-probes \
  --install-psyche-compat \
  --no-auto-fallback-env \
  --json-out target/mps-unsupported-ops/psyche-isolated-no-fallback.json \
  --markdown-out target/mps-unsupported-ops/psyche-isolated-no-fallback.md

PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/probe-mps-unsupported-ops.py \
  --runtime-probes \
  --isolated-runtime-probes \
  --install-psyche-compat \
  --enable-experimental-psyche-routes \
  --no-auto-fallback-env \
  --json-out target/mps-unsupported-ops/psyche-isolated-experimental-no-fallback.json \
  --markdown-out target/mps-unsupported-ops/psyche-isolated-experimental-no-fallback.md

PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/probe-mps-unsupported-ops.py \
  --runtime-probes \
  --isolated-runtime-probes \
  --install-psyche-compat \
  --enable-experimental-psyche-routes \
  --enable-approximate-svd-probe \
  --no-auto-fallback-env \
  --json-out target/mps-unsupported-ops/psyche-isolated-experimental-approx-svd-no-fallback.json \
  --markdown-out target/mps-unsupported-ops/psyche-isolated-experimental-approx-svd-no-fallback.md
```

The dispatcher inventory is intentionally broad. On the local torch `2.10.0` stack used during this work, PyTorch reported:

- `3659` registered operators.
- `862` operators with direct MPS kernels.
- `2797` operators without direct MPS kernels.
- `316` operators with no direct MPS or composite kernel but a CPU kernel, making them likely CPU-fallback or not-implemented candidates.

The `316` likely candidates are the useful work queue. The biggest namespaces in that queue were `aten`, `quantized`, `c10d`, `onednn`, `prepacked`, and `sparse`. Not all are worth fixing for Psyche. `c10d`, `onednn`, and prepacked CPU/mobile kernels are usually not Apple GPU compute work. The priority is `aten` operators that appear in real model execution.

Runtime probes are higher-signal than dispatcher counts. If a runtime probe emits an MPS fallback warning, that op actually crossed to CPU on this machine. If it fails with `PYTORCH_ENABLE_MPS_FALLBACK=0`, it is a candidate for the compatibility layer.

Fallback warnings are not perfectly consistent across PyTorch operators, so promotion tests use `PYTORCH_ENABLE_MPS_FALLBACK=0` as the final source of truth. An op is only claimed when the no-fallback baseline fails and the compatibility route passes.

The isolated probe path currently shows this local evidence:

- Raw PyTorch with fallback enabled: `27` ok, `19` CPU fallback, `2`
  unsupported platform boundaries.
- Psyche compat installed with fallback enabled: `42` ok, `4` CPU fallback,
  `2` unsupported boundaries.
- Psyche compat installed with fallback disabled: the default exact route probes
  stay `ok`; remaining linear-algebra and sparse/dtype gaps are
  reported as fallback or unsupported instead of being claimed.

The fixed compatibility probes are `take`, `take.out`, `heaviside`, `logit_`,
`channel_shuffle`, `logspace`, `logspace.out`, `mvlgamma.out`, `vdot`, `frexp`,
`gcd`, `lcm`, `std.correction_out`, `var.correction_out`,
`adaptive_avg_pool3d`, `adaptive_avg_pool3d_backward`, and `geqrf`. Each of those
rows is backed by an installed Psyche route in the isolated report and remains
`ok` when fallback is disabled. Correctness proof still comes from
`scripts/check-mps-compat.py`, which compares the registered routes against CPU
PyTorch across targeted shape, dtype, `.out`, and autograd cases.

Runtime `ok` is execution evidence for the specific probe shape, not a semantic
proof and not proof that Psyche handled the route. Check `psyche_route_state` in
the JSON/Markdown report before attributing an `ok` row to Psyche. Runtime
`unsupported` is a known boundary, not support. A `fallback` row remains a
no-go for honest GPU support even if it appears in a fallback-disabled run; on
this stack, raw `linalg_svd` still emits a fallback warning and must not be
claimed as an exact MPS-resident route.

The experimental route flag is only for separately-gated routes whose numerical
contracts need extra scrutiny. With `--enable-experimental-psyche-routes` and
fallback disabled, the isolated report currently shows `44` ok, `2` fallback,
and `2` unsupported rows. It moves `linalg_qr` and `linalg_matrix_exp` from
`unsupported`/`disabled_by_env` to `ok`/`installed`. That is evidence for the
gated QR and matrix-exp routes only; it does not claim `linalg_eigh`,
`linalg_svd`, Sparse CSR, or float64 support.

`--enable-approximate-svd-probe` is different from
`--enable-experimental-psyche-routes`. It runs only the `linalg_svd` runtime
probe through `MpsCompatibilityMode(allow_approximate_svd=True)` and marks the
row with
`psyche_experimental_probe_route_state=experimental_approximate_svd_dispatch`
when that diagnostic dispatch mode actually intercepts the op.
`psyche_route_state` stays `not_a_psyche_route` because no `aten::linalg_svd`
MPS kernel is registered.
With both experimental exact routes and the approximate SVD diagnostic enabled,
the isolated fallback-disabled report currently shows `45` ok, `1` fallback,
and `2` unsupported rows; that is research evidence for this reduced SVD probe
only, not exact PyTorch SVD support.

Dispatcher counts are supporting evidence only: with default Psyche
compatibility installed, direct MPS registrations moved from `862` to `886`,
and likely CPU fallback/not-implemented candidates moved from `316` to `295`.
With the experimental QR/matrix-exp routes enabled, direct MPS registrations
move to `890`, and likely CPU fallback/not-implemented candidates move to
`292`.

## Harvest Upstream Decompositions

PyTorch already carries many Python decompositions in `torch._decomp`. Use the harvester to find missing-MPS operators that have an upstream decomposition before writing new math:

```bash
scripts/harvest-mps-decompositions.py \
  --json-out target/mps-unsupported-ops/decomposition-candidates.json \
  --markdown-out target/mps-unsupported-ops/decomposition-candidates.md
```

On the local torch `2.10.0` stack, this found `38` likely fallback operators with upstream decompositions out of `1124` decomposition table entries. The harvester is triage, not proof. Every candidate still needs a no-fallback runtime probe and compatibility test before it is claimed.

## Compatibility Layer

The compatibility layer lives in:

```bash
python/python/psyche/mps_compat.py
```

Enable it for Python-backed MPS model forwards:

```bash
PSYCHE_MPS_COMPAT=1
```

`PSYCHE_CUDA_COMPAT=1` also enables these exact routes automatically when a
Psyche-owned CUDA-shaped device request is redirected to MPS. Use
`PSYCHE_CUDA_COMPAT_MPS_ROUTES=0` when you need to audit raw PyTorch MPS behavior
under CUDA-shaped device translation; `0`, `false`, `no`, and `off` are accepted
false spellings.

The production path registers specific replacements at the MPS dispatch key with `torch.library`. This keeps normal PyTorch MPS kernels on their native path and only intercepts operators we explicitly claim.

Kernel registration is process-global. Once `install_mps_compat_kernels()` runs, registered MPS implementations remain active for the lifetime of the Python process. `PSYCHE_MPS_COMPAT=1` gates installation, not per-call dispatch after installation.

`TorchDispatchMode` remains in the module for discovery and experiments, but it is not the hot path.

## Current Routes

| Operator | Route | Status |
| --- | --- | --- |
| `aten::_adaptive_avg_pool3d` | Exact Python decomposition into MPS slicing, `mean`, and `stack` over adaptive bins. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`. |
| `aten::_adaptive_avg_pool3d_backward` | Exact gradient redistribution over adaptive bins on MPS. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`. |
| `aten::heaviside` / `.out` | Exact elementwise route using MPS comparisons and `where`, with CPU-matching NaN and autograd behavior. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`. |
| `aten::gcd` / `.out` | Fixed-iteration Euclidean route using MPS integer operations, bool/integer promotion, signed-min parity, and CPU-like `.out` casting. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`. |
| `aten::lcm` / `.out` | Exact integer route built from the MPS `gcd` route, integer division, multiplication, zero masking, overflow-sign parity checks, and CPU-like `.out` casting/overlap behavior. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`. |
| `aten::std.correction_out` / `aten::var.correction_out` | Exact real-floating `.out` shims over native MPS `std.correction` and `var.correction`, including dtype casts and non-contiguous outputs. Expanded or internally overlapping outputs are rejected instead of emulating CPU storage quirks. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`; complex inputs are deliberately deferred. |
| `aten::take` / `.out` | Exact indexing decomposition: validate bounds on MPS, normalize negative indices, gather from the logical flattened MPS tensor, and copy into same-dtype `.out` tensors with strict overlap rejection. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`; complex inputs are deliberately deferred. |
| `aten::logit_` | In-place real-floating route with MPS elementwise clamp/log handling for `eps <= 0.5` and autograd version-error parity for repeated-use in-place graphs. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`; complex, integer, bool, float64, and `eps > 0.5` are deliberately rejected. |
| `aten::_addmm_activation` / `.out` | Exact MPS decomposition through `addmm` and default GELU or ReLU, preserving `beta=0` input-NaN suppression and strict `.out` device/dtype/overlap guards. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`; default eager backward matches CPU `NotImplemented`; MPS-scoped decomposition autograd is opt-in via `PSYCHE_MPS_COMPAT_ADDMM_ACTIVATION_GRAD=1`. |
| `aten::channel_shuffle` | Exact reshape/transpose/contiguous decomposition over MPS tensors. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`. |
| `aten::logspace` / `.out` | MPS factory decomposition using `linspace` for exponents and `pow` on Apple GPU. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`; limited to real MPS floating dtypes and rejects float64, integer, complex endpoints/results, fractional-exponent negative-base grids, sparse layouts, and pinned memory. |
| `aten::mvlgamma.out` | Exact `.out` shim over the native MPS `mvlgamma` default route, with floating output casts and overlap guards. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`. |
| `aten::vdot` / `.out` | Real-only route over MPS `dot`, preserving integer and floating result dtypes and scalar `.out` behavior. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`; bool and complex inputs are deliberately rejected. |
| `aten::frexp.Tensor` / `aten::frexp.Tensor_out` | MPS mantissa/exponent decomposition with bit-aware float32/float16/bfloat16 handling, signed-zero/special-value parity, int32 exponents, and strict two-output `.out` validation. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`; integer, bool, complex, and float64 inputs are deliberately rejected. |
| `aten::geqrf` | Householder QR factorization over MPS tensors, returning PyTorch/LAPACK-style packed reflectors and `tau` so `torch.orgqr` can reconstruct Q. | Experimental; enabled by `PSYCHE_MPS_COMPAT=1`; currently real `float32` only, with complex/float16/bfloat16 and autograd deliberately rejected. |
| `aten::linalg_matrix_exp` / `.out` | Scaling-and-squaring with a Padé `[13/13]` approximant using MPS matmul and `linalg.solve`. | Gated by `PSYCHE_MPS_COMPAT_MATRIX_EXP=1`. |
| `aten::linalg_qr` / `.out` | Reorthogonalized modified Gram-Schmidt QR over MPS matmul and reductions, with `reduced`, `complete`, and `r` modes. Validates reconstruction, orthogonality, rank-deficient orthonormal completion, `.out`, and reconstruction-loss autograd. | Gated by `PSYCHE_MPS_COMPAT_QR=1`; real floating tensors only and not rank-revealing. |
| SVD helper | Experimental power-iteration/deflation route using MPS matmul and reductions only. | Explicit helper only; not registered as `aten::linalg_svd`. |

The exact adaptive pooling route is the first real proof point: with `PYTORCH_ENABLE_MPS_FALLBACK=0`, forward and backward return MPS tensors and match CPU PyTorch within float tolerance on the tested cases. It is still a Python decomposition, not a fused Metal kernel, so benchmark it before relying on it in a hot training path.

The `heaviside` route is a small exact decomposition, but it has two non-obvious parity edges. CPU PyTorch returns `0` for NaN inputs, so the route initializes from zeros and only overwrites `input > 0` and `input == 0` lanes. CPU PyTorch allows forward execution with `requires_grad=True` but errors on backward, so the compatibility route uses an autograd barrier instead of exposing fake `where` gradients.

The `gcd` route is deliberately fixed-iteration instead of using a data-dependent host loop. It avoids `.any()`, `.item()`, and `.cpu()` inside the registered path. It also avoids MPS integer `abs`: on the local torch `2.10.0` stack, MPS `abs` saturates or mis-handles signed integer minima for several dtypes, while CPU PyTorch keeps overflow-shaped behavior. The route uses `torch.where(x < 0, -x, x)`, dtype-specific modulo behavior, and `128` int64 rounds to cover the Fibonacci worst case without CPU synchronization.

The `lcm` route reuses the MPS `gcd` route and computes `abs(a / gcd(a, b) * b)` with explicit zero handling. It intentionally preserves the local CPU overflow quirks: `int8` and `int16` can retain wrapped signed artifacts, while `int32` and `int64` normalize the final sign. Its `.out` route computes in the promoted integer result dtype and then uses MPS `copy_` casting so integer, float, and complex outputs match CPU behavior, while bool outputs, partial overlaps, and expanded outputs are rejected.

The `std.correction_out` and `var.correction_out` routes are intentionally narrow `.out` shims: the default reductions already have native MPS kernels, but the `.out` overloads do not. The shim computes the native MPS default result and copies it into the caller-provided MPS output with CPU-compatible casting and resizing. Normal strided outputs are supported, but expanded or internally overlapping outputs are rejected because CPU's storage-slot behavior there is too brittle to bless as a GPU compatibility contract. Complex reductions are not registered yet because the local MPS default complex variance/std behavior differs from CPU. `aten::narrow_copy.out` is also deferred: CPU accepts non-contiguous and expanded outputs with storage-order behavior that a naive `out.copy_` implementation would not match.

The `take` route uses supported MPS gather behavior rather than a custom Metal kernel. Its bounds check is mandatory: raw MPS gather returned bogus values for out-of-range indices on the tested stack, so the route validates `index >= -input.numel()` and `index < input.numel()` before gather and raises `IndexError` on failure. That validation introduces a synchronization point, but skipping it would trade speed for silent corruption. The route matches CPU semantics for scalar inputs, scalar indices, empty indices, negative wrapping, non-contiguous inputs and indices, bool/integer/float dtypes, and same-dtype `.out` copies. It deliberately rejects complex inputs because MPS gather does not support complex tensors. The Python `.out` shim resizes wrong-shaped outputs correctly, but it does not reproduce PyTorch's native resize warning.

The `logit_` route exists because the in-place overload is missing on MPS even though the normal math can be expressed with native elementwise operations. For `eps <= 0.5`, the route stays on MPS and applies CPU-compatible clamp/log semantics. For `eps > 0.5`, PyTorch's CPU kernel exposes vectorization-dependent conflict behavior for an invalid-looking clamp range, so the route rejects that range instead of copying the artifact into a new backend or staging through CPU. Repeated-use in-place autograd graphs are expected to raise the same version-counter error as CPU.

The `_addmm_activation` route targets a transformer-adjacent fused MLP primitive that fails on the tested MPS stack by reaching the missing `.out` overload. The route follows PyTorch's own decomposition: `addmm` first, then exact/default GELU for non-CUDA backends or ReLU when `use_gelu=False`. It deliberately calls `torch.addmm` instead of expanding `beta * input + alpha * matmul` so `beta=0` keeps PyTorch's promise that NaN and Inf values in the input term are ignored. Direct CPU eager backward for `aten::_addmm_activation` raises `derivative for aten::_addmm_activation is not implemented`; the default MPS route preserves that behavior. For training experiments, set `PSYCHE_MPS_COMPAT_ADDMM_ACTIVATION_GRAD=1` before installing kernels to register an `AutogradMPS` formula over the decomposition. The opt-in backward is scoped to MPS tensors; the regression check proves CPU eager backward still raises while the flag is enabled.

The `channel_shuffle` route is pure layout work: validate rank and group divisibility, reshape channels into groups, transpose group/channel axes, reshape back, and materialize a contiguous MPS tensor. It is not a transformer priority, but it is a clean proof that missing ATen layout helpers can often be promoted without a custom Metal kernel.

The `logspace` route is an honest MPS factory route, not a hidden CPU emulation. CPU PyTorch uses a float64 intermediate for many real cases, but MPS has no float64 support, so this route only claims real `float16`, `bfloat16`, and `float32` outputs within tested tolerances. It rejects float64, integer, complex endpoints/results, fractional-exponent negative-base grids, sparse-layout, and pinned-memory variants instead of silently downcasting or copying CPU behavior. Real negative bases are routed only when every generated exponent is an integer; fractional-exponent negative-base grids are rejected because CPU and MPS `linspace` rounding can change the `nan` mask. For example, `torch.logspace(0, 3, 4, base=-2, device="mps")` is supported. The `.out` overload computes directly for the output dtype on MPS.

The `mvlgamma.out` route exists because `torch.mvlgamma(input, p)` already runs on MPS on the tested stack while the `.out` overload does not. The shim computes the native MPS default result and copies it into floating outputs, including CPU-compatible float16 casts, while rejecting integer outputs and overlapping writes.

The `vdot` route uses MPS `dot` for real integer and floating vectors. Complex `vdot` is deliberately deferred because the first-argument conjugation contract and MPS complex reductions need their own parity work. Bool inputs are also rejected to match CPU `dot`/`vdot` behavior.

The `frexp` route needed a different shape of decomposition than the simpler elementwise routes. A logarithm-only implementation works for many normal values but fails for subnormal and power-of-two boundary cases on this MPS stack, so the route views float32 lanes as int32 bits and float16/bfloat16 lanes as int16 bits, then rebuilds normalized mantissas/exponents on the GPU. The tests assert not only reconstruction but also the real `frexp` invariant that every finite nonzero mantissa has `0.5 <= abs(mantissa) < 1.0`. The `.out` overload returns the exact caller-provided output tensors, resizes them like PyTorch, allows the mantissa output to alias the input exactly, and rejects partial overlaps or wrong exponent dtypes.

The matrix exponential route is the second exact-intent route. It is separately gated because it owns a numerical algorithm rather than forwarding to an upstream decomposition. The current implementation supports real `float32`, `float16`, and `bfloat16` inputs, computes internally in `float32`, rejects complex and non-MPS `float64`, and tests both direct ATen overloads and autograd parity with fallback disabled.

The SVD route is intentionally not registered as an `aten` compatibility kernel. It is useful as a research path because it avoids CPU fallback, `linalg_eigh`, and `linalg_qr`, but it is approximate, does not provide PyTorch-compatible SVD autograd semantics, and can fail on difficult spectra, repeated singular values, or larger matrices without more numerical work.

The QR route is intentionally narrower than PyTorch's full LAPACK-style surface. On the local torch `2.10.0` stack, `torch.linalg.qr` fails with `PYTORCH_ENABLE_MPS_FALLBACK=0` through `aten::linalg_qr.out`, and `torch.linalg.householder_product` already has native MPS support. The compatibility route covers `torch.linalg.qr` directly with a scale-aware dependent-column threshold and one reorthogonalization pass, but it is not rank-revealing.

The `geqrf` route covers the lower-level LAPACK-style contract separately. It uses Householder reflectors on MPS, returns the packed reflector matrix plus `tau`, and the tests compare both raw outputs against CPU PyTorch and reconstruct with `torch.orgqr` for tall/square cases. It intentionally does not claim autograd, complex tensors, or half/bfloat16 parity yet; CPU PyTorch also reports `geqrf` autograd as unimplemented on this stack.
This route is correctness-first and Python-looped over columns; it is not a
fused Metal QR kernel. The finite `float32` scope is tested against CPU packed
output, including degenerate columns, rank-1 shapes, mixed batches,
non-contiguous input, and large finite values.

Check the routes:

```bash
PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/check-mps-compat.py
PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/check-mps-compat.py --matrix-exp
PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/check-mps-compat.py --qr
PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/check-mps-compat.py --experimental-approx-svd
scripts/check-mps-probe-harness.py
```

## Promotion Rules

Every new compatibility route should move through this sequence:

1. Add or extend a runtime probe that reproduces the unsupported op.
2. Prove the baseline fails or falls back with MPS.
3. Add an exact decomposition, MPSGraph bridge, custom Metal kernel, or explicitly gated approximation.
4. Run with `PYTORCH_ENABLE_MPS_FALLBACK=0` to prove no CPU fallback is required.
5. Compare CPU and MPS outputs across shape and dtype cases.
6. Benchmark against CPU fallback so the route is not merely correct but worth using.
7. Document limitations, especially approximate math, unsupported dtypes, and shape restrictions.

## Backlog

High-value next routes:

- Harvest PyTorch's own decomposition table for missing MPS ops, then register exact decompositions whose primitive ops already have MPS coverage.
- Add exact decompositions for more pooling and reduction-shaped ops before touching harder linear algebra.
- Replace the composed `aten::lcm` route with a single custom Metal kernel if profiling shows kernel-launch and memory-bandwidth overhead in integer-heavy workloads.
- Decide whether to emulate `aten::narrow_copy.out` storage-order behavior for non-contiguous and expanded outputs, or leave it unclaimed until PyTorch semantics become less surprising.
- Add complex `std/var.correction_out` support with an explicit CPU-compatible complex variance formula instead of relying on the local MPS default complex behavior.
- Extend the `aten::geqrf` route beyond float32 only if complex and half/bfloat16 parity can be proven against the relevant PyTorch contracts.
- Reuse the gated QR and default `geqrf` routes to strengthen the explicit SVD/eigensolver research helpers, then decide whether a Householder QR should replace the modified Gram-Schmidt route for numerical stability on larger matrices.
- Treat quantized/prepacked LLM paths as rewrite candidates: dequantize to MPS dense, use MLX-style packed matmul, or add Metal int4/int8 kernels instead of accepting CPU-only quantized ops.
- Avoid Sparse CSR as a first target. PyTorch currently lacks honest MPS CSR tensor storage, so dense substitutes or model-boundary rewrites are more realistic than pretending CSR is supported.

Hard limits:

- `float64` is not supported by MPS tensors. That is a dtype boundary, not a missing Python decomposition.
- Some distributed `c10d` operators are communication/control-plane work and should not be forced onto MPS just because they appear in the dispatcher gap list.
