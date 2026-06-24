#!/usr/bin/env python3
"""
Inventory PyTorch MPS operator gaps for the current local torch build.

This script has two jobs:

1. Dispatcher inventory:
   list every registered PyTorch operator that does not have a direct MPS
   kernel in this build. This is the broad "what could fall back" map.

2. Runtime probes:
   execute representative transformer/training and historically-problematic
   operations on MPS while `PYTORCH_ENABLE_MPS_FALLBACK=1` is active, recording
   any CPU fallback warnings or hard failures.

The dispatcher list is intentionally broader than the true failure set because
many ops decompose through CompositeImplicitAutograd/CompositeExplicitAutograd
and still run on MPS via lower-level kernels. Treat runtime fallback warnings
as the higher-signal evidence.
"""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import json
import os
import platform
import re
import subprocess
import sys
import time
import warnings
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable


FALLBACK_PATTERN = re.compile(
    r"The operator '([^']+)' is not currently supported on the MPS backend"
)
PROBE_JSON_SENTINEL = "__PSYCHE_PROBE_JSON__="
STDOUT_TAIL_BYTES = 4000
EXPERIMENTAL_ROUTE_ENV = {
    "PSYCHE_MPS_COMPAT_MATRIX_EXP": "1",
    "PSYCHE_MPS_COMPAT_QR": "1",
}
DEFAULT_APPROXIMATE_SVD_ITERATIONS = 64
PROBE_COMPAT_ROUTES = {
    "adaptive_avg_pool3d": ("aten::_adaptive_avg_pool3d.default",),
    "adaptive_avg_pool3d_backward": ("aten::_adaptive_avg_pool3d_backward.default",),
    "heaviside": ("aten::heaviside.default", "aten::heaviside.out"),
    "gcd": ("aten::gcd.default", "aten::gcd.out"),
    "lcm": ("aten::lcm.default", "aten::lcm.out"),
    "std_correction_out": ("aten::std.correction_out",),
    "var_correction_out": ("aten::var.correction_out",),
    "take": ("aten::take.default",),
    "take_out": ("aten::take.out",),
    "logit_inplace": ("aten::logit_",),
    "channel_shuffle": ("aten::channel_shuffle.default",),
    "logspace": ("aten::logspace.default",),
    "logspace_out": ("aten::logspace.out",),
    "mvlgamma_out": ("aten::mvlgamma.out",),
    "vdot": ("aten::vdot.default", "aten::vdot.out"),
    "frexp": ("aten::frexp.Tensor", "aten::frexp.Tensor_out"),
    "geqrf": ("aten::geqrf.default",),
    "linalg_matrix_exp": ("aten::linalg_matrix_exp.default", "aten::linalg_matrix_exp.out"),
    "linalg_qr": ("aten::linalg_qr.default", "aten::linalg_qr.out"),
}


@dataclass(frozen=True)
class Probe:
    name: str
    category: str
    route: str
    fn: Callable[[Any], Any]
    expected_unsupported_error: str | tuple[str, ...] | None = None


def import_torch():
    import torch
    import torch.nn.functional as F

    return torch, F


def enable_experimental_psyche_routes() -> None:
    for name, value in EXPERIMENTAL_ROUTE_ENV.items():
        os.environ[name] = value


def install_psyche_mps_compat() -> dict[str, Any]:
    try:
        from psyche.mps_compat import install_mps_compat_kernels
    except ModuleNotFoundError as exc:
        if exc.name != "psyche":
            raise
        module_path = (
            Path(__file__).resolve().parents[1]
            / "python/python/psyche/mps_compat.py"
        )
        spec = importlib.util.spec_from_file_location("psyche_mps_compat_probe", module_path)
        module = importlib.util.module_from_spec(spec)
        assert spec and spec.loader
        spec.loader.exec_module(module)
        install_mps_compat_kernels = module.install_mps_compat_kernels

    install_result = install_mps_compat_kernels()
    return {
        "installed": list(install_result.installed),
        "already_registered": list(install_result.already_registered),
        "skipped_existing_mps": list(install_result.skipped_existing_mps),
        "disabled_by_env": list(install_result.disabled_by_env),
    }


def load_psyche_mps_dispatch_mode() -> tuple[type[Any], type[Any]]:
    try:
        from psyche.mps_compat import MpsCompatStats, MpsCompatibilityMode
    except ModuleNotFoundError as exc:
        if exc.name != "psyche":
            raise
        module_path = (
            Path(__file__).resolve().parents[1]
            / "python/python/psyche/mps_compat.py"
        )
        spec = importlib.util.spec_from_file_location("psyche_mps_compat_probe", module_path)
        module = importlib.util.module_from_spec(spec)
        assert spec and spec.loader
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        MpsCompatStats = module.MpsCompatStats
        MpsCompatibilityMode = module.MpsCompatibilityMode

    return MpsCompatibilityMode, MpsCompatStats


def psyche_route_state_for_probe(
    probe_name: str,
    install_result: dict[str, Any] | None,
) -> str:
    if install_result is None:
        return "psyche_not_installed"
    routes = PROBE_COMPAT_ROUTES.get(probe_name)
    if not routes:
        return "not_a_psyche_route"

    route_sets = {
        "installed": set(install_result.get("installed", [])),
        "already_registered": set(install_result.get("already_registered", [])),
        "skipped_existing_mps": set(install_result.get("skipped_existing_mps", [])),
        "disabled_by_env": set(install_result.get("disabled_by_env", [])),
    }
    for state in ("installed", "already_registered", "skipped_existing_mps", "disabled_by_env"):
        if any(route in route_sets[state] for route in routes):
            return state
    return "not_registered"


def _svd_replacement_count(replacements: dict[str, int]) -> int:
    return sum(
        count
        for op, count in replacements.items()
        if op == "aten::linalg_svd" or op.startswith("aten::linalg_svd.")
    )


def psyche_experimental_probe_route_state_for_probe(
    probe_name: str,
    approximate_svd_probe_enabled: bool,
    replacements: dict[str, int] | None = None,
) -> str:
    if probe_name != "linalg_svd":
        return "not_applicable"
    if not approximate_svd_probe_enabled:
        return "disabled"
    if replacements and _svd_replacement_count(replacements) > 0:
        return "experimental_approximate_svd_dispatch"
    return "enabled_not_used"


def annotate_runtime_probe_routes(
    runtime: list[dict[str, Any]],
    install_result: dict[str, Any] | None,
) -> list[dict[str, Any]]:
    for row in runtime:
        row["psyche_route_state"] = psyche_route_state_for_probe(row["name"], install_result)
    return runtime


def parse_probe_json_from_stdout(stdout: str) -> dict[str, Any]:
    for line in reversed(stdout.splitlines()):
        if line.startswith(PROBE_JSON_SENTINEL):
            return json.loads(line[len(PROBE_JSON_SENTINEL) :])
    raise json.JSONDecodeError(
        f"missing {PROBE_JSON_SENTINEL!r} line",
        stdout,
        0,
    )


def ensure_fallback_env(argv: list[str]) -> None:
    if "--runtime-probes" not in argv and "--single-runtime-probe" not in argv:
        return
    if "--no-auto-fallback-env" in argv:
        return
    if os.environ.get("PYTORCH_ENABLE_MPS_FALLBACK") == "1":
        return

    env = dict(os.environ)
    env["PYTORCH_ENABLE_MPS_FALLBACK"] = "1"
    os.execve(sys.executable, [sys.executable, *argv], env)


def has_kernel(torch: Any, op_name: str, key: str) -> bool:
    try:
        return bool(torch._C._dispatch_has_kernel_for_dispatch_key(op_name, key))
    except RuntimeError:
        return False


def dispatcher_inventory(torch: Any) -> dict[str, Any]:
    ops = sorted(torch._C._dispatch_get_all_op_names())
    rows: list[dict[str, Any]] = []
    counts: Counter[str] = Counter()
    namespaces: Counter[str] = Counter()
    likely_fallback_namespaces: Counter[str] = Counter()

    for op in ops:
        direct_mps = has_kernel(torch, op, "MPS")
        composite_implicit = has_kernel(torch, op, "CompositeImplicitAutograd")
        composite_explicit = has_kernel(torch, op, "CompositeExplicitAutograd")
        cpu = has_kernel(torch, op, "CPU")
        namespace = op.split("::", 1)[0] if "::" in op else "_unknown"

        if direct_mps:
            classification = "direct_mps"
        elif composite_implicit or composite_explicit:
            classification = "composite_candidate"
        elif cpu:
            classification = "likely_cpu_fallback_or_not_implemented"
            likely_fallback_namespaces[namespace] += 1
        else:
            classification = "no_mps_no_cpu_or_special_backend"

        counts[classification] += 1
        namespaces[namespace] += 1
        rows.append(
            {
                "op": op,
                "namespace": namespace,
                "classification": classification,
                "has_mps": direct_mps,
                "has_composite_implicit": composite_implicit,
                "has_composite_explicit": composite_explicit,
                "has_cpu": cpu,
            }
        )

    missing_direct = [row for row in rows if not row["has_mps"]]
    likely_fallback = [
        row for row in rows if row["classification"] == "likely_cpu_fallback_or_not_implemented"
    ]

    return {
        "total_ops": len(rows),
        "classification_counts": dict(counts),
        "namespace_counts": dict(namespaces),
        "likely_fallback_namespace_counts": dict(likely_fallback_namespaces),
        "missing_direct_mps_count": len(missing_direct),
        "likely_cpu_fallback_or_not_implemented_count": len(likely_fallback),
        "operators": rows,
    }


def make_probes(torch: Any, F: Any) -> list[Probe]:
    d = "mps"

    def backward_scalar(value: Any) -> Any:
        loss = value if getattr(value, "ndim", 0) == 0 else value.float().sum()
        loss.backward()
        return value

    def adamw_step() -> Any:
        model = torch.nn.Sequential(
            torch.nn.Linear(16, 32),
            torch.nn.GELU(),
            torch.nn.Linear(32, 8),
        ).to(d, dtype=torch.float16)
        opt = torch.optim.AdamW(model.parameters(), lr=1e-3)
        x = torch.randn(4, 16, device=d, dtype=torch.float16)
        y = model(x).float().pow(2).mean()
        y.backward()
        opt.step()
        return next(model.parameters())

    def embedding_backward() -> Any:
        emb = torch.nn.Embedding(128, 16).to(d)
        idx = torch.randint(0, 128, (4, 12), device=d)
        return backward_scalar(emb(idx).float().pow(2).mean())

    def cross_entropy_backward() -> Any:
        logits = torch.randn(4, 17, device=d, requires_grad=True)
        target = torch.randint(0, 17, (4,), device=d)
        return backward_scalar(F.cross_entropy(logits, target))

    def sdpa_backward() -> Any:
        q = torch.randn(2, 2, 8, 16, device=d, requires_grad=True)
        k = torch.randn(2, 2, 8, 16, device=d, requires_grad=True)
        v = torch.randn(2, 2, 8, 16, device=d, requires_grad=True)
        return backward_scalar(F.scaled_dot_product_attention(q, k, v).sum())

    def rms_norm_backward() -> Any:
        x = torch.randn(4, 8, 16, device=d, requires_grad=True)
        weight = torch.ones(16, device=d, requires_grad=True)
        if hasattr(F, "rms_norm"):
            return backward_scalar(F.rms_norm(x, (16,), weight=weight))
        normed = x * torch.rsqrt(x.pow(2).mean(dim=-1, keepdim=True) + 1e-6)
        return backward_scalar(normed * weight)

    def sparse_coo_to_mps() -> Any:
        indices = torch.tensor([[0, 1, 1], [2, 0, 2]], device=d)
        values = torch.tensor([3.0, 4.0, 5.0], device=d)
        return torch.sparse_coo_tensor(indices, values, (2, 3), device=d)

    def adaptive_avg_pool3d_backward() -> Any:
        x = torch.randn(1, 1, 4, 4, 4, device=d, requires_grad=True)
        y = F.adaptive_avg_pool3d(x, output_size=(2, 2, 2))
        y.sum().backward()
        return x.grad

    return [
        Probe(
            "linear_gelu_adamw_step",
            "transformer_training",
            "Keep on native MPS; if optimizer foreach/fused paths fall back, add MPS optimizer kernels or route through MLX-style fused AdamW.",
            adamw_step,
        ),
        Probe(
            "embedding_backward",
            "transformer_training",
            "If embedding scatter-add falls back, implement GPU index-add/scatter-add via MPSGraph scatter or a Metal atomic accumulation kernel.",
            embedding_backward,
        ),
        Probe(
            "cross_entropy_backward",
            "transformer_training",
            "Prefer decomposed log_softmax + nll_loss on MPS; for large vocabularies, add fused Metal cross entropy to avoid materializing logits twice.",
            cross_entropy_backward,
        ),
        Probe(
            "scaled_dot_product_attention_backward",
            "transformer_training",
            "Use PyTorch SDPA on MPS when supported; longer term, port a tiled flash-attention-like Metal kernel using simdgroup matrix ops.",
            sdpa_backward,
        ),
        Probe(
            "rms_norm_backward",
            "transformer_training",
            "Keep as reductions + pointwise ops or add a fused Metal RMSNorm kernel for bandwidth-bound transformer blocks.",
            rms_norm_backward,
        ),
        Probe(
            "index_select",
            "indexing",
            "Use MPSGraph gather when shapes are dense; use custom Metal gather for irregular layouts.",
            lambda: torch.index_select(
                torch.randn(8, 16, device=d),
                0,
                torch.tensor([0, 2, 7], device=d),
            ),
        ),
        Probe(
            "gather",
            "indexing",
            "Bridge to MPSGraph gather; for repeated patterns cache index tensors as PyTorch already does for view materialization.",
            lambda: torch.gather(
                torch.randn(4, 8, device=d),
                1,
                torch.tensor([[0, 1, 2, 3], [3, 2, 1, 0], [4, 5, 6, 7], [7, 6, 5, 4]], device=d),
            ),
        ),
        Probe(
            "scatter_add",
            "indexing",
            "Implement with Metal atomics for high-contention reductions; MPSGraph scatter is enough for low-contention updates.",
            lambda: torch.zeros(4, 8, device=d).scatter_add(
                1,
                torch.tensor([[0, 1, 2, 3], [3, 2, 1, 0], [4, 5, 6, 7], [7, 6, 5, 4]], device=d),
                torch.randn(4, 4, device=d),
            ),
        ),
        Probe(
            "take",
            "indexing",
            "Exact GPU decomposition: validate index bounds on MPS, normalize negative indices, then gather from the flattened MPS tensor.",
            lambda: torch.take(
                torch.arange(12, device=d).reshape(3, 4),
                torch.tensor([0, 5, -1, -12], device=d),
            ),
        ),
        Probe(
            "take_out",
            "indexing",
            "Exact .out shim over the MPS take decomposition, with same-dtype output and strict overlap rejection.",
            lambda: torch.take(
                torch.arange(12, device=d).reshape(3, 4),
                torch.tensor([0, 5, -1, -12], device=d),
                out=torch.empty(4, dtype=torch.int64, device=d),
            ),
        ),
        Probe(
            "index_add",
            "indexing",
            "Use a Metal atomic-add kernel; this is the primitive that would unblock many embedding/optimizer-style updates.",
            lambda: torch.zeros(8, 16, device=d).index_add(
                0,
                torch.tensor([0, 2, 7], device=d),
                torch.randn(3, 16, device=d),
            ),
        ),
        Probe(
            "nonzero",
            "dynamic_shape",
            "Use MPSGraph nonZeroIndices where available; otherwise two-pass Metal prefix-sum compaction.",
            lambda: torch.nonzero(torch.tensor([[1, 0, 2], [0, 0, 3]], device=d)),
        ),
        Probe(
            "masked_select",
            "dynamic_shape",
            "Lower through nonzero + gather; real GPU route needs prefix-sum compaction for dynamic output sizes.",
            lambda: torch.masked_select(
                torch.arange(12, device=d).reshape(3, 4),
                torch.tensor(
                    [[True, False, True, False], [False, True, False, True], [True, True, False, False]],
                    device=d,
                ),
            ),
        ),
        Probe(
            "unique",
            "dynamic_shape",
            "Sort + run-length encode on GPU; use MPSGraph sort if exposed, then custom Metal compaction.",
            lambda: torch.unique(torch.tensor([3, 1, 3, 2, 1, 4], device=d)),
        ),
        Probe(
            "bincount",
            "dynamic_shape",
            "Histogram via Metal atomic increments; promote to 32-bit counters unless int64 is required at the boundary.",
            lambda: torch.bincount(torch.tensor([1, 2, 1, 3, 2, 2], device=d), minlength=5),
        ),
        Probe(
            "topk",
            "sort_select",
            "Use MPSGraph topK when possible; otherwise bitonic/radix selection in Metal for small-k transformer sampling.",
            lambda: torch.topk(torch.randn(64, device=d), k=8).values,
        ),
        Probe(
            "sort",
            "sort_select",
            "Use MPSGraph sort where available; otherwise custom bitonic/radix kernels by dtype.",
            lambda: torch.sort(torch.randn(64, device=d)).values,
        ),
        Probe(
            "searchsorted",
            "sort_select",
            "Implement as parallel binary search over sorted boundaries in Metal.",
            lambda: torch.searchsorted(
                torch.tensor([1, 3, 5, 7, 9], device=d),
                torch.tensor([0, 2, 4, 8, 10], device=d),
            ),
        ),
        Probe(
            "heaviside",
            "elementwise",
            "Exact GPU decomposition: comparisons plus where, preserving NaN-to-zero and backward-not-implemented semantics.",
            lambda: torch.heaviside(
                torch.tensor(
                    [-float("inf"), -1.0, -0.0, 0.0, 1.0, float("nan")],
                    device=d,
                ),
                torch.tensor([0.5], device=d),
            ),
        ),
        Probe(
            "logit_inplace",
            "elementwise",
            "Exact in-place GPU route for eps <= 0.5: MPS clamp/log math plus in-place writeback with custom autograd for grad-capable tensors.",
            lambda: torch.tensor([0.2, 0.5, 0.8], device=d).logit_(),
        ),
        Probe(
            "_addmm_activation",
            "transformer_training",
            "Exact GPU decomposition: MPS addmm followed by default GELU or ReLU; default backward preserves eager PyTorch's NotImplemented behavior, with opt-in decomposition autograd for training experiments.",
            lambda: torch.ops.aten._addmm_activation.default(
                torch.randn(3, 5, device=d),
                torch.randn(3, 4, device=d),
                torch.randn(4, 5, device=d),
                beta=1,
                alpha=1,
                use_gelu=True,
            ),
        ),
        Probe(
            "channel_shuffle",
            "vision",
            "Exact GPU decomposition: reshape channel groups, transpose groups/channels, reshape back, and materialize a contiguous MPS tensor.",
            lambda: torch.channel_shuffle(
                torch.arange(2 * 4 * 3, dtype=torch.float32, device=d).reshape(2, 4, 3),
                2,
            ),
        ),
        Probe(
            "logspace",
            "factory",
            "MPS factory route: linspace exponents and pow on MPS for supported real floating dtypes, with explicit float64, complex endpoint/result, and fractional-exponent negative-base rejection.",
            lambda: torch.logspace(0, 3, 4, device=d),
        ),
        Probe(
            "logspace_out",
            "factory",
            "Exact .out route over the MPS logspace decomposition, with MPS out validation and dtype-limited semantics.",
            lambda: torch.logspace(0, 3, 4, out=torch.empty(0, device=d)),
        ),
        Probe(
            "mvlgamma_out",
            "special",
            "Exact .out shim over native MPS mvlgamma default, preserving cast-compatible output writes.",
            lambda: torch.mvlgamma(
                torch.tensor([3.0, 4.0], device=d),
                2,
                out=torch.empty(0, device=d),
            ),
        ),
        Probe(
            "vdot",
            "linear_algebra",
            "Real-only GPU route over MPS dot; complex vdot is deliberately deferred until conjugation/reduction parity is proven.",
            lambda: torch.vdot(
                torch.arange(4, dtype=torch.float32, device=d),
                torch.arange(4, dtype=torch.float32, device=d),
            ),
        ),
        Probe(
            "frexp",
            "dtype",
            "Bit-aware float32/float16/bfloat16 GPU decomposition preserving subnormals, signed zero, inf/nan, and int32 exponents.",
            lambda: torch.frexp(
                torch.tensor(
                    [-float("inf"), -0.0, 0.0, 0.3, 1.0, 3.0, float("inf"), float("nan")],
                    dtype=torch.float32,
                    device=d,
                )
            ),
        ),
        Probe(
            "gcd",
            "integer",
            "Exact GPU decomposition: fixed-iteration Euclidean loop with CPU-like signed-min behavior and no host-synced while loop.",
            lambda: torch.gcd(
                torch.tensor(
                    [7540113804746346429, -9223372036854775808, 0, 48],
                    dtype=torch.int64,
                    device=d,
                ),
                torch.tensor(
                    [4660046610375530309, 0, -9223372036854775808, -18],
                    dtype=torch.int64,
                    device=d,
                ),
            ),
        ),
        Probe(
            "lcm",
            "integer",
            "Exact GPU decomposition: reuse the MPS gcd route, integer division/multiply, zero masking, and CPU-like overflow/out casting semantics.",
            lambda: torch.lcm(
                torch.tensor(
                    [-9223372036854775808, -9223372036854775807, 0, 3037000500, 48],
                    dtype=torch.int64,
                    device=d,
                ),
                torch.tensor(
                    [-9223372036854775807, -2, 0, 3037000500, -18],
                    dtype=torch.int64,
                    device=d,
                ),
            ),
        ),
        Probe(
            "std_correction_out",
            "reduction",
            "Exact .out shim: compute native MPS std.correction, then copy/cast into MPS out; reject expanded/internal-overlap output.",
            lambda: torch.ops.aten.std.correction_out(
                torch.tensor(
                    [[0.0, 1.0, 2.0, 3.0], [0.0, 2.0, 4.0, 6.0], [0.0, 3.0, 6.0, 9.0]],
                    device=d,
                ),
                [1],
                correction=1,
                keepdim=False,
                out=torch.empty(0, device=d),
            ),
        ),
        Probe(
            "var_correction_out",
            "reduction",
            "Exact .out shim: compute native MPS var.correction, then copy/cast into MPS out; reject expanded/internal-overlap output.",
            lambda: torch.ops.aten.var.correction_out(
                torch.tensor(
                    [[0.0, 1.0, 2.0, 3.0], [0.0, 2.0, 4.0, 6.0], [0.0, 3.0, 6.0, 9.0]],
                    device=d,
                ),
                [1],
                correction=1,
                keepdim=False,
                out=torch.empty(0, device=d),
            ),
        ),
        # The diagnostic approximate helper supports reduced SVD only; keeping
        # this explicit prevents the probe from drifting back to full_matrices=True.
        Probe(
            "linalg_svd",
            "linear_algebra",
            "Exact route unavailable; research mode can use an explicit approximate MPS power-iteration/deflation helper for full_matrices=False.",
            lambda: torch.linalg.svd(torch.randn(8, 8, device=d), full_matrices=False)[1],
        ),
        Probe(
            "linalg_qr",
            "linear_algebra",
            "Implement Householder or modified Gram-Schmidt in Metal; useful as a building block for SVD/eigensolver routes.",
            lambda: torch.linalg.qr(torch.randn(8, 4, device=d))[0],
            expected_unsupported_error="not currently implemented for the MPS device",
        ),
        Probe(
            "geqrf",
            "linear_algebra",
            "Implement Householder factorization on MPS; this is the missing QR factorization primitive on the tested torch stack.",
            lambda: torch.geqrf(torch.randn(8, 4, device=d))[0],
            expected_unsupported_error="not currently implemented for the MPS device",
        ),
        Probe(
            "linalg_householder_product",
            "linear_algebra",
            "Already native MPS on the tested torch stack; use as a QR building block instead of replacing it.",
            lambda: torch.linalg.householder_product(
                torch.randn(8, 4, device=d),
                torch.randn(4, device=d),
            ),
        ),
        Probe(
            "linalg_eigh",
            "linear_algebra",
            "Symmetric eigensolver can be Jacobi or QR iteration in Metal; high effort and not a transformer-training priority.",
            lambda: (
                lambda a: torch.linalg.eigh(a.mT @ a)[0]
            )(torch.randn(8, 8, device=d)),
            expected_unsupported_error="not currently implemented for the MPS device",
        ),
        Probe(
            "linalg_solve",
            "linear_algebra",
            "Route through MPSMatrix LU/triangular solve if exposed; otherwise blocked Gaussian elimination in Metal.",
            lambda: torch.linalg.solve(
                torch.eye(8, device=d) + 0.01 * torch.randn(8, 8, device=d),
                torch.randn(8, 2, device=d),
            ),
        ),
        Probe(
            "linalg_matrix_exp",
            "linear_algebra",
            "Use scaling-and-squaring with Padé approximants on MPS matmul kernels; not a Psyche LLM priority.",
            lambda: torch.linalg.matrix_exp(torch.randn(4, 4, device=d)),
            expected_unsupported_error="not currently implemented for the MPS device",
        ),
        Probe(
            "fft_fft",
            "spectral",
            "Apple MPSGraph has FFT APIs; bridge PyTorch FFT overloads to those before considering custom kernels.",
            lambda: torch.fft.fft(torch.randn(16, device=d)),
        ),
        Probe(
            "histc",
            "histogram",
            "Histogram via Metal atomics; keep bin count small and use per-threadgroup partial histograms to reduce contention.",
            lambda: torch.histc(torch.randn(128, device=d), bins=8),
        ),
        Probe(
            "adaptive_avg_pool3d",
            "vision_3d",
            "Exact GPU decomposition: slice each adaptive bin on MPS, reduce with mean, then stack.",
            lambda: F.adaptive_avg_pool3d(
                torch.randn(1, 1, 4, 4, 4, device=d),
                output_size=(2, 2, 2),
            ),
        ),
        Probe(
            "adaptive_avg_pool3d_backward",
            "vision_3d",
            "Exact GPU decomposition: redistribute each output gradient over its adaptive input bin on MPS.",
            adaptive_avg_pool3d_backward,
        ),
        Probe(
            "max_pool3d",
            "vision_3d",
            "Use MPSGraph pooling if present; lower 3D pooling to tiled Metal if shapes are static.",
            lambda: F.max_pool3d(torch.randn(1, 1, 4, 4, 4, device=d), kernel_size=2),
        ),
        Probe(
            "grid_sample",
            "vision_sampling",
            "Custom Metal bilinear sampler; less relevant to Psyche LLM training but common in vision models.",
            lambda: F.grid_sample(
                torch.randn(1, 1, 4, 4, device=d),
                torch.randn(1, 2, 2, 2, device=d),
                align_corners=False,
            ),
        ),
        Probe(
            "sparse_coo_tensor",
            "sparse",
            "Sparse MPS needs storage-format kernels; for LLM training avoid sparse PyTorch tensors or route to dense/MLX-specific kernels.",
            sparse_coo_to_mps,
        ),
        Probe(
            "to_sparse_csr",
            "sparse",
            "No honest tensor-compatible GPU route until PyTorch has SparseCsrMPS storage; use dense substitutes or a custom wrapper at model boundaries.",
            lambda: torch.randn(4, 4, device=d).to_sparse_csr(),
            expected_unsupported_error=("new_compressed_tensor", "_to_sparse_csr"),
        ),
        Probe(
            "float64_tensor",
            "dtype",
            "Apple MPS does not support float64 tensors; keep double precision on CPU or downcast with explicit error budgets.",
            lambda: torch.ones(4, device=d, dtype=torch.float64),
            expected_unsupported_error="float64",
        ),
        Probe(
            "bfloat16_matmul",
            "dtype",
            "Use runtime BF16 numerical probe; if incorrect, stay fp16 or wait for newer Metal/PyTorch stack.",
            lambda: torch.ones((4, 4), device=d, dtype=torch.bfloat16)
            @ torch.ones((4, 4), device=d, dtype=torch.bfloat16),
        ),
    ]


def simplify_result(value: Any) -> dict[str, Any]:
    if isinstance(value, tuple):
        value = value[0] if value else None
    if hasattr(value, "detach"):
        return {
            "type": "tensor",
            "device": str(value.device),
            "dtype": str(value.dtype),
            "shape": list(value.shape),
        }
    return {"type": type(value).__name__, "repr": repr(value)[:160]}


def run_runtime_probes(
    torch: Any,
    F: Any,
    *,
    only_probe: str | None = None,
    enable_approximate_svd_probe: bool = False,
    approximate_svd_iterations: int = DEFAULT_APPROXIMATE_SVD_ITERATIONS,
) -> list[dict[str, Any]]:
    if not torch.backends.mps.is_available():
        return [
            {
                "name": "_mps_unavailable",
                "status": "skipped",
                "error": "torch.backends.mps.is_available() is false",
            }
        ]

    results = []
    probes = make_probes(torch, F)
    if only_probe is not None:
        probes = [probe for probe in probes if probe.name == only_probe]
        if not probes:
            return [
                {
                    "name": only_probe,
                    "status": "error",
                    "error": f"unknown runtime probe: {only_probe}",
                    "fallback_ops": [],
                }
            ]

    for probe in probes:
        started = time.perf_counter()
        warning_texts: list[str] = []
        fallback_ops: list[str] = []
        stderr_text = ""
        caught_warnings = []
        stderr_buffer = io.StringIO()
        experimental_probe_replacements: dict[str, int] = {}
        approximate_svd_active = enable_approximate_svd_probe and probe.name == "linalg_svd"
        dispatch_context = contextlib.nullcontext()
        if approximate_svd_active:
            MpsCompatibilityMode, MpsCompatStats = load_psyche_mps_dispatch_mode()
            research_stats = MpsCompatStats()
            dispatch_context = MpsCompatibilityMode(
                allow_approximate_svd=True,
                svd_iterations=approximate_svd_iterations,
                stats=research_stats,
            )
        try:
            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter("always")
                with contextlib.redirect_stderr(stderr_buffer):
                    with dispatch_context:
                        value = probe.fn()
                    torch.mps.synchronize()
                if approximate_svd_active:
                    experimental_probe_replacements = dict(research_stats.replacements)
                caught_warnings = list(caught)
            warning_texts = [str(item.message) for item in caught]
            stderr_text = stderr_buffer.getvalue()
            for text in [*warning_texts, stderr_text]:
                fallback_ops.extend(FALLBACK_PATTERN.findall(text))
            status = "fallback" if fallback_ops else "ok"
            result = simplify_result(value)
            error = None
        except Exception as exc:  # noqa: BLE001 - this is a probe harness
            warning_texts = [str(item.message) for item in caught_warnings]
            stderr_text = stderr_buffer.getvalue()
            for text in [*warning_texts, stderr_text]:
                fallback_ops.extend(FALLBACK_PATTERN.findall(text))
            error_text = f"{type(exc).__name__}: {exc}"
            expected_errors = probe.expected_unsupported_error
            if isinstance(expected_errors, str):
                expected_errors = (expected_errors,)
            if expected_errors and any(token in error_text for token in expected_errors):
                status = "unsupported"
            else:
                status = "error"
            result = None
            error = error_text
        elapsed_ms = round((time.perf_counter() - started) * 1000, 3)
        results.append(
            {
                "name": probe.name,
                "category": probe.category,
                "status": status,
                "fallback_ops": sorted(set(fallback_ops)),
                "warnings": warning_texts,
                "stderr": stderr_text,
                "error": error,
                "result": result,
                "elapsed_ms": elapsed_ms,
                "gpu_route": probe.route,
                "psyche_experimental_probe_route_state": psyche_experimental_probe_route_state_for_probe(
                    probe.name,
                    approximate_svd_active,
                    experimental_probe_replacements,
                ),
                "psyche_experimental_probe_route_replacements": experimental_probe_replacements,
                "approximate_svd_probe_enabled": bool(enable_approximate_svd_probe),
                "approximate_svd_probe_active": approximate_svd_active,
                "approximate_svd_iterations": (
                    approximate_svd_iterations if enable_approximate_svd_probe else None
                ),
                "pytorch_enable_mps_fallback": os.environ.get(
                    "PYTORCH_ENABLE_MPS_FALLBACK",
                    "",
                ),
            }
        )
    return results


def run_isolated_runtime_probes(
    torch: Any,
    F: Any,
    *,
    install_compat: bool,
    enable_experimental_routes: bool,
    enable_approximate_svd_probe: bool,
    approximate_svd_iterations: int,
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    for probe in make_probes(torch, F):
        env = dict(os.environ)
        # Default isolated probes to fallback-enabled unless the parent
        # explicitly set PYTORCH_ENABLE_MPS_FALLBACK=0 for promotion testing.
        env.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")
        command = [
            sys.executable,
            str(Path(__file__).resolve()),
            "--single-runtime-probe",
            probe.name,
            "--no-auto-fallback-env",
        ]
        if install_compat:
            command.append("--install-psyche-compat")
        if enable_experimental_routes:
            command.append("--enable-experimental-psyche-routes")
        if enable_approximate_svd_probe:
            command.extend(
                [
                    "--enable-approximate-svd-probe",
                    "--approximate-svd-iterations",
                    str(approximate_svd_iterations),
                ]
            )
        completed = subprocess.run(
            command,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )
        if completed.returncode != 0:
            results.append(
                {
                    "name": probe.name,
                    "category": probe.category,
                    "status": "error",
                    "fallback_ops": [],
                    "warnings": [],
                    "stderr": completed.stderr,
                    "error": f"isolated probe process exited {completed.returncode}",
                    "result": None,
                    "elapsed_ms": None,
                    "gpu_route": probe.route,
                    "psyche_experimental_probe_route_state": psyche_experimental_probe_route_state_for_probe(
                        probe.name,
                        enable_approximate_svd_probe and probe.name == "linalg_svd",
                    ),
                    "psyche_experimental_probe_route_replacements": {},
                    "approximate_svd_probe_enabled": bool(enable_approximate_svd_probe),
                    "approximate_svd_probe_active": (
                        enable_approximate_svd_probe and probe.name == "linalg_svd"
                    ),
                    "approximate_svd_iterations": (
                        approximate_svd_iterations
                        if enable_approximate_svd_probe
                        else None
                    ),
                    "pytorch_enable_mps_fallback": env.get(
                        "PYTORCH_ENABLE_MPS_FALLBACK",
                        "",
                    ),
                    "isolated_process": True,
                    "stdout_tail": completed.stdout[-STDOUT_TAIL_BYTES:],
                    "stderr_tail": completed.stderr[-STDOUT_TAIL_BYTES:],
                }
            )
            continue
        try:
            row = parse_probe_json_from_stdout(completed.stdout)
        except json.JSONDecodeError as exc:
            results.append(
                {
                    "name": probe.name,
                    "category": probe.category,
                    "status": "error",
                    "fallback_ops": [],
                    "warnings": [],
                    "stderr": completed.stderr,
                    "error": f"isolated probe JSON parse failed: {exc}",
                    "result": None,
                    "elapsed_ms": None,
                    "gpu_route": probe.route,
                    "psyche_experimental_probe_route_state": psyche_experimental_probe_route_state_for_probe(
                        probe.name,
                        enable_approximate_svd_probe and probe.name == "linalg_svd",
                    ),
                    "psyche_experimental_probe_route_replacements": {},
                    "approximate_svd_probe_enabled": bool(enable_approximate_svd_probe),
                    "approximate_svd_probe_active": (
                        enable_approximate_svd_probe and probe.name == "linalg_svd"
                    ),
                    "approximate_svd_iterations": (
                        approximate_svd_iterations
                        if enable_approximate_svd_probe
                        else None
                    ),
                    "pytorch_enable_mps_fallback": env.get(
                        "PYTORCH_ENABLE_MPS_FALLBACK",
                        "",
                    ),
                    "isolated_process": True,
                    "stdout_tail": completed.stdout[-STDOUT_TAIL_BYTES:],
                    "stderr_tail": completed.stderr[-STDOUT_TAIL_BYTES:],
                }
            )
            continue
        row["isolated_process"] = True
        results.append(row)
    return results


def write_markdown(data: dict[str, Any], path: Path) -> None:
    dispatcher = data["dispatcher"]
    runtime = data.get("runtime_probes", [])
    by_status = Counter(row.get("status", "unknown") for row in runtime)
    likely_namespaces = dispatcher["likely_fallback_namespace_counts"]

    lines = [
        "# PyTorch MPS Unsupported Operator Probe",
        "",
        f"- Generated: `{data['generated_at']}`",
        f"- Python: `{data['python']}`",
        f"- Torch: `{data['torch']}`",
        f"- MPS available: `{data['mps_available']}`",
        f"- `PYTORCH_ENABLE_MPS_FALLBACK`: `{data['pytorch_enable_mps_fallback']}`",
        f"- Psyche compat installed: `{data['psyche_compat_installed']}`",
        f"- Experimental Psyche routes enabled: `{data['psyche_experimental_routes_enabled']}`",
        f"- Approximate SVD research probe enabled: `{data['psyche_approximate_svd_probe_enabled']}`",
        f"- Runtime probes isolated: `{data['runtime_probe_isolation']}`",
        "",
    ]
    if data.get("psyche_approximate_svd_probe_enabled"):
        lines.extend(
            [
                "### Approximate SVD Research Mode",
                "",
                f"- Iterations: `{data['psyche_approximate_svd_iterations']}`",
                "- Scope: diagnostic `TorchDispatchMode` only; it is not a registered `aten::linalg_svd` compatibility kernel.",
                "",
            ]
        )
    experimental_env = data.get("psyche_experimental_route_env") or {}
    if experimental_env:
        lines.extend(["### Experimental Route Env", ""])
        for name, value in sorted(experimental_env.items()):
            lines.append(f"- `{name}`: `{value}`")
        lines.append("")

    lines.extend(
        [
            "## Dispatcher Inventory",
            "",
            f"- Total registered operators: `{dispatcher['total_ops']}`",
            f"- Missing a direct MPS kernel: `{dispatcher['missing_direct_mps_count']}`",
            f"- No direct MPS or composite kernel, but CPU exists: `{dispatcher['likely_cpu_fallback_or_not_implemented_count']}`",
            "",
            "| Classification | Count |",
            "| --- | ---: |",
        ]
    )
    for key, value in sorted(dispatcher["classification_counts"].items()):
        lines.append(f"| `{key}` | {value} |")

    lines.extend(["", "## Likely CPU-Fallback Namespaces", "", "| Namespace | Count |", "| --- | ---: |"])
    for namespace, count in sorted(likely_namespaces.items(), key=lambda item: (-item[1], item[0]))[:20]:
        lines.append(f"| `{namespace}` | {count} |")

    if runtime:
        lines.extend(["", "## Runtime Probes", ""])
        if data.get("psyche_approximate_svd_probe_enabled"):
            diagnostic_successes = sum(
                1
                for row in runtime
                if row.get("psyche_experimental_probe_route_state")
                == "experimental_approximate_svd_dispatch"
            )
            lines.extend(
                [
                    "Runtime outcomes under selected probe configuration, including the experimental approximate-SVD diagnostic route:",
                    "",
                    f"- Status counts: `{dict(by_status)}`",
                    f"- Experimental diagnostic route successes: `{diagnostic_successes}`",
                    "- Registered compatibility counts are unchanged; `aten::linalg_svd` remains `not_a_psyche_route`.",
                    "",
                ]
            )
        else:
            lines.extend([f"Status counts: `{dict(by_status)}`", ""])
        lines.append("| Probe | Category | Status | Psyche route | Experimental probe route | Fallback ops / error | GPU route |")
        lines.append("| --- | --- | --- | --- | --- | --- | --- |")
        for row in runtime:
            issue = ", ".join(f"`{op}`" for op in row.get("fallback_ops", []))
            if not issue and row.get("error"):
                issue = row["error"].replace("|", "\\|")
            if not issue:
                issue = ""
            lines.append(
                "| {name} | {category} | `{status}` | {state} | {experimental_state} | {issue} | {route} |".format(
                    name=f"`{row['name']}`",
                    category=row.get("category", ""),
                    status=row.get("status", ""),
                    state=f"`{row.get('psyche_route_state', '')}`",
                    experimental_state=f"`{row.get('psyche_experimental_probe_route_state', '')}`",
                    issue=issue,
                    route=row.get("gpu_route", "").replace("|", "\\|"),
                )
            )

    lines.extend(
        [
            "",
            "## Notes",
            "",
            "- `missing_direct_mps_count` is an upper bound, not the failure count.",
            "- `composite_candidate` ops may still run entirely on MPS after decomposition.",
            "- Runtime `ok` rows mean the probe completed without a detected fallback warning; they are not semantic correctness proof.",
            "- `psyche_route_state` identifies whether Psyche registered a route for the probe; `ok` alone does not prove Psyche handled it.",
            "- `psyche_experimental_probe_route_state=experimental_approximate_svd_dispatch` is diagnostic evidence only and is not a registered ATen compatibility claim.",
            "- Runtime `fallback` rows are the highest-signal proof that PyTorch crossed to CPU for that op on this machine.",
            "- A `fallback` row is still unsupported as an honest GPU route even when the parent run used `PYTORCH_ENABLE_MPS_FALLBACK=0`.",
            "- Runtime `unsupported` rows are expected hard platform boundaries, not probe failures.",
            "- Runtime `error` rows are unexpected probe failures that need investigation.",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-probes", action="store_true", help="Run curated MPS probes in addition to dispatcher inventory.")
    parser.add_argument(
        "--install-psyche-compat",
        action="store_true",
        help="Install Psyche's opt-in MPS compatibility kernels before inventory/probes.",
    )
    parser.add_argument(
        "--enable-experimental-psyche-routes",
        action="store_true",
        help=(
            "Enable gated experimental Psyche MPS routes such as QR and matrix_exp "
            "before installing compatibility kernels."
        ),
    )
    parser.add_argument(
        "--isolated-runtime-probes",
        action="store_true",
        help="Run each runtime probe in a fresh Python process so one-shot fallback warnings are not suppressed.",
    )
    parser.add_argument(
        "--enable-approximate-svd-probe",
        action="store_true",
        help=(
            "Run the linalg_svd probe through Psyche's explicit approximate SVD "
            "TorchDispatchMode. This is diagnostic evidence only, not registered ATen support."
        ),
    )
    parser.add_argument(
        "--approximate-svd-iterations",
        type=int,
        default=DEFAULT_APPROXIMATE_SVD_ITERATIONS,
        help="Power-iteration count for --enable-approximate-svd-probe.",
    )
    parser.add_argument("--single-runtime-probe", help=argparse.SUPPRESS)
    parser.add_argument(
        "--no-auto-fallback-env",
        action="store_true",
        help="Do not re-exec with PYTORCH_ENABLE_MPS_FALLBACK=1 before runtime probes.",
    )
    parser.add_argument("--json-out", type=Path, help="Write full JSON results to this path.")
    parser.add_argument("--markdown-out", type=Path, help="Write a compact Markdown report to this path.")
    parser.add_argument(
        "--max-operator-rows",
        type=int,
        default=0,
        help="Limit operator rows in stdout JSON. Output files always receive the full list. 0 means no stdout list.",
    )
    return parser.parse_args()


def main() -> int:
    ensure_fallback_env(sys.argv)
    args = parse_args()
    if args.enable_experimental_psyche_routes:
        enable_experimental_psyche_routes()
    torch, F = import_torch()

    psyche_compat_install = install_psyche_mps_compat() if args.install_psyche_compat else None
    if args.single_runtime_probe:
        runtime = run_runtime_probes(
            torch,
            F,
            only_probe=args.single_runtime_probe,
            enable_approximate_svd_probe=bool(args.enable_approximate_svd_probe),
            approximate_svd_iterations=args.approximate_svd_iterations,
        )
        annotate_runtime_probe_routes(runtime, psyche_compat_install)
        print(PROBE_JSON_SENTINEL + json.dumps(runtime[0], sort_keys=True))
        return 0

    dispatcher = dispatcher_inventory(torch)
    if args.runtime_probes and args.isolated_runtime_probes:
        runtime = run_isolated_runtime_probes(
            torch,
            F,
            install_compat=bool(args.install_psyche_compat),
            enable_experimental_routes=bool(args.enable_experimental_psyche_routes),
            enable_approximate_svd_probe=bool(args.enable_approximate_svd_probe),
            approximate_svd_iterations=args.approximate_svd_iterations,
        )
    elif args.runtime_probes:
        runtime = run_runtime_probes(
            torch,
            F,
            enable_approximate_svd_probe=bool(args.enable_approximate_svd_probe),
            approximate_svd_iterations=args.approximate_svd_iterations,
        )
    else:
        runtime = []
    annotate_runtime_probe_routes(runtime, psyche_compat_install)
    data = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "python": platform.python_version(),
        "platform": platform.platform(),
        "torch": torch.__version__,
        "mps_built": bool(torch.backends.mps.is_built()),
        "mps_available": bool(torch.backends.mps.is_available()),
        "pytorch_enable_mps_fallback": os.environ.get("PYTORCH_ENABLE_MPS_FALLBACK", ""),
        "psyche_compat_installed": bool(args.install_psyche_compat),
        "psyche_compat_install": psyche_compat_install,
        "psyche_experimental_routes_enabled": bool(args.enable_experimental_psyche_routes),
        "psyche_experimental_route_env": {
            name: os.environ.get(name, "") for name in EXPERIMENTAL_ROUTE_ENV
        },
        "psyche_approximate_svd_probe_enabled": bool(args.enable_approximate_svd_probe),
        "psyche_approximate_svd_iterations": args.approximate_svd_iterations,
        "runtime_probe_isolation": bool(args.isolated_runtime_probes),
        "dispatcher": dispatcher,
        "runtime_probes": runtime,
    }

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    if args.markdown_out:
        args.markdown_out.parent.mkdir(parents=True, exist_ok=True)
        write_markdown(data, args.markdown_out)

    stdout_data = dict(data)
    stdout_dispatcher = dict(dispatcher)
    if args.max_operator_rows > 0:
        stdout_dispatcher["operators"] = dispatcher["operators"][: args.max_operator_rows]
    else:
        stdout_dispatcher["operators"] = []
    stdout_data["dispatcher"] = stdout_dispatcher
    print(json.dumps(stdout_data, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
