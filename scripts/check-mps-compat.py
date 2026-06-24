#!/usr/bin/env python3
"""
Smoke-check Psyche's opt-in MPS compatibility routes.

Run on Apple Silicon with a PyTorch build that has MPS:

    PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/check-mps-compat.py
    PYTORCH_ENABLE_MPS_FALLBACK=0 scripts/check-mps-compat.py --experimental-approx-svd

The first command verifies exact GPU decompositions. The second also verifies
the experimental approximate SVD route.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import subprocess
import sys
import textwrap
from pathlib import Path

import torch
import torch.nn.functional as F


def load_mps_compat():
    try:
        from psyche.mps_compat import (
            approximate_linalg_svd_mps,
            install_mps_compat_kernels,
            matrix_exp_mps,
            qr_mps,
        )

        return install_mps_compat_kernels, approximate_linalg_svd_mps, matrix_exp_mps, qr_mps
    except Exception:
        module_path = (
            Path(__file__).resolve().parents[1] / "python/python/psyche/mps_compat.py"
        )
        spec = importlib.util.spec_from_file_location("psyche_mps_compat_check", module_path)
        module = importlib.util.module_from_spec(spec)
        assert spec and spec.loader
        spec.loader.exec_module(module)
        return (
            module.install_mps_compat_kernels,
            module.approximate_linalg_svd_mps,
            module.matrix_exp_mps,
            module.qr_mps,
        )


def assert_mps_available() -> None:
    if not torch.backends.mps.is_available():
        raise SystemExit("MPS is not available in this Python/PyTorch environment")


def run_python_snippet(snippet: str) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["PYTORCH_ENABLE_MPS_FALLBACK"] = "0"
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(snippet)],
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


def check_adaptive_avg_pool3d_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch
        import torch.nn.functional as F

        x = torch.randn(1, 2, 5, 6, 7, device="mps")
        F.adaptive_avg_pool3d(x, (2, 3, 4))
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("adaptive_avg_pool3d baseline: native MPS support present; compat skip is ok")
        return
    print("adaptive_avg_pool3d baseline: fails without compat as expected")


def check_adaptive_avg_pool3d_schema() -> None:
    forward_schema = str(torch.ops.aten._adaptive_avg_pool3d.default._schema)
    backward_schema = str(torch.ops.aten._adaptive_avg_pool3d_backward.default._schema)
    expected_forward = "aten::_adaptive_avg_pool3d(Tensor self, SymInt[3] output_size) -> Tensor"
    expected_backward = "aten::_adaptive_avg_pool3d_backward(Tensor grad_output, Tensor self) -> Tensor"
    if forward_schema != expected_forward:
        raise AssertionError(f"unexpected _adaptive_avg_pool3d schema: {forward_schema}")
    if backward_schema != expected_backward:
        raise AssertionError(f"unexpected _adaptive_avg_pool3d_backward schema: {backward_schema}")
    print("adaptive_avg_pool3d schema: ok")


def check_matrix_exp_schema() -> None:
    default_schema = str(torch.ops.aten.linalg_matrix_exp.default._schema)
    out_schema = str(torch.ops.aten.linalg_matrix_exp.out._schema)
    expected_default = "aten::linalg_matrix_exp(Tensor self) -> Tensor"
    expected_out = "aten::linalg_matrix_exp.out(Tensor self, *, Tensor(a!) out) -> Tensor(a!)"
    if default_schema != expected_default:
        raise AssertionError(f"unexpected linalg_matrix_exp schema: {default_schema}")
    if out_schema != expected_out:
        raise AssertionError(f"unexpected linalg_matrix_exp.out schema: {out_schema}")
    print("matrix_exp schema: ok")


def check_qr_schema() -> None:
    default_schema = str(torch.ops.aten.linalg_qr.default._schema)
    out_schema = str(torch.ops.aten.linalg_qr.out._schema)
    expected_default = 'aten::linalg_qr(Tensor A, str mode="reduced") -> (Tensor Q, Tensor R)'
    expected_out = 'aten::linalg_qr.out(Tensor A, str mode="reduced", *, Tensor(a!) Q, Tensor(b!) R) -> (Tensor(a!) Q, Tensor(b!) R)'
    if default_schema != expected_default:
        raise AssertionError(f"unexpected linalg_qr schema: {default_schema}")
    if out_schema != expected_out:
        raise AssertionError(f"unexpected linalg_qr.out schema: {out_schema}")
    print("qr schema: ok")


def check_geqrf_schema() -> None:
    schema = str(torch.ops.aten.geqrf.default._schema)
    expected = "aten::geqrf(Tensor self) -> (Tensor a, Tensor tau)"
    if schema != expected:
        raise AssertionError(f"unexpected geqrf schema: {schema}")
    print("geqrf schema: ok")


def check_heaviside_schema() -> None:
    default_schema = str(torch.ops.aten.heaviside.default._schema)
    out_schema = str(torch.ops.aten.heaviside.out._schema)
    expected_default = "aten::heaviside(Tensor self, Tensor values) -> Tensor"
    expected_out = "aten::heaviside.out(Tensor self, Tensor values, *, Tensor(a!) out) -> Tensor(a!)"
    if default_schema != expected_default:
        raise AssertionError(f"unexpected heaviside schema: {default_schema}")
    if out_schema != expected_out:
        raise AssertionError(f"unexpected heaviside.out schema: {out_schema}")
    print("heaviside schema: ok")


def check_gcd_schema() -> None:
    default_schema = str(torch.ops.aten.gcd.default._schema)
    out_schema = str(torch.ops.aten.gcd.out._schema)
    expected_default = "aten::gcd(Tensor self, Tensor other) -> Tensor"
    expected_out = "aten::gcd.out(Tensor self, Tensor other, *, Tensor(a!) out) -> Tensor(a!)"
    if default_schema != expected_default:
        raise AssertionError(f"unexpected gcd schema: {default_schema}")
    if out_schema != expected_out:
        raise AssertionError(f"unexpected gcd.out schema: {out_schema}")
    print("gcd schema: ok")


def check_lcm_schema() -> None:
    default_schema = str(torch.ops.aten.lcm.default._schema)
    out_schema = str(torch.ops.aten.lcm.out._schema)
    expected_default = "aten::lcm(Tensor self, Tensor other) -> Tensor"
    expected_out = "aten::lcm.out(Tensor self, Tensor other, *, Tensor(a!) out) -> Tensor(a!)"
    if default_schema != expected_default:
        raise AssertionError(f"unexpected lcm schema: {default_schema}")
    if out_schema != expected_out:
        raise AssertionError(f"unexpected lcm.out schema: {out_schema}")
    print("lcm schema: ok")


def check_reduction_out_schema() -> None:
    schemas = {
        "std": (
            str(torch.ops.aten.std.correction._schema),
            str(torch.ops.aten.std.correction_out._schema),
        ),
        "var": (
            str(torch.ops.aten.var.correction._schema),
            str(torch.ops.aten.var.correction_out._schema),
        ),
    }
    expected_default = {
        "std": "aten::std.correction(Tensor self, int[1]? dim=None, *, Scalar? correction=None, bool keepdim=False) -> Tensor",
        "var": "aten::var.correction(Tensor self, int[1]? dim=None, *, Scalar? correction=None, bool keepdim=False) -> Tensor",
    }
    expected_out = {
        "std": "aten::std.correction_out(Tensor self, int[1]? dim=None, *, Scalar? correction=None, bool keepdim=False, Tensor(a!) out) -> Tensor(a!)",
        "var": "aten::var.correction_out(Tensor self, int[1]? dim=None, *, Scalar? correction=None, bool keepdim=False, Tensor(a!) out) -> Tensor(a!)",
    }
    for name, (default_schema, out_schema) in schemas.items():
        if default_schema != expected_default[name]:
            raise AssertionError(f"unexpected {name}.correction schema: {default_schema}")
        if out_schema != expected_out[name]:
            raise AssertionError(f"unexpected {name}.correction_out schema: {out_schema}")
    print("std/var correction_out schema: ok")


def check_take_schema() -> None:
    default_schema = str(torch.ops.aten.take.default._schema)
    out_schema = str(torch.ops.aten.take.out._schema)
    expected_default = "aten::take(Tensor self, Tensor index) -> Tensor"
    expected_out = "aten::take.out(Tensor self, Tensor index, *, Tensor(a!) out) -> Tensor(a!)"
    if default_schema != expected_default:
        raise AssertionError(f"unexpected take schema: {default_schema}")
    if out_schema != expected_out:
        raise AssertionError(f"unexpected take.out schema: {out_schema}")
    print("take schema: ok")


def check_logit_inplace_schema() -> None:
    schema = str(torch.ops.aten.logit_.default._schema)
    expected = "aten::logit_(Tensor(a!) self, float? eps=None) -> Tensor(a!)"
    if schema != expected:
        raise AssertionError(f"unexpected logit_ schema: {schema}")
    print("logit_ schema: ok")


def check_addmm_activation_schema() -> None:
    default_schema = str(torch.ops.aten._addmm_activation.default._schema)
    out_schema = str(torch.ops.aten._addmm_activation.out._schema)
    expected_default = (
        "aten::_addmm_activation(Tensor self, Tensor mat1, Tensor mat2, *, "
        "Scalar beta=1, Scalar alpha=1, bool use_gelu=False) -> Tensor"
    )
    expected_out = (
        "aten::_addmm_activation.out(Tensor self, Tensor mat1, Tensor mat2, *, "
        "Scalar beta=1, Scalar alpha=1, bool use_gelu=False, "
        "Tensor(a!) out) -> Tensor(a!)"
    )
    if default_schema != expected_default:
        raise AssertionError(f"unexpected _addmm_activation schema: {default_schema}")
    if out_schema != expected_out:
        raise AssertionError(f"unexpected _addmm_activation.out schema: {out_schema}")
    print("_addmm_activation schema: ok")


def check_channel_shuffle_schema() -> None:
    schema = str(torch.ops.aten.channel_shuffle.default._schema)
    expected = "aten::channel_shuffle(Tensor self, SymInt groups) -> Tensor"
    if schema != expected:
        raise AssertionError(f"unexpected channel_shuffle schema: {schema}")
    print("channel_shuffle schema: ok")


def check_logspace_mvlgamma_vdot_schema() -> None:
    expected = {
        "logspace": (
            str(torch.ops.aten.logspace.default._schema),
            "aten::logspace(Scalar start, Scalar end, int steps, float base=10., "
            "*, ScalarType? dtype=None, Layout? layout=None, Device? device=None, "
            "bool? pin_memory=None) -> Tensor",
        ),
        "logspace.out": (
            str(torch.ops.aten.logspace.out._schema),
            "aten::logspace.out(Scalar start, Scalar end, int steps, float base=10., "
            "*, Tensor(a!) out) -> Tensor(a!)",
        ),
        "mvlgamma.out": (
            str(torch.ops.aten.mvlgamma.out._schema),
            "aten::mvlgamma.out(Tensor self, int p, *, Tensor(a!) out) -> Tensor(a!)",
        ),
        "vdot": (
            str(torch.ops.aten.vdot.default._schema),
            "aten::vdot(Tensor self, Tensor other) -> Tensor",
        ),
        "vdot.out": (
            str(torch.ops.aten.vdot.out._schema),
            "aten::vdot.out(Tensor self, Tensor other, *, Tensor(a!) out) -> Tensor(a!)",
        ),
        "frexp": (
            str(torch.ops.aten.frexp.Tensor._schema),
            "aten::frexp.Tensor(Tensor self) -> (Tensor mantissa, Tensor exponent)",
        ),
        "frexp.out": (
            str(torch.ops.aten.frexp.Tensor_out._schema),
            "aten::frexp.Tensor_out(Tensor self, *, Tensor(a!) mantissa, Tensor(b!) exponent) "
            "-> (Tensor(a!) mantissa, Tensor(b!) exponent)",
        ),
    }
    for name, (schema, expected_schema) in expected.items():
        if schema != expected_schema:
            raise AssertionError(f"unexpected {name} schema: {schema}")
    print("logspace/mvlgamma/vdot/frexp schema: ok")


def check_adaptive_avg_pool3d(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    cases = [
        ((1, 2, 5, 6, 7), (2, 3, 4)),
        ((2, 3, 4, 4, 4), (1, 1, 1)),
        ((1, 1, 2, 3, 4), (4, 5, 6)),
        ((3, 5, 6, 7), (2, 3, 4)),
        ((1, 2, 3, 5, 7), (2, 4, 6)),
        ((1, 2, 8, 3, 5), (3, 2, 4)),
        ((1, 2, 1, 1, 1), (3, 4, 5)),
        ((1, 2, 5, 6, 7), (None, 3, 4)),
        ((1, 2, 5, 6, 7), (2, None, 4)),
    ]
    dtypes = [torch.float32, torch.float16]

    for shape, output_size in cases:
        for dtype in dtypes:
            source = torch.randn(*shape, dtype=dtype)
            expected = F.adaptive_avg_pool3d(source, output_size)
            got = F.adaptive_avg_pool3d(source.to("mps"), output_size)
            torch.mps.synchronize()
            max_diff = (got.cpu().float() - expected.float()).abs().max().item()
            tolerance = 1e-5 if dtype == torch.float32 else 5e-3
            if got.device.type != "mps":
                raise AssertionError(f"adaptive_avg_pool3d returned {got.device}, expected mps")
            if max_diff > tolerance:
                raise AssertionError(
                    f"adaptive_avg_pool3d max diff too high for {shape}->{output_size} "
                    f"{dtype}: {max_diff}"
                )
    print(
        "adaptive_avg_pool3d forward: "
        f"ok installed={install_result.installed} skipped={install_result.skipped_existing_mps}"
    )


def check_adaptive_avg_pool3d_backward(install_mps_compat_kernels) -> None:
    install_mps_compat_kernels()
    cases = [
        ((2, 3, 5, 6, 7), (2, 3, 4)),
        ((1, 2, 3, 5, 7), (2, 4, 6)),
        ((3, 5, 6, 7), (2, 3, 4)),
    ]
    max_seen_out = 0.0
    max_seen_grad = 0.0
    for dtype, tolerance in [(torch.float32, 1e-5), (torch.float16, 5e-3)]:
        for seed, (shape, output_size) in enumerate(cases):
            torch.manual_seed(seed)
            source_cpu = torch.randn(*shape, dtype=dtype, requires_grad=True)
            source_mps = source_cpu.detach().clone().to("mps").requires_grad_(True)
            expected = F.adaptive_avg_pool3d(source_cpu, output_size)
            got = F.adaptive_avg_pool3d(source_mps, output_size)
            if not got.requires_grad or got.grad_fn is None:
                raise AssertionError("adaptive_avg_pool3d did not create an autograd graph")
            grad_cpu = torch.randn_like(expected)
            expected.backward(grad_cpu)
            got.backward(grad_cpu.to("mps"))
            torch.mps.synchronize()
            if source_mps.grad is None or source_mps.grad.device.type != "mps":
                raise AssertionError("adaptive_avg_pool3d backward did not produce MPS grad")
            max_out_diff = (got.detach().cpu().float() - expected.detach().float()).abs().max().item()
            max_grad_diff = (
                source_mps.grad.cpu().float() - source_cpu.grad.float()
            ).abs().max().item()
            max_seen_out = max(max_seen_out, max_out_diff)
            max_seen_grad = max(max_seen_grad, max_grad_diff)
            if max_out_diff > tolerance:
                raise AssertionError(
                    f"adaptive_avg_pool3d backward output diff too high for {shape}->{output_size} "
                    f"{dtype}: {max_out_diff}"
                )
            if max_grad_diff > tolerance:
                raise AssertionError(
                    f"adaptive_avg_pool3d backward grad diff too high for {shape}->{output_size} "
                    f"{dtype}: {max_grad_diff}"
                )
    print(f"adaptive_avg_pool3d backward: ok out_diff={max_seen_out:.3g} grad_diff={max_seen_grad:.3g}")


def check_adaptive_avg_pool3d_backward_direct(install_mps_compat_kernels) -> None:
    install_mps_compat_kernels()
    grad_cpu = torch.randn(2, 3, 2, 3, 4)
    input_cpu = torch.randn(2, 3, 5, 6, 7)
    expected = torch.ops.aten._adaptive_avg_pool3d_backward.default(grad_cpu, input_cpu)
    got = torch.ops.aten._adaptive_avg_pool3d_backward.default(
        grad_cpu.to("mps"),
        input_cpu.to("mps"),
    )
    torch.mps.synchronize()
    max_diff = (got.cpu() - expected).abs().max().item()
    if got.device.type != "mps":
        raise AssertionError(f"_adaptive_avg_pool3d_backward returned {got.device}, expected mps")
    if max_diff > 1e-5:
        raise AssertionError(f"_adaptive_avg_pool3d_backward direct diff too high: {max_diff}")
    print(f"adaptive_avg_pool3d backward direct: ok max_diff={max_diff:.3g}")


def check_heaviside_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        x = torch.tensor([-1.0, 0.0, 1.0], device="mps")
        values = torch.tensor([0.5], device="mps")
        torch.heaviside(x, values)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("heaviside baseline: native MPS support present; compat skip is ok")
        return
    print("heaviside baseline: fails without compat as expected")


def check_gcd_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        left = torch.tensor([-6, 0, 12], dtype=torch.int64, device="mps")
        right = torch.tensor([4, 0, -18], dtype=torch.int64, device="mps")
        torch.gcd(left, right)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("gcd baseline: native MPS support present; compat skip is ok")
        return
    print("gcd baseline: fails without compat as expected")


def check_lcm_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        left = torch.tensor([-6, 0, 12], dtype=torch.int64, device="mps")
        right = torch.tensor([4, 0, -18], dtype=torch.int64, device="mps")
        torch.lcm(left, right)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("lcm baseline: native MPS support present; compat skip is ok")
        return
    print("lcm baseline: fails without compat as expected")


def check_reduction_out_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        x = torch.randn(3, 4, device="mps")
        out = torch.empty(0, device="mps")
        torch.ops.aten.std.correction_out(x, [1], correction=1, keepdim=False, out=out)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("std.correction_out baseline: native MPS support present; compat skip is ok")
    else:
        print("std.correction_out baseline: fails without compat as expected")

    result = run_python_snippet(
        """
        import torch

        x = torch.randn(3, 4, device="mps")
        out = torch.empty(0, device="mps")
        torch.ops.aten.var.correction_out(x, [1], correction=1, keepdim=False, out=out)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("var.correction_out baseline: native MPS support present; compat skip is ok")
    else:
        print("var.correction_out baseline: fails without compat as expected")


def check_take_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        x = torch.arange(8, device="mps")
        index = torch.tensor([0, 2, -1], device="mps")
        torch.take(x, index)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("take baseline: native MPS support present; compat skip is ok")
    else:
        print("take baseline: fails without compat as expected")

    result = run_python_snippet(
        """
        import torch

        x = torch.arange(8, device="mps")
        index = torch.tensor([0, 2, -1], device="mps")
        out = torch.empty(0, dtype=x.dtype, device="mps")
        torch.take(x, index, out=out)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("take.out baseline: native MPS support present; compat skip is ok")
    else:
        print("take.out baseline: fails without compat as expected")


def check_logit_inplace_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        x = torch.tensor([0.2, 0.5, 0.8], device="mps")
        x.logit_()
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("logit_ baseline: native MPS support present; compat skip is ok")
    else:
        print("logit_ baseline: fails without compat as expected")


def check_addmm_activation_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        input_tensor = torch.randn(3, 5, device="mps")
        mat1 = torch.randn(3, 4, device="mps")
        mat2 = torch.randn(4, 5, device="mps")
        torch.ops.aten._addmm_activation.default(
            input_tensor,
            mat1,
            mat2,
            beta=1,
            alpha=1,
            use_gelu=True,
        )
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("_addmm_activation baseline: native MPS support present; compat skip is ok")
    else:
        print("_addmm_activation baseline: fails without compat as expected")

    result = run_python_snippet(
        """
        import torch

        input_tensor = torch.randn(3, 5, device="mps")
        mat1 = torch.randn(3, 4, device="mps")
        mat2 = torch.randn(4, 5, device="mps")
        out = torch.empty(0, device="mps")
        torch.ops.aten._addmm_activation.out(
            input_tensor,
            mat1,
            mat2,
            beta=1,
            alpha=1,
            use_gelu=False,
            out=out,
        )
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("_addmm_activation.out baseline: native MPS support present; compat skip is ok")
    else:
        print("_addmm_activation.out baseline: fails without compat as expected")


def check_channel_shuffle_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        input_tensor = torch.arange(2 * 4 * 3, dtype=torch.float32, device="mps").reshape(2, 4, 3)
        torch.channel_shuffle(input_tensor, 2)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("channel_shuffle baseline: native MPS support present; compat skip is ok")
    else:
        print("channel_shuffle baseline: fails without compat as expected")


def check_logspace_mvlgamma_vdot_baseline_fails() -> None:
    probes = [
        (
            "logspace",
            """
            import torch
            torch.logspace(0, 3, 4, device="mps")
            torch.mps.synchronize()
            """,
        ),
        (
            "logspace.out",
            """
            import torch
            out = torch.empty(0, device="mps")
            torch.logspace(0, 3, 4, out=out)
            torch.mps.synchronize()
            """,
        ),
        (
            "mvlgamma.out",
            """
            import torch
            x = torch.tensor([3.0, 4.0], device="mps")
            out = torch.empty(0, device="mps")
            torch.mvlgamma(x, 2, out=out)
            torch.mps.synchronize()
            """,
        ),
        (
            "vdot",
            """
            import torch
            left = torch.arange(4, dtype=torch.float32, device="mps")
            right = torch.arange(4, dtype=torch.float32, device="mps")
            torch.vdot(left, right)
            torch.mps.synchronize()
            """,
        ),
        (
            "vdot.out",
            """
            import torch
            left = torch.arange(4, dtype=torch.float32, device="mps")
            right = torch.arange(4, dtype=torch.float32, device="mps")
            out = torch.empty((), device="mps")
            torch.vdot(left, right, out=out)
            torch.mps.synchronize()
            """,
        ),
        (
            "frexp",
            """
            import torch
            x = torch.tensor([1.0, 2.0, 3.0], device="mps")
            torch.frexp(x)
            torch.mps.synchronize()
            """,
        ),
        (
            "frexp.out",
            """
            import torch
            x = torch.tensor([1.0, 2.0, 3.0], device="mps")
            mantissa = torch.empty(0, device="mps")
            exponent = torch.empty(0, dtype=torch.int32, device="mps")
            torch.frexp(x, out=(mantissa, exponent))
            torch.mps.synchronize()
            """,
        ),
    ]
    for name, snippet in probes:
        result = run_python_snippet(snippet)
        if result.returncode == 0:
            print(f"{name} baseline: native MPS support present; compat skip is ok")
        else:
            print(f"{name} baseline: fails without compat as expected")


def check_geqrf_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        x = torch.randn(4, 3, device="mps")
        torch.geqrf(x)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("geqrf baseline: native MPS support present; compat skip is ok")
    else:
        print("geqrf baseline: fails without compat as expected")


def check_heaviside(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {"aten::heaviside.default", "aten::heaviside.out"}
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"heaviside routes were not installed or skipped cleanly: {missing}")

    cases = [
        (
            torch.tensor([-float("inf"), -2.0, -0.0, 0.0, 3.0, float("inf"), float("nan")]),
            torch.tensor([0.25]),
        ),
        (
            torch.tensor([[-1.0, 0.0, 1.0], [2.0, -3.0, 0.0]]),
            torch.tensor([[0.5, -0.5, 0.75]]),
        ),
    ]
    for dtype in (torch.float32, torch.float16):
        for input_cpu, values_cpu in cases:
            input_cpu = input_cpu.to(dtype)
            values_cpu = values_cpu.to(dtype)
            expected = torch.heaviside(input_cpu, values_cpu)
            got = torch.heaviside(input_cpu.to("mps"), values_cpu.to("mps"))
            torch.mps.synchronize()
            tolerance = 1e-5 if dtype == torch.float32 else 1e-3
            if got.device.type != "mps":
                raise AssertionError(f"heaviside returned {got.device}, expected mps")
            got_cpu = got.cpu()
            if not torch.equal(torch.isnan(got_cpu), torch.isnan(expected)):
                raise AssertionError(f"heaviside nan mask mismatch for {dtype}: {got_cpu} != {expected}")
            non_nan = ~torch.isnan(expected)
            if (got_cpu[non_nan].float() - expected[non_nan].float()).abs().max().item() > tolerance:
                raise AssertionError(f"heaviside mismatch for {dtype}: {got.cpu()} != {expected}")

    integer_input = torch.tensor([-2, 0, 3], dtype=torch.int64)
    integer_values = torch.tensor([7], dtype=torch.int64)
    integer_expected = torch.heaviside(integer_input, integer_values)
    integer_got = torch.heaviside(integer_input.to("mps"), integer_values.to("mps"))
    bool_input = torch.tensor([False, True, False], dtype=torch.bool)
    bool_values = torch.tensor([False, True, True], dtype=torch.bool)
    bool_expected = torch.heaviside(bool_input, bool_values)
    bool_got = torch.heaviside(bool_input.to("mps"), bool_values.to("mps"))
    torch.mps.synchronize()
    if not torch.equal(integer_got.cpu(), integer_expected):
        raise AssertionError(f"integer heaviside mismatch: {integer_got.cpu()} != {integer_expected}")
    if not torch.equal(bool_got.cpu(), bool_expected):
        raise AssertionError(f"bool heaviside mismatch: {bool_got.cpu()} != {bool_expected}")

    direct_input = torch.tensor([-1.0, 0.0, 1.0])
    direct_values = torch.tensor([0.5])
    direct_expected = torch.ops.aten.heaviside.default(direct_input, direct_values)
    direct_got = torch.ops.aten.heaviside.default(
        direct_input.to("mps"),
        direct_values.to("mps"),
    )
    direct_out = torch.empty(1, device="mps")
    direct_returned = torch.ops.aten.heaviside.out(
        direct_input.to("mps"),
        direct_values.to("mps"),
        out=direct_out,
    )
    torch.mps.synchronize()
    if direct_returned.data_ptr() != direct_out.data_ptr():
        raise AssertionError("heaviside.out did not return the provided output tensor")
    if direct_out.shape != direct_expected.shape:
        raise AssertionError(f"heaviside.out did not resize output: {direct_out.shape}")
    if not torch.allclose(direct_got.cpu(), direct_expected):
        raise AssertionError("direct aten heaviside mismatch")
    if not torch.allclose(direct_returned.cpu(), direct_expected):
        raise AssertionError("direct aten heaviside.out mismatch")

    exact_out = torch.empty_like(direct_input, device="mps")
    exact_ptr = exact_out.data_ptr()
    torch.heaviside(direct_input.to("mps"), direct_values.to("mps"), out=exact_out)
    torch.mps.synchronize()
    if exact_out.data_ptr() != exact_ptr:
        raise AssertionError("heaviside.out unexpectedly changed storage for exact-shape out")

    noncontiguous_out_base = torch.empty(3, 2, device="mps")
    noncontiguous_out = noncontiguous_out_base[:, 0]
    if noncontiguous_out.is_contiguous():
        raise AssertionError("heaviside non-contiguous out fixture is contiguous")
    torch.heaviside(direct_input.to("mps"), direct_values.to("mps"), out=noncontiguous_out)
    torch.mps.synchronize()
    if not torch.allclose(noncontiguous_out.cpu(), direct_expected):
        raise AssertionError("heaviside.out non-contiguous output mismatch")

    alias_input = torch.tensor([-1.0, 0.0, 1.0], device="mps")
    alias_expected = torch.heaviside(alias_input.cpu(), direct_values)
    torch.heaviside(alias_input, direct_values.to("mps"), out=alias_input)
    torch.mps.synchronize()
    if not torch.allclose(alias_input.cpu(), alias_expected):
        raise AssertionError("heaviside.out alias-input mismatch")

    alias_values_input = torch.tensor([-1.0, 0.0, 1.0], device="mps")
    alias_values = torch.tensor([0.25, 0.5, 0.75], device="mps")
    alias_values_expected = torch.heaviside(alias_values_input.cpu(), alias_values.cpu())
    torch.heaviside(alias_values_input, alias_values, out=alias_values)
    torch.mps.synchronize()
    if not torch.allclose(alias_values.cpu(), alias_values_expected):
        raise AssertionError("heaviside.out alias-values mismatch")

    try:
        torch.heaviside(
            torch.tensor([-1, 0, 1], dtype=torch.int64, device="mps"),
            torch.tensor([7, 8, 9], dtype=torch.int64, device="mps"),
            out=torch.empty(3, dtype=torch.float32, device="mps"),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("heaviside.out unexpectedly accepted a different dtype")

    try:
        torch.heaviside(
            torch.tensor([-1, 0, 1], dtype=torch.int64, device="mps"),
            torch.tensor([7, 8, 9], dtype=torch.float32, device="mps"),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("heaviside unexpectedly accepted mixed dtypes")

    try:
        torch.heaviside(
            direct_input.to("mps"),
            direct_values.to("mps"),
            out=torch.empty_like(direct_input),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("heaviside.out unexpectedly accepted CPU output")

    for input_requires_grad, values_requires_grad in [(True, False), (False, True), (True, True)]:
        grad_input = torch.tensor(
            [-1.0, 0.0, 1.0],
            device="mps",
            requires_grad=input_requires_grad,
        )
        grad_values = torch.tensor(
            [0.5, 0.5, 0.5],
            device="mps",
            requires_grad=values_requires_grad,
        )
        grad_output = torch.heaviside(grad_input, grad_values)
        if not grad_output.requires_grad:
            raise AssertionError("heaviside requires_grad forward did not preserve autograd state")
        try:
            grad_output.sum().backward()
        except RuntimeError as exc:
            if "derivative for aten::heaviside is not implemented" not in str(exc):
                raise
        else:
            raise AssertionError("heaviside backward unexpectedly succeeded")

    second_install = install_mps_compat_kernels()
    if "aten::heaviside.default" not in second_install.already_registered:
        raise AssertionError(f"heaviside install was not idempotent: {second_install}")

    print(f"heaviside: ok installed={install_result.installed}")


def check_gcd(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {"aten::gcd.default", "aten::gcd.out"}
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"gcd routes were not installed or skipped cleanly: {missing}")

    cases = [
        (
            torch.tensor([-6, 0, 12], dtype=torch.int64),
            torch.tensor([4, 0, -18], dtype=torch.int64),
        ),
        (
            torch.tensor([[-24, 0, 35], [81, -128, -127]], dtype=torch.int8),
            torch.tensor([[18, 0, 14]], dtype=torch.int8),
        ),
        (
            torch.tensor([-32768, -32767, 0, 1024], dtype=torch.int16),
            torch.tensor([-32767, -2, 0, 768], dtype=torch.int16),
        ),
        (
            torch.tensor([-2147483648, -2147483647, 0, 48], dtype=torch.int32),
            torch.tensor([-2147483647, -2, 0, -18], dtype=torch.int32),
        ),
        (
            torch.tensor([2147483647, -2147483648, 0], dtype=torch.int32),
            torch.tensor([-2147483648, 0, -2147483648], dtype=torch.int32),
        ),
        (
            torch.tensor([-9223372036854775808, -9223372036854775807, 0, 48], dtype=torch.int64),
            torch.tensor([-9223372036854775807, -2, 0, -18], dtype=torch.int64),
        ),
        (
            torch.tensor([9223372036854775807, -9223372036854775808, 0], dtype=torch.int64),
            torch.tensor([-9223372036854775808, 0, -9223372036854775808], dtype=torch.int64),
        ),
        (
            torch.tensor([7540113804746346429], dtype=torch.int64),
            torch.tensor([4660046610375530309], dtype=torch.int64),
        ),
        (
            torch.tensor([255, 0, 12], dtype=torch.uint8),
            torch.tensor([10, 0, 18], dtype=torch.uint8),
        ),
        (
            torch.tensor([True, False, True], dtype=torch.bool),
            torch.tensor([3, 0, 4], dtype=torch.int64),
        ),
        (
            torch.tensor([255, 0, 12], dtype=torch.uint8),
            torch.tensor([-10, 0, 18], dtype=torch.int16),
        ),
    ]

    for left_cpu, right_cpu in cases:
        expected = torch.gcd(left_cpu, right_cpu)
        got = torch.gcd(left_cpu.to("mps"), right_cpu.to("mps"))
        torch.mps.synchronize()
        if got.device.type != "mps":
            raise AssertionError(f"gcd returned {got.device}, expected mps")
        if got.dtype != expected.dtype:
            raise AssertionError(f"gcd dtype mismatch: {got.dtype} != {expected.dtype}")
        if not torch.equal(got.cpu(), expected):
            raise AssertionError(
                f"gcd mismatch for {left_cpu.dtype}/{right_cpu.dtype}: {got.cpu()} != {expected}"
            )

    dtype_values = {
        torch.bool: [False, True, True],
        torch.uint8: [0, 128, 255],
        torch.int8: [-128, -5, 127],
        torch.int16: [-32768, -5, 32767],
        torch.int32: [-2147483648, -5, 2147483647],
        torch.int64: [-9223372036854775808, -5, 9223372036854775807],
    }
    for left_dtype, left_values in dtype_values.items():
        for right_dtype, right_values in dtype_values.items():
            left_cpu = torch.tensor(left_values, dtype=left_dtype)[:, None]
            right_cpu = torch.tensor(right_values, dtype=right_dtype)[None, :]
            if left_dtype == torch.bool and right_dtype == torch.bool:
                try:
                    torch.gcd(left_cpu.to("mps"), right_cpu.to("mps"))
                except NotImplementedError:
                    continue
                raise AssertionError("gcd(bool, bool) unexpectedly succeeded in dtype grid")
            expected = torch.gcd(left_cpu, right_cpu)
            got = torch.gcd(left_cpu.to("mps"), right_cpu.to("mps"))
            torch.mps.synchronize()
            if got.dtype != expected.dtype or not torch.equal(got.cpu(), expected):
                raise AssertionError(
                    f"gcd dtype grid mismatch for {left_dtype}/{right_dtype}: "
                    f"{got.cpu()} ({got.dtype}) != {expected} ({expected.dtype})"
                )

    for dense_dtype, dense_values in [
        (torch.int8, [-128, -127, -6, -1, 0, 1, 6, 126, 127]),
        (torch.int16, [-32768, -32767, -255, -1, 0, 1, 255, 32766, 32767]),
    ]:
        left_cpu = torch.tensor(dense_values, dtype=dense_dtype)[:, None]
        right_cpu = torch.tensor(dense_values, dtype=dense_dtype)[None, :]
        expected = torch.gcd(left_cpu, right_cpu)
        got = torch.gcd(left_cpu.to("mps"), right_cpu.to("mps"))
        torch.mps.synchronize()
        if not torch.equal(got.cpu(), expected):
            raise AssertionError(f"gcd dense signed-small mismatch for {dense_dtype}")

    empty_left = torch.empty(0, 1, dtype=torch.int32)
    empty_right = torch.empty(3, dtype=torch.int32)
    empty_got = torch.gcd(empty_left.to("mps"), empty_right.to("mps"))
    torch.mps.synchronize()
    if empty_got.device.type != "mps" or empty_got.shape != (0, 3):
        raise AssertionError(f"gcd empty broadcast mismatch: {empty_got.device} {empty_got.shape}")

    try:
        torch.gcd(
            torch.tensor([1.0], device="mps"),
            torch.tensor([1.0], device="mps"),
        )
    except NotImplementedError:
        pass
    else:
        raise AssertionError("gcd unexpectedly accepted floating tensors")

    try:
        torch.gcd(
            torch.tensor([True], device="mps"),
            torch.tensor([False], device="mps"),
        )
    except NotImplementedError:
        pass
    else:
        raise AssertionError("gcd(bool, bool) unexpectedly succeeded")

    direct_left = torch.tensor([-6, 0, 12], dtype=torch.int32)
    direct_right = torch.tensor([4, 0, -18], dtype=torch.int32)
    direct_expected = torch.ops.aten.gcd.default(direct_left, direct_right)
    direct_got = torch.ops.aten.gcd.default(direct_left.to("mps"), direct_right.to("mps"))
    direct_out = torch.empty(0, dtype=torch.int32, device="mps")
    direct_returned = torch.ops.aten.gcd.out(
        direct_left.to("mps"),
        direct_right.to("mps"),
        out=direct_out,
    )
    torch.mps.synchronize()
    if direct_returned.data_ptr() != direct_out.data_ptr():
        raise AssertionError("gcd.out did not return the provided output tensor")
    if direct_out.shape != direct_expected.shape:
        raise AssertionError(f"gcd.out did not resize output: {direct_out.shape}")
    if not torch.equal(direct_got.cpu(), direct_expected):
        raise AssertionError("direct aten gcd mismatch")
    if not torch.equal(direct_returned.cpu(), direct_expected):
        raise AssertionError("direct aten gcd.out mismatch")

    exact_out = torch.empty_like(direct_left, device="mps")
    exact_ptr = exact_out.data_ptr()
    torch.gcd(direct_left.to("mps"), direct_right.to("mps"), out=exact_out)
    torch.mps.synchronize()
    if exact_out.data_ptr() != exact_ptr:
        raise AssertionError("gcd.out unexpectedly changed storage for exact-shape out")

    noncontiguous_out_base = torch.empty(3, 2, dtype=torch.int32, device="mps")
    noncontiguous_out = noncontiguous_out_base[:, 0]
    if noncontiguous_out.is_contiguous():
        raise AssertionError("gcd non-contiguous out fixture is contiguous")
    torch.gcd(direct_left.to("mps"), direct_right.to("mps"), out=noncontiguous_out)
    torch.mps.synchronize()
    if not torch.equal(noncontiguous_out.cpu(), direct_expected):
        raise AssertionError("gcd.out non-contiguous output mismatch")

    try:
        torch.gcd(
            direct_left.to("mps"),
            direct_right.to("mps"),
            out=torch.empty(3, dtype=torch.bool, device="mps"),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("gcd.out unexpectedly accepted bool output")

    alias_input = torch.tensor([48, 0, -18], dtype=torch.int64, device="mps")
    alias_other = torch.tensor([18, 0, 12], dtype=torch.int64, device="mps")
    alias_expected = torch.gcd(alias_input.cpu(), alias_other.cpu())
    torch.gcd(alias_input, alias_other, out=alias_input)
    torch.mps.synchronize()
    if not torch.equal(alias_input.cpu(), alias_expected):
        raise AssertionError("gcd.out alias-input mismatch")

    alias_other_input = torch.tensor([48, 0, -18], dtype=torch.int64, device="mps")
    alias_other = torch.tensor([18, 0, 12], dtype=torch.int64, device="mps")
    alias_other_expected = torch.gcd(alias_other_input.cpu(), alias_other.cpu())
    torch.gcd(alias_other_input, alias_other, out=alias_other)
    torch.mps.synchronize()
    if not torch.equal(alias_other.cpu(), alias_other_expected):
        raise AssertionError("gcd.out alias-other mismatch")

    overlap_base = torch.arange(8, dtype=torch.int32, device="mps")
    try:
        torch.gcd(
            overlap_base[:-1],
            torch.ones(7, dtype=torch.int32, device="mps"),
            out=overlap_base[1:],
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("gcd.out unexpectedly accepted partial overlap")

    try:
        torch.gcd(
            direct_left.to("mps"),
            direct_right.to("mps"),
            out=torch.empty(1, dtype=torch.int32, device="mps").expand(4),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("gcd.out unexpectedly accepted expanded output")

    try:
        torch.gcd(
            direct_left.to("mps"),
            direct_right.to("mps"),
            out=torch.empty_like(direct_left),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("gcd.out unexpectedly accepted CPU output")

    second_install = install_mps_compat_kernels()
    if "aten::gcd.default" not in second_install.already_registered:
        raise AssertionError(f"gcd install was not idempotent: {second_install}")

    print(f"gcd: ok installed={install_result.installed}")


def check_lcm(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {"aten::lcm.default", "aten::lcm.out"}
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"lcm routes were not installed or skipped cleanly: {missing}")

    cases = [
        (
            torch.tensor([-6, 0, 12], dtype=torch.int64),
            torch.tensor([4, 0, -18], dtype=torch.int64),
        ),
        (
            torch.tensor([[-24, 0, 35], [81, -128, -127]], dtype=torch.int8),
            torch.tensor([[18, 0, 14]], dtype=torch.int8),
        ),
        (
            torch.tensor([-32768, -32767, 0, 1024], dtype=torch.int16),
            torch.tensor([-32767, -2, 0, 768], dtype=torch.int16),
        ),
        (
            torch.tensor([-2147483648, -2147483647, 0, 46341], dtype=torch.int32),
            torch.tensor([-2147483647, -2, 0, 46341], dtype=torch.int32),
        ),
        (
            torch.tensor([-9223372036854775808, -9223372036854775807, 0, 3037000500], dtype=torch.int64),
            torch.tensor([-9223372036854775807, -2, 0, 3037000500], dtype=torch.int64),
        ),
        (
            torch.tensor([255, 0, 12], dtype=torch.uint8),
            torch.tensor([10, 0, 18], dtype=torch.uint8),
        ),
        (
            torch.tensor([True, False, True], dtype=torch.bool),
            torch.tensor([3, 0, 4], dtype=torch.int64),
        ),
        (
            torch.tensor([255, 0, 12], dtype=torch.uint8),
            torch.tensor([-10, 0, 18], dtype=torch.int16),
        ),
    ]

    for left_cpu, right_cpu in cases:
        expected = torch.lcm(left_cpu, right_cpu)
        got = torch.lcm(left_cpu.to("mps"), right_cpu.to("mps"))
        torch.mps.synchronize()
        if got.device.type != "mps":
            raise AssertionError(f"lcm returned {got.device}, expected mps")
        if got.dtype != expected.dtype:
            raise AssertionError(f"lcm dtype mismatch: {got.dtype} != {expected.dtype}")
        if not torch.equal(got.cpu(), expected):
            raise AssertionError(
                f"lcm mismatch for {left_cpu.dtype}/{right_cpu.dtype}: {got.cpu()} != {expected}"
            )

    dtype_values = {
        torch.bool: [False, True, True],
        torch.uint8: [0, 1, 2, 3, 127, 128, 254, 255],
        torch.int8: [-128, -127, -126, -7, -5, -2, -1, 0, 1, 2, 3, 5, 7, 126, 127],
        torch.int16: [-32768, -32767, -32766, -255, -128, -7, -5, -2, -1, 0, 1, 2, 3, 5, 7, 128, 255, 32766, 32767],
        torch.int32: [-2147483648, -2147483647, -2147483646, -65536, -46341, -1, 0, 1, 2, 46341, 65536, 2147483646, 2147483647],
        torch.int64: [-9223372036854775808, -9223372036854775807, -9223372036854775806, -4294967296, -3037000500, -1, 0, 1, 2, 3037000500, 4294967296, 9223372036854775806, 9223372036854775807],
    }
    for left_dtype, left_values in dtype_values.items():
        for right_dtype, right_values in dtype_values.items():
            left_cpu = torch.tensor(left_values, dtype=left_dtype)[:, None]
            right_cpu = torch.tensor(right_values, dtype=right_dtype)[None, :]
            if left_dtype == torch.bool and right_dtype == torch.bool:
                try:
                    torch.lcm(left_cpu.to("mps"), right_cpu.to("mps"))
                except NotImplementedError:
                    continue
                raise AssertionError("lcm(bool, bool) unexpectedly succeeded in dtype grid")
            expected = torch.lcm(left_cpu, right_cpu)
            got = torch.lcm(left_cpu.to("mps"), right_cpu.to("mps"))
            torch.mps.synchronize()
            if got.dtype != expected.dtype or not torch.equal(got.cpu(), expected):
                raise AssertionError(
                    f"lcm dtype grid mismatch for {left_dtype}/{right_dtype}: "
                    f"{got.cpu()} ({got.dtype}) != {expected} ({expected.dtype})"
                )

    for dense_dtype, dense_values in [
        (torch.int8, [-128, -127, -64, -3, -2, -1, 0, 1, 2, 3, 64, 100, 126, 127]),
        (torch.int16, [-32768, -32767, -129, -128, -127, -64, -3, -2, -1, 0, 1, 2, 3, 64, 127, 128, 129, 32766, 32767]),
    ]:
        left_cpu = torch.tensor(dense_values, dtype=dense_dtype)[:, None]
        right_cpu = torch.tensor(dense_values, dtype=dense_dtype)[None, :]
        expected = torch.lcm(left_cpu, right_cpu)
        got = torch.lcm(left_cpu.to("mps"), right_cpu.to("mps"))
        torch.mps.synchronize()
        if not torch.equal(got.cpu(), expected):
            raise AssertionError(f"lcm dense signed-small mismatch for {dense_dtype}")

    empty_left = torch.empty(0, 1, dtype=torch.int32)
    empty_right = torch.empty(3, dtype=torch.int32)
    empty_got = torch.lcm(empty_left.to("mps"), empty_right.to("mps"))
    torch.mps.synchronize()
    if empty_got.device.type != "mps" or empty_got.shape != (0, 3):
        raise AssertionError(f"lcm empty broadcast mismatch: {empty_got.device} {empty_got.shape}")

    noncontiguous_left = torch.arange(1, 40, dtype=torch.int32)[::2]
    noncontiguous_right = torch.arange(2, 41, dtype=torch.int32)[::2]
    expected_noncontiguous = torch.lcm(noncontiguous_left, noncontiguous_right)
    got_noncontiguous = torch.lcm(noncontiguous_left.to("mps"), noncontiguous_right.to("mps"))
    torch.mps.synchronize()
    if not torch.equal(got_noncontiguous.cpu(), expected_noncontiguous):
        raise AssertionError("lcm non-contiguous input mismatch")

    try:
        torch.lcm(
            torch.tensor([1.0], device="mps"),
            torch.tensor([1.0], device="mps"),
        )
    except NotImplementedError:
        pass
    else:
        raise AssertionError("lcm unexpectedly accepted floating tensors")

    try:
        torch.lcm(
            torch.tensor([True], device="mps"),
            torch.tensor([False], device="mps"),
        )
    except NotImplementedError:
        pass
    else:
        raise AssertionError("lcm(bool, bool) unexpectedly succeeded")

    direct_left = torch.tensor([6, 0, -6, 127], dtype=torch.int32)
    direct_right = torch.tensor([4, 5, 4, 2], dtype=torch.int32)
    direct_expected = torch.ops.aten.lcm.default(direct_left, direct_right)
    direct_got = torch.ops.aten.lcm.default(direct_left.to("mps"), direct_right.to("mps"))
    direct_out = torch.empty(1, dtype=torch.float32, device="mps")
    direct_returned = torch.ops.aten.lcm.out(
        direct_left.to("mps"),
        direct_right.to("mps"),
        out=direct_out,
    )
    torch.mps.synchronize()
    if direct_returned.data_ptr() != direct_out.data_ptr():
        raise AssertionError("lcm.out did not return the provided output tensor")
    if direct_out.shape != direct_expected.shape:
        raise AssertionError(f"lcm.out did not resize output: {direct_out.shape}")
    if not torch.equal(direct_got.cpu(), direct_expected):
        raise AssertionError("direct aten lcm mismatch")
    if not torch.equal(direct_returned.cpu(), direct_expected.float()):
        raise AssertionError("direct aten lcm.out mismatch")

    for out_dtype in (torch.uint8, torch.int8, torch.int16, torch.int64, torch.float16, torch.complex64):
        out = torch.empty(0, dtype=out_dtype, device="mps")
        expected_out = torch.empty(0, dtype=out_dtype)
        torch.lcm(direct_left, direct_right, out=expected_out)
        returned = torch.lcm(direct_left.to("mps"), direct_right.to("mps"), out=out)
        torch.mps.synchronize()
        if returned.data_ptr() != out.data_ptr():
            raise AssertionError(f"lcm.out did not return out for {out_dtype}")
        if not torch.equal(out.cpu(), expected_out):
            raise AssertionError(f"lcm.out cast mismatch for {out_dtype}: {out.cpu()} != {expected_out}")

    exact_out = torch.empty_like(direct_left, device="mps")
    exact_ptr = exact_out.data_ptr()
    torch.lcm(direct_left.to("mps"), direct_right.to("mps"), out=exact_out)
    torch.mps.synchronize()
    if exact_out.data_ptr() != exact_ptr:
        raise AssertionError("lcm.out unexpectedly changed storage for exact-shape out")

    noncontiguous_out_base = torch.empty(4, 2, dtype=torch.int32, device="mps")
    noncontiguous_out = noncontiguous_out_base[:, 0]
    if noncontiguous_out.is_contiguous():
        raise AssertionError("lcm non-contiguous out fixture is contiguous")
    torch.lcm(direct_left.to("mps"), direct_right.to("mps"), out=noncontiguous_out)
    torch.mps.synchronize()
    if not torch.equal(noncontiguous_out.cpu(), direct_expected):
        raise AssertionError("lcm.out non-contiguous output mismatch")

    try:
        torch.lcm(
            direct_left.to("mps"),
            direct_right.to("mps"),
            out=torch.empty(4, dtype=torch.bool, device="mps"),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("lcm.out unexpectedly accepted bool output")

    alias_input = torch.tensor([48, 0, -18, 127], dtype=torch.int64, device="mps")
    alias_other = torch.tensor([18, 0, 12, 2], dtype=torch.int64, device="mps")
    alias_expected = torch.lcm(alias_input.cpu(), alias_other.cpu())
    torch.lcm(alias_input, alias_other, out=alias_input)
    torch.mps.synchronize()
    if not torch.equal(alias_input.cpu(), alias_expected):
        raise AssertionError("lcm.out alias-input mismatch")

    alias_other_input = torch.tensor([48, 0, -18, 127], dtype=torch.int64, device="mps")
    alias_other = torch.tensor([18, 0, 12, 2], dtype=torch.int64, device="mps")
    alias_other_expected = torch.lcm(alias_other_input.cpu(), alias_other.cpu())
    torch.lcm(alias_other_input, alias_other, out=alias_other)
    torch.mps.synchronize()
    if not torch.equal(alias_other.cpu(), alias_other_expected):
        raise AssertionError("lcm.out alias-other mismatch")

    noncontiguous_alias_base = torch.tensor(
        [[48, 7], [0, 9], [-18, 11], [127, 13]],
        dtype=torch.int64,
        device="mps",
    )
    noncontiguous_alias = noncontiguous_alias_base[:, 0]
    noncontiguous_other = torch.tensor([18, 0, 12, 2], dtype=torch.int64, device="mps")
    noncontiguous_expected = torch.lcm(noncontiguous_alias.cpu(), noncontiguous_other.cpu())
    torch.lcm(noncontiguous_alias, noncontiguous_other, out=noncontiguous_alias)
    torch.mps.synchronize()
    if not torch.equal(noncontiguous_alias.cpu(), noncontiguous_expected):
        raise AssertionError("lcm.out non-contiguous exact alias mismatch")

    overlap_base = torch.arange(8, dtype=torch.int32, device="mps")
    try:
        torch.lcm(
            overlap_base[:-1],
            torch.ones(7, dtype=torch.int32, device="mps"),
            out=overlap_base[1:],
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("lcm.out unexpectedly accepted partial overlap")

    try:
        torch.lcm(
            direct_left.to("mps"),
            direct_right.to("mps"),
            out=torch.empty(1, dtype=torch.int32, device="mps").expand(4),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("lcm.out unexpectedly accepted expanded output")

    try:
        torch.lcm(
            direct_left.to("mps"),
            direct_right.to("mps"),
            out=torch.empty_like(direct_left),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("lcm.out unexpectedly accepted CPU output")

    inplace_input = torch.tensor([6, 0, -6, 127], dtype=torch.int64)
    inplace_other = torch.tensor([4, 5, 4, 2], dtype=torch.int64)
    inplace_expected = inplace_input.clone()
    inplace_expected.lcm_(inplace_other)
    inplace_got = inplace_input.to("mps")
    returned = inplace_got.lcm_(inplace_other.to("mps"))
    torch.mps.synchronize()
    if returned.data_ptr() != inplace_got.data_ptr():
        raise AssertionError("lcm_ did not return self")
    if not torch.equal(inplace_got.cpu(), inplace_expected):
        raise AssertionError(f"lcm_ mismatch: {inplace_got.cpu()} != {inplace_expected}")

    noncontiguous_inplace_base = torch.tensor(
        [[6, 1], [0, 2], [-6, 3], [127, 4]],
        dtype=torch.int64,
        device="mps",
    )
    noncontiguous_inplace = noncontiguous_inplace_base[:, 0]
    noncontiguous_inplace_other = torch.tensor([4, 5, 4, 2], dtype=torch.int64, device="mps")
    noncontiguous_inplace_expected = noncontiguous_inplace.cpu().clone()
    noncontiguous_inplace_expected.lcm_(noncontiguous_inplace_other.cpu())
    noncontiguous_inplace.lcm_(noncontiguous_inplace_other)
    torch.mps.synchronize()
    if not torch.equal(noncontiguous_inplace.cpu(), noncontiguous_inplace_expected):
        raise AssertionError("lcm_ non-contiguous exact alias mismatch")

    try:
        torch.tensor([2], dtype=torch.int64, device="mps").lcm_(
            torch.tensor([3, 4], dtype=torch.int64, device="mps")
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("lcm_ unexpectedly accepted broadcasted output shape")

    try:
        torch.tensor([True, False], dtype=torch.bool, device="mps").lcm_(
            torch.tensor([2, 0], dtype=torch.int64, device="mps")
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("lcm_ unexpectedly accepted bool self")

    second_install = install_mps_compat_kernels()
    if "aten::lcm.default" not in second_install.already_registered:
        raise AssertionError(f"lcm install was not idempotent: {second_install}")

    print(f"lcm: ok installed={install_result.installed}")


def check_reduction_out(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {"aten::std.correction_out", "aten::var.correction_out"}
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"std/var correction_out routes were not installed or skipped cleanly: {missing}")

    fixtures = [
        torch.tensor(
            [[0.0, 1.0, 2.0, 3.0], [0.0, 2.0, 4.0, 6.0], [0.0, 3.0, 6.0, 9.0]],
            dtype=torch.float32,
        ),
        torch.randn(2, 3, 4, dtype=torch.float32),
    ]
    dims = [None, [0], [1], [-1], [0, 1], []]
    corrections = [None, 0, 1, 2]

    for name, op in [
        ("std", torch.ops.aten.std),
        ("var", torch.ops.aten.var),
    ]:
        for source in fixtures:
            for dtype in (torch.float32, torch.float16):
                source_cpu = source.to(dtype)
                source_mps = source_cpu.to("mps")
                for dim in dims:
                    for correction in corrections:
                        for keepdim in (False, True):
                            try:
                                expected = op.correction(
                                    source_cpu,
                                    dim,
                                    correction=correction,
                                    keepdim=keepdim,
                                )
                            except RuntimeError:
                                continue
                            out = torch.empty(0, dtype=expected.dtype, device="mps")
                            returned = op.correction_out(
                                source_mps,
                                dim,
                                correction=correction,
                                keepdim=keepdim,
                                out=out,
                            )
                            torch.mps.synchronize()
                            if returned.data_ptr() != out.data_ptr():
                                raise AssertionError(f"{name}.correction_out did not return out")
                            if out.shape != expected.shape:
                                raise AssertionError(
                                    f"{name}.correction_out shape mismatch: {out.shape} != {expected.shape}"
                                )
                            tolerance = 2e-2 if dtype == torch.float16 else 1e-5
                            got_cpu = out.cpu().float()
                            expected_cpu = expected.float()
                            nan_mask_matches = torch.equal(torch.isnan(got_cpu), torch.isnan(expected_cpu))
                            max_diff = (
                                (got_cpu[~torch.isnan(expected_cpu)] - expected_cpu[~torch.isnan(expected_cpu)])
                                .abs()
                                .max()
                                .item()
                                if expected.numel() and (~torch.isnan(expected_cpu)).any()
                                else 0.0
                            )
                            if not nan_mask_matches or max_diff > tolerance:
                                raise AssertionError(
                                    f"{name}.correction_out mismatch for {dtype} dim={dim} "
                                    f"correction={correction} keepdim={keepdim}: {out.cpu()} != {expected}"
                                )

        cast_source = fixtures[0]
        for out_dtype in (torch.float16, torch.float32):
            expected = torch.empty(0, dtype=out_dtype)
            op.correction_out(cast_source, [1], correction=1, keepdim=False, out=expected)
            out = torch.empty(0, dtype=out_dtype, device="mps")
            op.correction_out(cast_source.to("mps"), [1], correction=1, keepdim=False, out=out)
            torch.mps.synchronize()
            if not torch.allclose(out.cpu().float(), expected.float(), atol=2e-2, equal_nan=True):
                raise AssertionError(f"{name}.correction_out cast mismatch for {out_dtype}")

        noncontiguous_base = torch.empty(3, 2, dtype=torch.float32, device="mps")
        noncontiguous_out = noncontiguous_base[:, 0]
        expected = op.correction(cast_source, [1], correction=1, keepdim=False)
        op.correction_out(cast_source.to("mps"), [1], correction=1, keepdim=False, out=noncontiguous_out)
        torch.mps.synchronize()
        if not torch.allclose(noncontiguous_out.cpu(), expected, atol=1e-5, equal_nan=True):
            raise AssertionError(f"{name}.correction_out non-contiguous output mismatch")

        overlap_source = cast_source.to("mps")
        overlap_expected = op.correction(cast_source, [1], correction=1, keepdim=False)
        op.correction_out(overlap_source, [1], correction=1, keepdim=False, out=overlap_source[:, 0])
        torch.mps.synchronize()
        if not torch.allclose(overlap_source[:, 0].cpu(), overlap_expected, atol=1e-5, equal_nan=True):
            raise AssertionError(f"{name}.correction_out overlap output mismatch")

        expanded_source = torch.tensor(
            [[0.0, 1.0, 2.0, 3.0], [0.0, 2.0, 4.0, 6.0], [0.0, 3.0, 6.0, 9.0]],
            dtype=torch.float32,
        )
        expanded_out = torch.full((1,), -999.0, device="mps").expand(3)
        try:
            op.correction_out(
                expanded_source.to("mps"),
                [1],
                correction=1,
                keepdim=False,
                out=expanded_out,
            )
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"{name}.correction_out unexpectedly accepted expanded output")

        partial_overlap_out = torch.as_strided(
            torch.empty(6, dtype=torch.float32, device="mps"),
            (3, 4),
            (1, 1),
        )
        try:
            op.correction_out(
                expanded_source.to("mps"),
                [],
                correction=0,
                keepdim=False,
                out=partial_overlap_out,
            )
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"{name}.correction_out unexpectedly accepted partial-overlap output")

        try:
            op.correction_out(
                cast_source.to("mps"),
                [1],
                correction=1,
                keepdim=False,
                out=torch.empty(3, dtype=torch.int32, device="mps"),
            )
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"{name}.correction_out unexpectedly accepted int output")

        try:
            op.correction_out(
                cast_source.to("mps"),
                [1],
                correction=1,
                keepdim=False,
                out=torch.empty(3, dtype=torch.complex64, device="mps"),
            )
        except NotImplementedError:
            pass
        else:
            raise AssertionError(f"{name}.correction_out unexpectedly accepted complex output")

        try:
            op.correction_out(
                cast_source.to(torch.complex64).to("mps"),
                [1],
                correction=1,
                keepdim=False,
                out=torch.empty(3, dtype=torch.float32, device="mps"),
            )
        except NotImplementedError:
            pass
        else:
            raise AssertionError(f"{name}.correction_out unexpectedly accepted complex input")

        try:
            op.correction_out(
                cast_source.to("mps"),
                [1],
                correction=1,
                keepdim=False,
                out=torch.empty(3, dtype=torch.float32),
            )
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"{name}.correction_out unexpectedly accepted CPU output")

    second_install = install_mps_compat_kernels()
    if "aten::std.correction_out" not in second_install.already_registered:
        raise AssertionError(f"std.correction_out install was not idempotent: {second_install}")
    if "aten::var.correction_out" not in second_install.already_registered:
        raise AssertionError(f"var.correction_out install was not idempotent: {second_install}")

    print(f"std/var correction_out: ok installed={install_result.installed}")


def _assert_take_matches_cpu(source_cpu: torch.Tensor, index_cpu: torch.Tensor) -> None:
    expected = torch.take(source_cpu, index_cpu)
    got = torch.take(source_cpu.to("mps"), index_cpu.to("mps"))
    torch.mps.synchronize()
    got_cpu = got.cpu()
    if got.shape != expected.shape:
        raise AssertionError(f"take shape mismatch: {got.shape} != {expected.shape}")
    if source_cpu.is_floating_point():
        if not torch.allclose(got_cpu.float(), expected.float(), atol=1e-5, equal_nan=True):
            raise AssertionError(f"take mismatch: {got_cpu} != {expected}")
    elif not torch.equal(got_cpu, expected):
        raise AssertionError(f"take mismatch: {got_cpu} != {expected}")


def check_take(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {"aten::take.default", "aten::take.out"}
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"take routes were not installed or skipped cleanly: {missing}")

    index_cases = [
        torch.tensor([0, 5, -1, -12], dtype=torch.int64),
        torch.tensor([[0, 2], [-1, -12]], dtype=torch.int64),
        torch.tensor(2, dtype=torch.int64),
        torch.empty(0, dtype=torch.int64),
        torch.empty(2, 0, dtype=torch.int64),
    ]
    dtype_cases = [
        torch.bool,
        torch.uint8,
        torch.int8,
        torch.int16,
        torch.int32,
        torch.int64,
        torch.float16,
        torch.bfloat16,
        torch.float32,
    ]
    base = torch.arange(12).reshape(3, 4)
    for dtype in dtype_cases:
        source = (base % 2).to(dtype) if dtype == torch.bool else base.to(dtype)
        for index in index_cases:
            _assert_take_matches_cpu(source, index)

    _assert_take_matches_cpu(torch.arange(12).reshape(3, 4).t(), torch.tensor([0, 1, 4, 11, -1]))
    _assert_take_matches_cpu(torch.arange(20)[::2], torch.tensor([0, 1, 9, -1]))
    _assert_take_matches_cpu(torch.tensor(42.0), torch.tensor([0, -1]))
    _assert_take_matches_cpu(torch.tensor(42.0), torch.tensor(0))
    _assert_take_matches_cpu(torch.tensor(42.0), torch.empty(0, dtype=torch.int64))
    _assert_take_matches_cpu(torch.tensor(42.0), torch.empty(0, 3, dtype=torch.int64))
    _assert_take_matches_cpu(torch.empty(0), torch.empty(0, dtype=torch.int64))
    _assert_take_matches_cpu(
        torch.arange(12),
        torch.tensor([[0, 1, 2], [3, 4, 5]], dtype=torch.int64).t(),
    )

    for bad_index in [
        torch.tensor([0, 12], dtype=torch.int64, device="mps"),
        torch.tensor([-13], dtype=torch.int64, device="mps"),
        torch.tensor([0], dtype=torch.int64, device="mps"),
        torch.tensor([1], dtype=torch.int64, device="mps"),
    ]:
        source = (
            torch.empty(0, device="mps")
            if bad_index.numel() == 1 and bad_index.item() == 0
            else torch.tensor(42.0, device="mps")
            if bad_index.numel() == 1 and bad_index.item() == 1
            else torch.arange(12, device="mps")
        )
        try:
            torch.take(source, bad_index)
            torch.mps.synchronize()
        except IndexError:
            pass
        else:
            raise AssertionError(f"take unexpectedly accepted bad index {bad_index.cpu()}")

    try:
        torch.take(
            torch.arange(12, device="mps"),
            torch.tensor([0, 1], dtype=torch.int32, device="mps"),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("take unexpectedly accepted non-int64 index")

    try:
        torch.take(
            torch.arange(12).to(torch.complex64).to("mps"),
            torch.tensor([0, 1], dtype=torch.int64, device="mps"),
        )
    except NotImplementedError:
        pass
    else:
        raise AssertionError("take unexpectedly accepted complex input")

    try:
        torch.take(torch.arange(12, device="mps"), torch.tensor([0, 1], dtype=torch.int64))
    except RuntimeError:
        pass
    else:
        raise AssertionError("take unexpectedly accepted CPU index")

    source_cpu = torch.arange(12, dtype=torch.float32).reshape(3, 4)
    index_cpu = torch.tensor([0, 5, -1, -12], dtype=torch.int64)
    expected = torch.take(source_cpu, index_cpu)
    out = torch.empty(0, dtype=source_cpu.dtype, device="mps")
    returned = torch.take(source_cpu.to("mps"), index_cpu.to("mps"), out=out)
    torch.mps.synchronize()
    if returned.data_ptr() != out.data_ptr():
        raise AssertionError("take.out did not return out")
    if not torch.allclose(out.cpu(), expected, atol=1e-5):
        raise AssertionError("take.out output mismatch")

    scalar_source = torch.tensor(42.0)
    scalar_index = torch.tensor(0, dtype=torch.int64)
    scalar_expected = torch.take(scalar_source, scalar_index)
    scalar_out = torch.empty((), dtype=scalar_source.dtype, device="mps")
    torch.take(scalar_source.to("mps"), scalar_index.to("mps"), out=scalar_out)
    torch.mps.synchronize()
    if scalar_out.shape != scalar_expected.shape or scalar_out.item() != scalar_expected.item():
        raise AssertionError("take.out scalar output mismatch")

    noncontiguous_base = torch.empty(4, 2, dtype=torch.float32, device="mps")
    noncontiguous_out = noncontiguous_base[:, 0]
    torch.take(source_cpu.to("mps"), index_cpu.to("mps"), out=noncontiguous_out)
    torch.mps.synchronize()
    if not torch.allclose(noncontiguous_out.cpu(), expected, atol=1e-5):
        raise AssertionError("take.out non-contiguous output mismatch")

    resized_out = torch.empty(1, dtype=source_cpu.dtype, device="mps")
    torch.take(source_cpu.to("mps"), index_cpu.to("mps"), out=resized_out)
    torch.mps.synchronize()
    if resized_out.shape != expected.shape or not torch.allclose(resized_out.cpu(), expected, atol=1e-5):
        raise AssertionError("take.out resized output mismatch")

    try:
        torch.take(
            source_cpu.to("mps"),
            index_cpu.to("mps"),
            out=torch.empty(1, dtype=torch.float32, device="mps").expand(4),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("take.out unexpectedly accepted expanded output")

    try:
        torch.take(
            source_cpu.to("mps"),
            index_cpu.to("mps"),
            out=torch.as_strided(torch.empty(5, dtype=torch.float32, device="mps"), (2, 2), (1, 1)),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("take.out unexpectedly accepted partial-overlap output")

    alias_source = torch.arange(12, dtype=torch.float32, device="mps")
    try:
        torch.take(alias_source, torch.tensor([0, 1, 2], device="mps"), out=alias_source[:3])
    except RuntimeError:
        pass
    else:
        raise AssertionError("take.out unexpectedly accepted output overlapping input")

    alias_index_input = torch.arange(12, dtype=torch.int64, device="mps")
    alias_index = torch.tensor([0, 1, 2], dtype=torch.int64, device="mps")
    try:
        torch.take(alias_index_input, alias_index, out=alias_index)
    except RuntimeError:
        pass
    else:
        raise AssertionError("take.out unexpectedly accepted output overlapping index")

    for bad_out in [
        torch.empty(4, dtype=torch.int32, device="mps"),
        torch.empty(4, dtype=torch.float32),
    ]:
        try:
            torch.take(source_cpu.to("mps"), index_cpu.to("mps"), out=bad_out)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"take.out guard did not fire for {bad_out.device}/{bad_out.dtype}")

    second_install = install_mps_compat_kernels()
    if "aten::take.default" not in second_install.already_registered:
        raise AssertionError(f"take install was not idempotent: {second_install}")
    if "aten::take.out" not in second_install.already_registered:
        raise AssertionError(f"take.out install was not idempotent: {second_install}")

    print(f"take: ok installed={install_result.installed}")


def _assert_logit_inplace_matches_cpu(values: torch.Tensor, dtype: torch.dtype, eps: float | None) -> None:
    source_cpu = values.to(dtype)
    expected = source_cpu.clone()
    expected_returned = expected.logit_(eps=eps)
    source_mps = source_cpu.to("mps")
    original_ptr = source_mps.data_ptr()
    returned = source_mps.logit_(eps=eps)
    torch.mps.synchronize()
    if returned.data_ptr() != original_ptr or source_mps.data_ptr() != original_ptr:
        raise AssertionError("logit_ did not preserve tensor identity")
    got = source_mps.cpu().float()
    want = expected.float()
    if not torch.equal(torch.isnan(got), torch.isnan(want)):
        raise AssertionError(f"logit_ NaN mask mismatch for {dtype} eps={eps}: {got} != {want}")
    if not torch.equal(torch.isinf(got), torch.isinf(want)):
        raise AssertionError(f"logit_ inf mask mismatch for {dtype} eps={eps}: {got} != {want}")
    finite = torch.isfinite(want)
    if finite.any():
        tolerance = 3e-2 if dtype in (torch.float16, torch.bfloat16) else 1e-5
        diff = (got[finite] - want[finite]).abs().max().item()
        if diff > tolerance:
            raise AssertionError(f"logit_ mismatch for {dtype} eps={eps}: {diff} {got} != {want}")
    if expected_returned.data_ptr() != expected.data_ptr():
        raise AssertionError("CPU logit_ sanity check did not return self")


def _logit_grad_cpu(values: torch.Tensor, eps: float | None, repeated_use: bool = False) -> torch.Tensor:
    base = values.detach().clone().requires_grad_(True)
    tensor = base * 0.8 + 0.1
    if repeated_use:
        before = tensor.sin()
        tensor.logit_(eps=eps)
        loss = before.sum() + tensor.cos().sum()
    else:
        tensor.logit_(eps=eps)
        loss = tensor.sum()
    loss.backward()
    assert base.grad is not None
    return base.grad.detach()


def _logit_grad_mps(values: torch.Tensor, eps: float | None, repeated_use: bool = False) -> torch.Tensor:
    base = values.detach().clone().to("mps").requires_grad_(True)
    tensor = base * 0.8 + 0.1
    if repeated_use:
        before = tensor.sin()
        tensor.logit_(eps=eps)
        loss = before.sum() + tensor.cos().sum()
    else:
        tensor.logit_(eps=eps)
        loss = tensor.sum()
    loss.backward()
    torch.mps.synchronize()
    assert base.grad is not None
    return base.grad.detach().cpu()


def check_logit_inplace(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {"aten::logit_"}
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"logit_ route was not installed or skipped cleanly: {missing}")

    values = torch.tensor([-1.0, 0.0, 0.001, 0.2, 0.5, 0.8, 0.999, 1.0, 2.0])
    for dtype in (torch.float16, torch.bfloat16, torch.float32):
        for eps in (None, -0.1, 0.0, 1e-3, 0.5):
            _assert_logit_inplace_matches_cpu(values, dtype, eps)

    base_cpu = torch.full((4, 4), 0.5)
    base_mps = base_cpu.to("mps")
    view_cpu = base_cpu[::2, ::2]
    view_mps = base_mps[::2, ::2]
    view_mps_ptr = view_mps.data_ptr()
    view_cpu.logit_()
    returned = view_mps.logit_()
    torch.mps.synchronize()
    if returned.data_ptr() != view_mps_ptr:
        raise AssertionError("logit_ non-contiguous view did not return the view")
    if not torch.allclose(base_mps.cpu(), base_cpu, atol=1e-5, equal_nan=True):
        raise AssertionError("logit_ non-contiguous view did not mutate base like CPU")

    for bad_tensor, expected_error in [
        (torch.tensor([0, 1], dtype=torch.int64, device="mps"), RuntimeError),
        (torch.tensor([True, False], dtype=torch.bool, device="mps"), RuntimeError),
        (torch.tensor([0.2 + 0j], dtype=torch.complex64, device="mps"), NotImplementedError),
    ]:
        try:
            bad_tensor.logit_()
        except expected_error:
            pass
        else:
            raise AssertionError(f"logit_ unexpectedly accepted {bad_tensor.dtype}")

    try:
        torch.tensor([0.2], device="mps", requires_grad=True).logit_()
    except RuntimeError:
        pass
    else:
        raise AssertionError("logit_ unexpectedly accepted leaf tensor requiring grad")

    leaf = torch.tensor([0.2], device="mps", requires_grad=True)
    with torch.no_grad():
        leaf.logit_()
    torch.mps.synchronize()
    if not torch.allclose(leaf.detach().cpu(), torch.tensor([0.2]).logit(), atol=1e-5):
        raise AssertionError("logit_ no_grad leaf mismatch")

    try:
        torch.tensor([0.2, 0.5, 0.8], device="mps").logit_(eps=0.6)
    except RuntimeError as exc:
        if "eps > 0.5" not in str(exc):
            raise AssertionError(f"unexpected logit_ eps guard error: {exc}") from exc
    else:
        raise AssertionError("logit_ unexpectedly accepted eps > 0.5")

    grad_values = torch.tensor([0.2, 0.5, 0.8])
    for eps in (None, 1e-3, 0.5):
        expected_grad = _logit_grad_cpu(grad_values, eps)
        got_grad = _logit_grad_mps(grad_values, eps)
        if not torch.allclose(got_grad, expected_grad, atol=1e-5, equal_nan=True):
            raise AssertionError(f"logit_ grad mismatch for eps={eps}: {got_grad} != {expected_grad}")

    boundary_values = torch.tensor([0.0, 0.001, 0.002, 0.998, 0.999, 1.0])
    boundary_cpu = boundary_values.detach().clone().requires_grad_(True)
    boundary_cpu_tensor = boundary_cpu * 1.0
    boundary_cpu_tensor.logit_(eps=1e-3).sum().backward()
    boundary_mps = boundary_values.detach().clone().to("mps").requires_grad_(True)
    boundary_mps_tensor = boundary_mps * 1.0
    boundary_mps_tensor.logit_(eps=1e-3).sum().backward()
    torch.mps.synchronize()
    assert boundary_cpu.grad is not None and boundary_mps.grad is not None
    if not torch.allclose(
        boundary_mps.grad.cpu(),
        boundary_cpu.grad,
        atol=1e-5,
        equal_nan=True,
    ):
        raise AssertionError(
            f"logit_ clamp-boundary grad mismatch: {boundary_mps.grad.cpu()} != {boundary_cpu.grad}"
        )

    try:
        expected_repeated = _logit_grad_cpu(grad_values, 1e-3, repeated_use=True)
    except RuntimeError as cpu_error:
        try:
            _logit_grad_mps(grad_values, 1e-3, repeated_use=True)
        except RuntimeError as mps_error:
            if "modified by an inplace operation" not in str(mps_error):
                raise AssertionError(f"unexpected MPS repeated-use logit_ error: {mps_error}") from mps_error
        else:
            raise AssertionError("MPS repeated-use logit_ backward succeeded but CPU failed") from cpu_error
    else:
        got_repeated = _logit_grad_mps(grad_values, 1e-3, repeated_use=True)
        if not torch.allclose(got_repeated, expected_repeated, atol=1e-5, equal_nan=True):
            raise AssertionError(f"logit_ repeated-use grad mismatch: {got_repeated} != {expected_repeated}")

    second_install = install_mps_compat_kernels()
    if "aten::logit_" not in second_install.already_registered:
        raise AssertionError(f"logit_ install was not idempotent: {second_install}")

    print(f"logit_: ok installed={install_result.installed}")


def _addmm_activation_reference(
    input_tensor: torch.Tensor,
    mat1: torch.Tensor,
    mat2: torch.Tensor,
    *,
    beta: float | int = 1,
    alpha: float | int = 1,
    use_gelu: bool = False,
) -> torch.Tensor:
    computed = torch.addmm(input_tensor, mat1, mat2, beta=beta, alpha=alpha)
    if use_gelu:
        return F.gelu(computed, approximate="none")
    return torch.relu(computed)


def _assert_addmm_activation_matches_reference(
    input_cpu: torch.Tensor,
    mat1_cpu: torch.Tensor,
    mat2_cpu: torch.Tensor,
    *,
    beta: float | int,
    alpha: float | int,
    use_gelu: bool,
) -> None:
    expected = _addmm_activation_reference(
        input_cpu,
        mat1_cpu,
        mat2_cpu,
        beta=beta,
        alpha=alpha,
        use_gelu=use_gelu,
    )
    got = torch.ops.aten._addmm_activation.default(
        input_cpu.to("mps"),
        mat1_cpu.to("mps"),
        mat2_cpu.to("mps"),
        beta=beta,
        alpha=alpha,
        use_gelu=use_gelu,
    )
    torch.mps.synchronize()
    tolerance = 5e-2 if input_cpu.dtype in (torch.float16, torch.bfloat16) else 1e-5
    if got.device.type != "mps":
        raise AssertionError(f"_addmm_activation returned {got.device}, expected mps")
    if not torch.allclose(
        got.cpu().float(),
        expected.float(),
        atol=tolerance,
        rtol=tolerance,
        equal_nan=True,
    ):
        diff = (got.cpu().float() - expected.float()).abs().max().item()
        raise AssertionError(
            f"_addmm_activation mismatch dtype={input_cpu.dtype} "
            f"gelu={use_gelu} beta={beta} alpha={alpha}: {diff}"
        )


def _addmm_activation_grads(
    input_cpu: torch.Tensor,
    mat1_cpu: torch.Tensor,
    mat2_cpu: torch.Tensor,
    *,
    device: str,
    use_route: bool,
    use_gelu: bool,
    beta: float,
    alpha: float,
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    input_tensor = input_cpu.detach().clone().to(device).requires_grad_(True)
    mat1 = mat1_cpu.detach().clone().to(device).requires_grad_(True)
    mat2 = mat2_cpu.detach().clone().to(device).requires_grad_(True)
    if use_route:
        output = torch.ops.aten._addmm_activation.default(
            input_tensor,
            mat1,
            mat2,
            beta=beta,
            alpha=alpha,
            use_gelu=use_gelu,
        )
    else:
        output = _addmm_activation_reference(
            input_tensor,
            mat1,
            mat2,
            beta=beta,
            alpha=alpha,
            use_gelu=use_gelu,
        )
    output.sum().backward()
    if input_tensor.grad is None or mat1.grad is None or mat2.grad is None:
        raise AssertionError("_addmm_activation gradient check produced missing grads")
    if device == "mps":
        torch.mps.synchronize()
    return (
        input_tensor.grad.detach().cpu(),
        mat1.grad.detach().cpu(),
        mat2.grad.detach().cpu(),
    )


def check_addmm_activation(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {
        "aten::_addmm_activation.default",
        "aten::_addmm_activation.out",
    }
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"_addmm_activation routes were not installed or skipped cleanly: {missing}")

    torch.manual_seed(100)
    for dtype in (torch.float16, torch.bfloat16, torch.float32):
        input_cpu = torch.randn(3, 5, dtype=dtype)
        mat1_cpu = torch.randn(3, 4, dtype=dtype)
        mat2_cpu = torch.randn(4, 5, dtype=dtype)
        for use_gelu in (False, True):
            for beta, alpha in ((1, 1), (0.5, 1.25), (0, 2)):
                _assert_addmm_activation_matches_reference(
                    input_cpu,
                    mat1_cpu,
                    mat2_cpu,
                    beta=beta,
                    alpha=alpha,
                    use_gelu=use_gelu,
                )

    nan_input = torch.full((3, 5), float("nan"))
    mat1 = torch.randn(3, 4)
    mat2 = torch.randn(4, 5)
    _assert_addmm_activation_matches_reference(
        nan_input,
        mat1,
        mat2,
        beta=0,
        alpha=1,
        use_gelu=False,
    )

    bias = torch.randn(5)
    _assert_addmm_activation_matches_reference(
        bias,
        mat1,
        mat2,
        beta=1,
        alpha=1,
        use_gelu=True,
    )

    out = torch.empty(0, device="mps")
    returned = torch.ops.aten._addmm_activation.out(
        bias.to("mps"),
        mat1.to("mps"),
        mat2.to("mps"),
        beta=0.25,
        alpha=1.5,
        use_gelu=True,
        out=out,
    )
    torch.mps.synchronize()
    if returned.data_ptr() != out.data_ptr() or tuple(out.shape) != (3, 5):
        raise AssertionError("_addmm_activation.out did not return and resize out")
    expected_out = _addmm_activation_reference(
        bias,
        mat1,
        mat2,
        beta=0.25,
        alpha=1.5,
        use_gelu=True,
    )
    if not torch.allclose(out.cpu(), expected_out, atol=1e-5, rtol=1e-5):
        raise AssertionError("_addmm_activation.out did not match reference")

    for bad_out in (
        torch.empty(3, 5),
        torch.empty(3, 5, dtype=torch.float16, device="mps"),
    ):
        try:
            torch.ops.aten._addmm_activation.out(
                bias.to("mps"),
                mat1.to("mps"),
                mat2.to("mps"),
                out=bad_out,
            )
        except RuntimeError:
            pass
        else:
            raise AssertionError("_addmm_activation.out guard did not fire")

    overlap = torch.empty(3, 5, device="mps")
    try:
        torch.ops.aten._addmm_activation.out(
            overlap,
            mat1.to("mps"),
            mat2.to("mps"),
            out=overlap,
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("_addmm_activation.out overlap guard did not fire")

    grad_input = torch.randn(3, 5, device="mps", requires_grad=True)
    grad_mat1 = torch.randn(3, 4, device="mps", requires_grad=True)
    grad_mat2 = torch.randn(4, 5, device="mps", requires_grad=True)
    grad_result = torch.ops.aten._addmm_activation.default(
        grad_input,
        grad_mat1,
        grad_mat2,
        use_gelu=True,
    )
    try:
        grad_result.sum().backward()
    except RuntimeError as exc:
        if "derivative for aten::_addmm_activation is not implemented" not in str(exc):
            raise
    else:
        raise AssertionError("_addmm_activation default backward unexpectedly succeeded")

    os.environ["PSYCHE_MPS_COMPAT_ADDMM_ACTIVATION_GRAD"] = "1"
    grad_install = install_mps_compat_kernels()
    seen_grad_routes = set(grad_install.installed) | set(grad_install.already_registered)
    if "aten::_addmm_activation.autograd" not in seen_grad_routes:
        raise AssertionError(f"_addmm_activation autograd route not registered: {grad_install}")
    cpu_guard_input = torch.randn(3, 5, requires_grad=True)
    cpu_guard_mat1 = torch.randn(3, 4, requires_grad=True)
    cpu_guard_mat2 = torch.randn(4, 5, requires_grad=True)
    cpu_guard_result = torch.ops.aten._addmm_activation.default(
        cpu_guard_input,
        cpu_guard_mat1,
        cpu_guard_mat2,
        use_gelu=True,
    )
    try:
        cpu_guard_result.sum().backward()
    except RuntimeError as exc:
        if "derivative for aten::_addmm_activation is not implemented" not in str(exc):
            raise
    else:
        raise AssertionError("_addmm_activation opt-in AutogradMPS affected CPU backward")

    torch.manual_seed(200)
    for use_gelu in (False, True):
        input_cpu = torch.randn(3, 5)
        mat1_cpu = torch.randn(3, 4)
        mat2_cpu = torch.randn(4, 5)
        expected_grads = _addmm_activation_grads(
            input_cpu,
            mat1_cpu,
            mat2_cpu,
            device="cpu",
            use_route=False,
            use_gelu=use_gelu,
            beta=0.5,
            alpha=1.25,
        )
        got_grads = _addmm_activation_grads(
            input_cpu,
            mat1_cpu,
            mat2_cpu,
            device="mps",
            use_route=True,
            use_gelu=use_gelu,
            beta=0.5,
            alpha=1.25,
        )
        for got, expected in zip(got_grads, expected_grads):
            if not torch.allclose(got, expected, atol=1e-4, rtol=1e-4):
                raise AssertionError("_addmm_activation opt-in autograd mismatch")

    print(
        "_addmm_activation: ok "
        f"installed={install_result.installed} grad_installed={grad_install.installed}"
    )


def check_channel_shuffle(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {"aten::channel_shuffle.default"}
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"channel_shuffle route was not installed or skipped cleanly: {missing}")

    for dtype in (torch.float16, torch.bfloat16, torch.float32, torch.int64, torch.bool):
        if dtype is torch.bool:
            source = (torch.arange(2 * 4 * 3).reshape(2, 4, 3) % 2 == 0)
        else:
            source = torch.arange(2 * 4 * 3).reshape(2, 4, 3).to(dtype)
        expected = torch.channel_shuffle(source, 2)
        got = torch.channel_shuffle(source.to("mps"), 2)
        torch.mps.synchronize()
        if not torch.equal(got.cpu(), expected):
            raise AssertionError(f"channel_shuffle mismatch for {dtype}")
        if not got.is_contiguous():
            raise AssertionError("channel_shuffle result was not contiguous")

    noncontig = torch.arange(2 * 4 * 3 * 2, dtype=torch.float32).reshape(2, 4, 3, 2).transpose(-1, -2)
    expected_noncontig = torch.channel_shuffle(noncontig, 2)
    got_noncontig = torch.channel_shuffle(noncontig.to("mps"), 2)
    torch.mps.synchronize()
    if not torch.equal(got_noncontig.cpu(), expected_noncontig):
        raise AssertionError("channel_shuffle non-contiguous input mismatch")

    for bad_input, groups in [
        (torch.randn(4, 4, device="mps"), 2),
        (torch.randn(2, 4, 3, device="mps"), 0),
        (torch.randn(2, 5, 3, device="mps"), 2),
    ]:
        try:
            torch.channel_shuffle(bad_input, groups)
        except RuntimeError:
            pass
        else:
            raise AssertionError("channel_shuffle invalid input guard did not fire")

    base_cpu = torch.randn(2, 4, 3, requires_grad=True)
    base_mps = base_cpu.detach().clone().to("mps").requires_grad_(True)
    torch.channel_shuffle(base_cpu, 2).sum().backward()
    torch.channel_shuffle(base_mps, 2).sum().backward()
    torch.mps.synchronize()
    if base_cpu.grad is None or base_mps.grad is None:
        raise AssertionError("channel_shuffle gradient check produced missing grads")
    if not torch.equal(base_mps.grad.cpu(), base_cpu.grad):
        raise AssertionError("channel_shuffle gradient mismatch")

    print(f"channel_shuffle: ok installed={install_result.installed}")


def check_logspace_mvlgamma_vdot(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {
        "aten::logspace.default",
        "aten::logspace.out",
        "aten::mvlgamma.out",
        "aten::vdot.default",
        "aten::vdot.out",
    }
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"logspace/mvlgamma/vdot routes missing: {missing}")

    for dtype, tolerance in (
        (torch.float16, 5e-2),
        (torch.bfloat16, 5e-2),
        (torch.float32, 1e-5),
    ):
        for start, end, steps, base in (
            (0, 3, 4, 10.0),
            (-1, 2, 4, 10.0),
            (0.1, 1.0, 5, 10.0),
            (-1, 2, 4, 0.5),
            (0, 3, 4, -2.0),
            (-3, 3, 7, -1.5),
            (-2, 2, 5, -0.5),
            (2, 2, 1, -3.0),
            (0, 1, 0, -10.0),
            (-1, 2, 4, 0.0),
            (2, 2, 1, 2.0),
            (0, 1, 0, 10.0),
        ):
            expected = torch.logspace(start, end, steps, base=base, dtype=dtype)
            got = torch.logspace(start, end, steps, base=base, dtype=dtype, device="mps")
            torch.mps.synchronize()
            if got.device.type != "mps" or got.dtype != dtype:
                raise AssertionError(f"logspace returned {got.device}/{got.dtype}")
            if not torch.allclose(
                got.cpu().float(),
                expected.float(),
                atol=tolerance,
                rtol=tolerance,
                equal_nan=True,
            ):
                raise AssertionError(f"logspace mismatch for {dtype}/{start}/{end}/{steps}/{base}")

    out = torch.empty(0, device="mps")
    returned = torch.logspace(0, 2, 3, out=out)
    torch.mps.synchronize()
    if returned.data_ptr() != out.data_ptr() or tuple(out.shape) != (3,):
        raise AssertionError("logspace.out did not return and resize out")
    if not torch.allclose(out.cpu(), torch.logspace(0, 2, 3), atol=1e-5):
        raise AssertionError("logspace.out mismatch")

    grad_result = torch.logspace(0, 1, 3, device="mps", requires_grad=True)
    if not grad_result.requires_grad:
        raise AssertionError("logspace requires_grad was not preserved")

    for kwargs, expected_error in [
        ({"dtype": torch.float64, "device": "mps"}, NotImplementedError),
        ({"dtype": torch.complex64, "device": "mps"}, NotImplementedError),
        ({"dtype": torch.int64, "device": "mps"}, NotImplementedError),
        ({"dtype": torch.float32, "device": "mps", "base": -2.0}, NotImplementedError),
    ]:
        try:
            torch.logspace(0, 1, 3, **kwargs)
        except expected_error:
            pass
        else:
            raise AssertionError(f"logspace guard did not fire for {kwargs}")

    for start, end, steps, base in (
        (0, 1, 5, -2.0),
        (-2, 2, 9, -0.5),
        (0.25, 2.25, 9, -3.0),
        (-0.5, 0.5, 7, -10.0),
    ):
        try:
            torch.logspace(start, end, steps, base=base, dtype=torch.float32, device="mps")
        except NotImplementedError:
            pass
        else:
            raise AssertionError("logspace negative-base fractional grid guard did not fire")

    for bad_start, bad_end in (
        (1 + 0j, 3 + 0j),
        (torch.tensor(1 + 0j), torch.tensor(3 + 0j)),
    ):
        try:
            torch.logspace(bad_start, bad_end, 3, device="mps")
        except NotImplementedError:
            pass
        else:
            raise AssertionError("logspace complex endpoint guard did not fire")

    original_default = torch.get_default_dtype()
    try:
        torch.set_default_dtype(torch.float64)
        try:
            torch.logspace(0, 1, 3, device="mps")
        except NotImplementedError:
            pass
        else:
            raise AssertionError("logspace default float64 guard did not fire")
    finally:
        torch.set_default_dtype(original_default)

    try:
        torch.ops.aten.logspace.default(
            0,
            1,
            3,
            10.0,
            dtype=torch.float32,
            layout=torch.sparse_coo,
            device=torch.device("mps"),
            pin_memory=False,
        )
    except NotImplementedError:
        pass
    else:
        raise AssertionError("logspace layout guard did not fire")

    mvlgamma_source = torch.tensor([3.0, 4.0])
    for out_dtype, tolerance in ((torch.float32, 1e-5), (torch.float16, 5e-3)):
        expected = torch.mvlgamma(mvlgamma_source, 2).to(out_dtype)
        mvlgamma_out = torch.empty(0, dtype=out_dtype, device="mps")
        returned = torch.mvlgamma(mvlgamma_source.to("mps"), 2, out=mvlgamma_out)
        torch.mps.synchronize()
        if returned.data_ptr() != mvlgamma_out.data_ptr() or tuple(mvlgamma_out.shape) != (2,):
            raise AssertionError("mvlgamma.out did not return and resize out")
        if not torch.allclose(mvlgamma_out.cpu().float(), expected.float(), atol=tolerance, rtol=tolerance):
            raise AssertionError(f"mvlgamma.out mismatch for {out_dtype}")

    mvlgamma_alias = mvlgamma_source.to("mps")
    bad_mvlgamma_cases = [
        (mvlgamma_source.to("mps"), torch.empty(0)),
        (mvlgamma_source.to("mps"), torch.empty(0, dtype=torch.int64, device="mps")),
        (mvlgamma_alias, mvlgamma_alias),
    ]
    for bad_input, bad_out in bad_mvlgamma_cases:
        try:
            torch.mvlgamma(bad_input, 2, out=bad_out)
        except RuntimeError:
            pass
        else:
            raise AssertionError("mvlgamma.out guard did not fire")

    for dtype in (
        torch.uint8,
        torch.int8,
        torch.int16,
        torch.int32,
        torch.int64,
        torch.float16,
        torch.bfloat16,
        torch.float32,
    ):
        left = torch.arange(4).to(dtype)
        right = (torch.arange(4) + 1).to(dtype)
        expected = torch.vdot(left, right)
        got = torch.vdot(left.to("mps"), right.to("mps"))
        torch.mps.synchronize()
        if got.device.type != "mps" or got.dtype != expected.dtype or not torch.equal(got.cpu(), expected):
            raise AssertionError(f"vdot mismatch for {dtype}")

    out = torch.empty(1, device="mps")
    returned = torch.vdot(
        torch.arange(4, dtype=torch.float32, device="mps"),
        torch.arange(4, dtype=torch.float32, device="mps"),
        out=out,
    )
    torch.mps.synchronize()
    if returned.data_ptr() != out.data_ptr() or tuple(out.shape) != ():
        raise AssertionError("vdot.out did not return and resize scalar out")
    if not torch.equal(out.cpu(), torch.tensor(14.0)):
        raise AssertionError("vdot.out mismatch")

    for bad_call in (
        lambda: torch.vdot(
            torch.ones(2, 2, device="mps"),
            torch.ones(2, 2, device="mps"),
        ),
        lambda: torch.vdot(
            torch.tensor([True, False], device="mps"),
            torch.tensor([True, False], device="mps"),
        ),
        lambda: torch.vdot(
            torch.tensor([1 + 2j], dtype=torch.complex64, device="mps"),
            torch.tensor([1 + 0j], dtype=torch.complex64, device="mps"),
        ),
        lambda: torch.vdot(
            torch.arange(4, dtype=torch.float32, device="mps"),
            torch.arange(4, dtype=torch.float32, device="mps"),
            out=torch.empty((), dtype=torch.float16, device="mps"),
        ),
    ):
        try:
            bad_call()
        except (RuntimeError, NotImplementedError):
            pass
        else:
            raise AssertionError("vdot guard did not fire")

    print(f"logspace/mvlgamma/vdot: ok installed={install_result.installed}")


def check_frexp(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {
        "aten::frexp.Tensor",
        "aten::frexp.Tensor_out",
    }
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"frexp routes missing: {missing}")

    frexp_values = torch.tensor(
        [
            -float("inf"),
            -65504.0,
            -1024.0,
            -4.0,
            -3.0,
            -2.0,
            -1.0,
            -0.5,
            -0.300048828125,
            -0.0,
            0.0,
            0.300048828125,
            0.5,
            1.0,
            2.0,
            3.0,
            4.0,
            1024.0,
            65504.0,
            float("inf"),
            float("nan"),
        ]
    )
    for dtype in (torch.float16, torch.bfloat16, torch.float32):
        values = frexp_values.to(dtype)
        if dtype == torch.float32:
            subnormal_bits = torch.tensor(
                [1, 2, 3, 71362, 0x007FFFFF, 0x00800000, -2147412286],
                dtype=torch.int32,
            )
            values = torch.cat([subnormal_bits.view(torch.float32), values])
        elif dtype == torch.float16:
            subnormal_bits = torch.tensor(
                [1, 2, 0x03FF, 0x0400, -32767, -31745],
                dtype=torch.int16,
            )
            values = torch.cat(
                [
                    subnormal_bits.view(torch.float16),
                    torch.tensor([2**-24, -(2**-24)], dtype=torch.float16),
                    values,
                ]
            )
        elif dtype == torch.bfloat16:
            subnormal_bits = torch.tensor(
                [1, 2, 0x007F, 0x0080, -32767, -32641],
                dtype=torch.int16,
            )
            values = torch.cat([subnormal_bits.view(torch.bfloat16), values])

        expected_mantissa, expected_exponent = torch.frexp(values)
        got_mantissa, got_exponent = torch.frexp(values.to("mps"))
        torch.mps.synchronize()
        if got_mantissa.dtype != dtype or got_exponent.dtype != torch.int32:
            raise AssertionError("frexp returned wrong dtypes")
        if not torch.allclose(
            got_mantissa.cpu().float(),
            expected_mantissa.float(),
            atol=1e-3,
            rtol=1e-3,
            equal_nan=True,
        ):
            raise AssertionError(f"frexp mantissa mismatch for {dtype}")
        if not torch.equal(got_exponent.cpu(), expected_exponent):
            raise AssertionError(f"frexp exponent mismatch for {dtype}")
        if dtype == torch.float32:
            expected_boundary_exponents = torch.tensor(
                [-148, -147, -147, -132, -126, -125, -132],
                dtype=torch.int32,
            )
            if not torch.equal(got_exponent.cpu()[:7], expected_boundary_exponents):
                raise AssertionError("frexp float32 subnormal exponent mismatch")
        elif dtype == torch.float16:
            expected_boundary_exponents = torch.tensor(
                [-23, -22, -14, -13, -23, -14],
                dtype=torch.int32,
            )
            if not torch.equal(got_exponent.cpu()[:6], expected_boundary_exponents):
                raise AssertionError("frexp float16 subnormal exponent mismatch")
        elif dtype == torch.bfloat16:
            expected_boundary_exponents = torch.tensor(
                [-132, -131, -126, -125, -132, -126],
                dtype=torch.int32,
            )
            if not torch.equal(got_exponent.cpu()[:6], expected_boundary_exponents):
                raise AssertionError("frexp bfloat16 subnormal exponent mismatch")
        if not torch.equal(torch.signbit(got_mantissa.cpu()), torch.signbit(expected_mantissa)):
            raise AssertionError(f"frexp signbit mismatch for {dtype}")

        finite_nonzero = torch.isfinite(expected_mantissa) & (values != 0)
        if finite_nonzero.any():
            got_abs = got_mantissa.cpu().abs().float()
            if not torch.all((got_abs[finite_nonzero] >= 0.5) & (got_abs[finite_nonzero] < 1.0)):
                raise AssertionError(f"frexp mantissa range mismatch for {dtype}")
            reconstructed = torch.ldexp(got_mantissa.float(), got_exponent).cpu()
            if not torch.allclose(
                reconstructed[finite_nonzero].float(),
                values[finite_nonzero].float(),
                atol=1e-3,
                rtol=1e-3,
            ):
                raise AssertionError(f"frexp reconstruction mismatch for {dtype}")

    scalar_mantissa, scalar_exponent = torch.frexp(torch.tensor(4.0, device="mps"))
    torch.mps.synchronize()
    if scalar_mantissa.shape != () or scalar_exponent.shape != ():
        raise AssertionError("frexp scalar shape mismatch")
    if scalar_mantissa.cpu().item() != 0.5 or scalar_exponent.cpu().item() != 3:
        raise AssertionError("frexp scalar mismatch")

    empty_mantissa, empty_exponent = torch.frexp(torch.empty(0, dtype=torch.float16, device="mps"))
    torch.mps.synchronize()
    if empty_mantissa.shape != (0,) or empty_exponent.shape != (0,):
        raise AssertionError("frexp empty tensor mismatch")

    strided = torch.arange(12, dtype=torch.float32, device="mps").reshape(3, 4)[:, ::2]
    expected_strided = torch.frexp(strided.cpu())
    got_strided = torch.frexp(strided)
    torch.mps.synchronize()
    if not torch.equal(got_strided[0].cpu(), expected_strided[0]) or not torch.equal(
        got_strided[1].cpu(),
        expected_strided[1],
    ):
        raise AssertionError("frexp non-contiguous input mismatch")

    frexp_source = torch.tensor([1.0, 2.0, 3.0], device="mps")
    mantissa_out = torch.empty(0, device="mps")
    exponent_out = torch.empty(0, dtype=torch.int32, device="mps")
    returned_mantissa, returned_exponent = torch.frexp(
        frexp_source,
        out=(mantissa_out, exponent_out),
    )
    torch.mps.synchronize()
    if (
        returned_mantissa.data_ptr() != mantissa_out.data_ptr()
        or returned_exponent.data_ptr() != exponent_out.data_ptr()
        or tuple(mantissa_out.shape) != (3,)
        or tuple(exponent_out.shape) != (3,)
    ):
        raise AssertionError("frexp.out did not return/resize outputs")
    expected_mantissa, expected_exponent = torch.frexp(frexp_source.cpu())
    if not torch.equal(mantissa_out.cpu(), expected_mantissa) or not torch.equal(
        exponent_out.cpu(),
        expected_exponent,
    ):
        raise AssertionError("frexp.out mismatch")

    mantissa_buffer = torch.empty(3, 2, device="mps")
    exponent_buffer = torch.empty(3, 2, dtype=torch.int32, device="mps")
    mantissa_view = mantissa_buffer[:, 0]
    exponent_view = exponent_buffer[:, 1]
    returned_mantissa, returned_exponent = torch.frexp(
        frexp_source,
        out=(mantissa_view, exponent_view),
    )
    torch.mps.synchronize()
    if returned_mantissa.data_ptr() != mantissa_view.data_ptr() or (
        returned_exponent.data_ptr() != exponent_view.data_ptr()
    ):
        raise AssertionError("frexp.out non-contiguous outputs did not return views")
    if not torch.equal(mantissa_view.cpu(), expected_mantissa) or not torch.equal(
        exponent_view.cpu(),
        expected_exponent,
    ):
        raise AssertionError("frexp.out non-contiguous output mismatch")

    alias_source = torch.tensor([1.0, 2.0, 3.0], device="mps")
    alias_exponent = torch.empty(0, dtype=torch.int32, device="mps")
    alias_mantissa, _ = torch.frexp(alias_source, out=(alias_source, alias_exponent))
    torch.mps.synchronize()
    if alias_mantissa.data_ptr() != alias_source.data_ptr() or not torch.equal(
        alias_source.cpu(),
        expected_mantissa,
    ):
        raise AssertionError("frexp.out exact input alias mismatch")

    grad_source = torch.tensor([1.0, 2.0, 3.0], device="mps", requires_grad=True)
    grad_mantissa, _ = torch.frexp(grad_source)
    grad_mantissa.sum().backward()
    torch.mps.synchronize()
    if not torch.allclose(
        grad_source.grad.cpu(),
        torch.tensor([0.5, 0.25, 0.25]),
        atol=1e-6,
        rtol=1e-6,
    ):
        raise AssertionError("frexp backward mismatch")

    for bad_frexp in (
        lambda: torch.frexp(torch.tensor([1, 2], dtype=torch.int64, device="mps")),
        lambda: torch.frexp(torch.tensor([True, False], device="mps")),
        lambda: torch.frexp(torch.tensor([1 + 0j], dtype=torch.complex64, device="mps")),
        lambda: torch.frexp(
            frexp_source,
            out=(torch.empty(0, dtype=torch.float16, device="mps"), exponent_out),
        ),
        lambda: torch.frexp(
            frexp_source,
            out=(mantissa_out, torch.empty(0, dtype=torch.int64, device="mps")),
        ),
        lambda: torch.frexp(
            frexp_source,
            out=(torch.empty(1, device="mps").expand(3), exponent_out),
        ),
    ):
        try:
            bad_frexp()
        except (RuntimeError, NotImplementedError):
            pass
        else:
            raise AssertionError("frexp guard did not fire")

    print(f"frexp: ok installed={install_result.installed}")


def check_geqrf(install_mps_compat_kernels) -> None:
    install_result = install_mps_compat_kernels()
    expected_routes = {"aten::geqrf.default"}
    seen_routes = (
        set(install_result.installed)
        | set(install_result.already_registered)
        | set(install_result.skipped_existing_mps)
    )
    missing = expected_routes - seen_routes
    if missing:
        raise AssertionError(f"geqrf routes missing: {missing}")

    torch.manual_seed(101)
    non_contiguous = torch.randn(5, 4).t()
    cases = [
        torch.zeros(4, 3),
        torch.eye(4, 3),
        torch.tensor([[1.0, 0.0], [0.0, 0.0], [0.0, 0.0]]),
        torch.tensor([[-1.0, 0.0], [0.0, 0.0], [0.0, 0.0]]),
        torch.tensor([[0.0, 0.0], [2.0, 0.0], [0.0, 0.0]]),
        torch.tensor([[0.0], [3.0], [4.0]]),
        torch.tensor([[5.0], [0.0], [0.0]]),
        torch.tensor([[-5.0], [0.0], [0.0]]),
        torch.randn(1, 1),
        torch.randn(1, 4),
        torch.randn(4, 1),
        torch.randn(2, 1),
        torch.randn(1, 2),
        torch.randn(3, 2),
        torch.randn(2, 3),
        torch.randn(4, 4),
        torch.randn(2, 3, 2),
        torch.tensor([
            [[1.0, 2.0], [0.0, 3.0], [0.0, 4.0]],
            [[1.0, 2.0], [5.0, 3.0], [6.0, 4.0]],
        ]),
        torch.randn(16, 8),
        torch.randn(8, 16),
        torch.randn(2, 16, 8),
        torch.tensor([[1e20, 2.0], [1e20, 3.0], [1e20, 4.0]]),
        torch.tensor([[-1e20, 2.0], [1e20, 3.0], [-1e20, 4.0]]),
        non_contiguous,
        torch.empty(0, 3),
        torch.empty(3, 0),
    ]
    if non_contiguous.is_contiguous():
        raise AssertionError("geqrf non-contiguous fixture is unexpectedly contiguous")
    for index, source in enumerate(cases):
        expected_a, expected_tau = torch.geqrf(source)
        mps_source = source.to("mps")
        if source is non_contiguous and mps_source.is_contiguous():
            raise AssertionError("geqrf non-contiguous MPS fixture was materialized as contiguous")
        got_a, got_tau = torch.geqrf(mps_source)
        torch.mps.synchronize()
        if got_a.device.type != "mps" or got_tau.device.type != "mps":
            raise AssertionError("geqrf returned a non-MPS tensor")
        if tuple(got_a.shape) != tuple(expected_a.shape) or tuple(got_tau.shape) != tuple(expected_tau.shape):
            raise AssertionError(f"geqrf shape mismatch for case {index}")
        if got_a.numel() and not torch.allclose(
            got_a.cpu(),
            expected_a,
            atol=2e-5,
            rtol=2e-5,
        ):
            diff = (got_a.cpu() - expected_a).abs().max().item()
            raise AssertionError(f"geqrf packed matrix mismatch for case {index}: {diff}")
        if got_tau.numel() and not torch.allclose(
            got_tau.cpu(),
            expected_tau,
            atol=2e-5,
            rtol=2e-5,
        ):
            diff = (got_tau.cpu() - expected_tau).abs().max().item()
            raise AssertionError(f"geqrf tau mismatch for case {index}: {diff}")

        rows, cols = source.shape[-2:]
        rank = min(rows, cols)
        if rows >= cols and rank > 0:
            q = torch.orgqr(got_a, got_tau)
            r = torch.triu(got_a[..., :rank, :])
            reconstructed = q.float() @ r.float()
            torch.mps.synchronize()
            if not torch.allclose(
                reconstructed.cpu(),
                source.float(),
                atol=2e-5,
                rtol=2e-5,
            ):
                diff = (reconstructed.cpu() - source.float()).abs().max().item()
                raise AssertionError(f"geqrf reconstruction mismatch for case {index}: {diff}")

    for nonfinite_source in (
        torch.tensor([[float("nan"), 1.0], [2.0, 3.0]]),
        torch.tensor([[float("inf"), 1.0], [2.0, 3.0]]),
        torch.tensor([[float("-inf"), 1.0], [2.0, 3.0]]),
    ):
        got_a, got_tau = torch.geqrf(nonfinite_source.to("mps"))
        torch.mps.synchronize()
        if got_a.device.type != "mps" or got_tau.device.type != "mps":
            raise AssertionError("geqrf non-finite case returned a non-MPS tensor")
        if tuple(got_a.shape) != tuple(nonfinite_source.shape) or tuple(got_tau.shape) != (2,):
            raise AssertionError("geqrf non-finite case returned unexpected shapes")

    grad_source = torch.randn(3, 2, device="mps", requires_grad=True)
    grad_a, grad_tau = torch.geqrf(grad_source)
    try:
        (grad_a.sum() + grad_tau.sum()).backward()
    except NotImplementedError:
        pass
    else:
        raise AssertionError("geqrf backward unexpectedly succeeded")

    for bad_geqrf in (
        lambda: torch.geqrf(torch.randn(3, 2, dtype=torch.float16, device="mps")),
        lambda: torch.geqrf(torch.randn(3, 2, dtype=torch.complex64, device="mps")),
        lambda: torch.geqrf(torch.randn(3, device="mps")),
    ):
        try:
            bad_geqrf()
        except (RuntimeError, NotImplementedError):
            pass
        else:
            raise AssertionError("geqrf guard did not fire")

    print(f"geqrf: ok installed={install_result.installed}")


def check_experimental_approx_svd(approximate_linalg_svd_mps, iterations: int) -> None:
    matrix = torch.randn(5, 3, device="mps")
    u, s, vh = approximate_linalg_svd_mps(
        matrix,
        full_matrices=False,
        iterations=iterations,
    )
    torch.mps.synchronize()
    reconstructed = u.float() @ torch.diag(s.float()).to("mps") @ vh.float()
    error = (reconstructed - matrix.float()).norm().item()
    if any(t.device.type != "mps" for t in (u, s, vh)):
        raise AssertionError("approx_svd returned a non-MPS tensor")
    if error > 1e-3:
        raise AssertionError(f"approx_svd reconstruction error too high: {error}")
    print(f"experimental_approx_svd: ok reconstruction_norm={error:.3g}")


def check_matrix_exp_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        x = torch.randn(3, 3, device="mps")
        torch.linalg.matrix_exp(x)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("matrix_exp baseline: native MPS support present; compat skip is ok")
        return
    print("matrix_exp baseline: fails without compat as expected")


def matrix_exp_cases(dtype: torch.dtype) -> list[torch.Tensor]:
    return [
        torch.zeros(1, 1, dtype=dtype),
        torch.eye(2, dtype=dtype),
        torch.diag(torch.tensor([0.5, -1.0, 2.0], dtype=dtype)),
        torch.tensor([[0.0, 1.0], [-1.0, 0.0]], dtype=dtype),
        0.1 * torch.randn(3, 3, dtype=dtype),
        torch.randn(4, 4, dtype=dtype),
        2.0 * torch.randn(3, 3, dtype=dtype),
        torch.randn(2, 3, 3, dtype=dtype),
        torch.randn(2, 1, 4, 4, dtype=dtype),
    ]


def assert_close(name: str, got: torch.Tensor, expected: torch.Tensor, dtype: torch.dtype) -> None:
    tolerance = 5e-4 if dtype == torch.float32 else 8e-2
    diff = (got.cpu().float() - expected.float()).abs().max().item()
    scale = expected.float().abs().max().item()
    relative = diff / max(scale, 1.0)
    if diff > tolerance and relative > tolerance:
        raise AssertionError(f"{name} diff too high for {dtype}: abs={diff} rel={relative}")


def check_matrix_exp_helper(matrix_exp_mps) -> None:
    torch.manual_seed(7)
    for dtype in (torch.float32, torch.float16):
        for index, source in enumerate(matrix_exp_cases(dtype)):
            expected = torch.linalg.matrix_exp(source.float()).to(dtype)
            got = matrix_exp_mps(source.to("mps"))
            torch.mps.synchronize()
            if got.device.type != "mps":
                raise AssertionError(f"matrix_exp helper returned {got.device}, expected mps")
            assert_close(f"matrix_exp helper case {index}", got, expected, dtype)
    print("matrix_exp helper: ok")


def check_matrix_exp_registered(install_mps_compat_kernels) -> None:
    os.environ["PSYCHE_MPS_COMPAT_MATRIX_EXP"] = "1"
    install_result = install_mps_compat_kernels()
    if (
        "aten::linalg_matrix_exp.default" not in install_result.installed
        and "aten::linalg_matrix_exp.default" not in install_result.already_registered
        and "aten::linalg_matrix_exp.default" not in install_result.skipped_existing_mps
    ):
        raise AssertionError(f"matrix_exp did not install or skip cleanly: {install_result}")

    torch.manual_seed(11)
    for dtype in (torch.float32, torch.float16):
        source = torch.randn(3, 3, dtype=dtype)
        expected = torch.linalg.matrix_exp(source.float()).to(dtype)
        got = torch.linalg.matrix_exp(source.to("mps"))
        torch.mps.synchronize()
        assert_close("matrix_exp registered default", got, expected, dtype)

    direct_source = torch.randn(2, 2)
    direct_expected = torch.ops.aten.linalg_matrix_exp.default(direct_source)
    direct_got = torch.ops.aten.linalg_matrix_exp.default(direct_source.to("mps"))
    direct_out = torch.empty_like(direct_source, device="mps")
    direct_returned = torch.ops.aten.linalg_matrix_exp.out(
        direct_source.to("mps"),
        out=direct_out,
    )
    torch.mps.synchronize()
    assert_close("matrix_exp direct default", direct_got, direct_expected, torch.float32)
    assert_close("matrix_exp direct out", direct_returned, direct_expected, torch.float32)
    print(f"matrix_exp registered: ok installed={install_result.installed}")


def check_matrix_exp_backward(install_mps_compat_kernels) -> None:
    os.environ["PSYCHE_MPS_COMPAT_MATRIX_EXP"] = "1"
    install_mps_compat_kernels()
    torch.manual_seed(13)
    source_cpu = (0.1 * torch.randn(3, 3)).requires_grad_(True)
    source_mps = source_cpu.detach().clone().to("mps").requires_grad_(True)
    expected = torch.linalg.matrix_exp(source_cpu)
    got = torch.linalg.matrix_exp(source_mps)
    grad = torch.randn_like(expected)
    expected.backward(grad)
    got.backward(grad.to("mps"))
    torch.mps.synchronize()
    if source_mps.grad is None or source_mps.grad.device.type != "mps":
        raise AssertionError("matrix_exp backward did not produce an MPS grad")
    max_out_diff = (got.detach().cpu() - expected.detach()).abs().max().item()
    max_grad_diff = (source_mps.grad.cpu() - source_cpu.grad).abs().max().item()
    if max_out_diff > 5e-4:
        raise AssertionError(f"matrix_exp backward output diff too high: {max_out_diff}")
    if max_grad_diff > 2e-3:
        raise AssertionError(f"matrix_exp backward grad diff too high: {max_grad_diff}")
    print(f"matrix_exp backward: ok out_diff={max_out_diff:.3g} grad_diff={max_grad_diff:.3g}")


def check_qr_baseline_fails() -> None:
    result = run_python_snippet(
        """
        import torch

        x = torch.randn(4, 3, device="mps")
        torch.linalg.qr(x)
        torch.mps.synchronize()
        """
    )
    if result.returncode == 0:
        print("qr baseline: native MPS support present; compat skip is ok")
        return
    print("qr baseline: fails without compat as expected")


def assert_qr_valid(
    name: str,
    source: torch.Tensor,
    q: torch.Tensor,
    r: torch.Tensor,
    mode: str,
    tolerance: float | None = None,
) -> None:
    if tolerance is None:
        tolerance = 5e-2 if source.dtype in (torch.float16, torch.bfloat16) else 2e-4
    rows, cols = source.shape[-2:]
    rank = min(rows, cols)
    expected_q_shape = {
        "reduced": source.shape[:-2] + (rows, rank),
        "complete": source.shape[:-2] + (rows, rows),
        "r": (0,),
    }[mode]
    expected_r_shape = {
        "reduced": source.shape[:-2] + (rank, cols),
        "complete": source.shape[:-2] + (rows, cols),
        "r": source.shape[:-2] + (rank, cols),
    }[mode]
    if tuple(q.shape) != expected_q_shape or tuple(r.shape) != expected_r_shape:
        raise AssertionError(
            f"{name} shape mismatch: got Q={tuple(q.shape)} R={tuple(r.shape)}, "
            f"expected Q={expected_q_shape} R={expected_r_shape}"
        )
    if q.device.type != "mps" or r.device.type != "mps":
        raise AssertionError(f"{name} returned Q={q.device}, R={r.device}, expected MPS")
    if q.dtype != source.dtype or r.dtype != source.dtype:
        raise AssertionError(
            f"{name} dtype mismatch: got Q={q.dtype} R={r.dtype}, expected {source.dtype}"
        )
    source_gram = (source.float().transpose(-2, -1) @ source.float()).cpu()
    r_gram = (r.float().transpose(-2, -1) @ r.float()).cpu()
    r_gram_error = (r_gram - source_gram).abs().max().item()
    if r_gram_error > tolerance:
        raise AssertionError(f"{name} R^T R error too high: {r_gram_error}")
    if mode == "r":
        return

    reconstructed = (q.float() @ r.float()).cpu()
    reconstruction_error = (reconstructed - source.float()).abs().max().item()
    gram = (q.float().transpose(-2, -1) @ q.float()).cpu()
    eye = torch.eye(q.shape[-1], dtype=gram.dtype).expand(gram.shape)
    orthogonality_error = (gram - eye).abs().max().item()
    if reconstruction_error > tolerance:
        raise AssertionError(f"{name} reconstruction error too high: {reconstruction_error}")
    if orthogonality_error > tolerance:
        raise AssertionError(f"{name} orthogonality error too high: {orthogonality_error}")


def check_qr_helper(qr_mps) -> None:
    torch.manual_seed(17)

    def assert_qr_keeps_independent_residual(
        name: str,
        source: torch.Tensor,
        *,
        min_r11: float,
        max_reconstruction_error: float,
    ) -> None:
        q, r = qr_mps(source.to("mps"), mode="reduced")
        torch.mps.synchronize()
        r_cpu = r.cpu().float()
        residual = float(r_cpu[1, 1].abs().item())
        if residual <= min_r11:
            raise AssertionError(f"{name} lost independent residual: R[1,1]={residual}")
        reconstructed = (q.float() @ r.float()).cpu()
        reconstruction_error = (reconstructed - source.float()).abs().max().item()
        if reconstruction_error > max_reconstruction_error:
            raise AssertionError(
                f"{name} reconstruction error too high: {reconstruction_error}"
            )

    cases = [
        (torch.randn(4, 3), "reduced"),
        (torch.randn(3, 5), "reduced"),
        (torch.randn(2, 4, 3), "reduced"),
        (torch.zeros(4, 3), "reduced"),
        (
            torch.tensor(
                [
                    [1.0, 1.0, 2.0],
                    [2.0, 2.0, 4.0],
                    [3.0, 3.0, 6.0],
                    [4.0, 4.0, 8.0],
                ]
            ),
            "reduced",
        ),
        (
            torch.tensor(
                [
                    [1.0, 1.0, 1e3],
                    [0.0, 1e-3, 1e3],
                    [0.0, 0.0, 1e3],
                    [0.0, 0.0, 1e3],
                ]
            ),
            "reduced",
        ),
        (torch.randn(4, 3).index_fill(1, torch.tensor([1]), 0.0), "reduced"),
        (torch.randn(4, 3), "complete"),
        (torch.randn(3, 5), "complete"),
        (torch.randn(2, 4, 3), "complete"),
        (torch.randn(4, 3), "r"),
        (torch.randn(3, 5), "r"),
    ]
    for index, (source, mode) in enumerate(cases):
        q, r = qr_mps(source.to("mps"), mode=mode)
        torch.mps.synchronize()
        assert_qr_valid(f"qr helper case {index}", source, q, r, mode)

    low_precision_cases = [torch.float16]
    try:
        torch.randn(3, 3, dtype=torch.bfloat16, device="mps")
    except TypeError:
        pass
    else:
        low_precision_cases.append(torch.bfloat16)
    for dtype in low_precision_cases:
        source = torch.randn(4, 3).to(dtype)
        q, r = qr_mps(source.to("mps"), mode="reduced")
        torch.mps.synchronize()
        assert_qr_valid(f"qr helper low precision {dtype}", source, q, r, "reduced")

    small_scale = (1e-4 * torch.randn(4, 3)).to(torch.float32)
    q, r = qr_mps(small_scale.to("mps"), mode="reduced")
    torch.mps.synchronize()
    assert_qr_valid("qr helper small-scale", small_scale, q, r, "reduced")

    independent_residual = torch.tensor(
        [
            [1e6, 1e6 + 1.0],
            [1.0, 1.0],
            [0.0, 1.0],
            [0.0, 0.0],
        ]
    )
    assert_qr_keeps_independent_residual(
        "qr helper large independent residual",
        independent_residual,
        min_r11=1e-1,
        max_reconstruction_error=1e-2,
    )
    assert_qr_keeps_independent_residual(
        "qr helper small independent residual",
        independent_residual * 1e-9,
        min_r11=1e-10,
        max_reconstruction_error=1e-12,
    )

    try:
        qr_mps(torch.randn(3, 3, dtype=torch.complex64).to("mps"))
    except NotImplementedError:
        pass
    else:
        raise AssertionError("qr helper unexpectedly accepted complex input")

    print("qr helper: ok")


def check_qr_registered(install_mps_compat_kernels) -> None:
    os.environ["PSYCHE_MPS_COMPAT_QR"] = "1"
    install_result = install_mps_compat_kernels()
    if (
        "aten::linalg_qr.default" not in install_result.installed
        and "aten::linalg_qr.default" not in install_result.already_registered
        and "aten::linalg_qr.default" not in install_result.skipped_existing_mps
    ):
        raise AssertionError(f"qr did not install or skip cleanly: {install_result}")

    torch.manual_seed(19)
    for mode in ("reduced", "complete", "r"):
        source = torch.randn(4, 3)
        q, r = torch.linalg.qr(source.to("mps"), mode=mode)
        torch.mps.synchronize()
        assert_qr_valid(f"qr registered {mode}", source, q, r, mode)

    source = torch.randn(3, 5)
    expected_q, expected_r = torch.linalg.qr(source, mode="reduced")
    q_out = torch.empty_like(expected_q, device="mps")
    r_out = torch.empty_like(expected_r, device="mps")
    returned_q, returned_r = torch.ops.aten.linalg_qr.out(
        source.to("mps"),
        "reduced",
        Q=q_out,
        R=r_out,
    )
    torch.mps.synchronize()
    if returned_q.data_ptr() != q_out.data_ptr() or returned_r.data_ptr() != r_out.data_ptr():
        raise AssertionError("qr.out did not return caller outputs")
    assert_qr_valid("qr.out reduced", source, q_out, r_out, "reduced")

    wrong_q = torch.empty(1, device="mps")
    wrong_r = torch.empty(1, device="mps")
    returned_q, returned_r = torch.ops.aten.linalg_qr.out(
        source.to("mps"),
        "complete",
        Q=wrong_q,
        R=wrong_r,
    )
    torch.mps.synchronize()
    if returned_q.data_ptr() != wrong_q.data_ptr() or returned_r.data_ptr() != wrong_r.data_ptr():
        raise AssertionError("qr.out resized path did not return caller outputs")
    assert_qr_valid("qr.out resized complete", source, wrong_q, wrong_r, "complete")

    try:
        torch.ops.aten.linalg_qr.out(
            source.to("mps"),
            "reduced",
            Q=torch.empty_like(expected_q),
            R=torch.empty_like(expected_r, device="mps"),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("qr.out unexpectedly accepted CPU Q output")

    try:
        torch.ops.aten.linalg_qr.out(
            source.to("mps"),
            "reduced",
            Q=torch.empty_like(expected_q, dtype=torch.float16, device="mps"),
            R=torch.empty_like(expected_r, device="mps"),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("qr.out unexpectedly accepted dtype-mismatched Q output")

    noncontiguous_q_base = torch.empty(expected_q.shape[0], expected_q.shape[1], 2, device="mps")
    noncontiguous_q = noncontiguous_q_base[:, :, 0]
    noncontiguous_r_base = torch.empty(expected_r.shape[0], expected_r.shape[1], 2, device="mps")
    noncontiguous_r = noncontiguous_r_base[:, :, 0]
    torch.ops.aten.linalg_qr.out(
        source.to("mps"),
        "reduced",
        Q=noncontiguous_q,
        R=noncontiguous_r,
    )
    torch.mps.synchronize()
    assert_qr_valid("qr.out non-contiguous", source, noncontiguous_q, noncontiguous_r, "reduced")
    print(f"qr registered: ok installed={install_result.installed}")


def check_qr_backward(install_mps_compat_kernels) -> None:
    os.environ["PSYCHE_MPS_COMPAT_QR"] = "1"
    install_mps_compat_kernels()
    torch.manual_seed(23)
    source = torch.randn(4, 3)
    source_mps = source.to("mps").requires_grad_(True)
    q, r = torch.linalg.qr(source_mps, mode="reduced")
    reconstructed = q.float() @ r.float()
    loss = reconstructed.pow(2).sum()
    loss.backward()
    torch.mps.synchronize()
    if source_mps.grad is None or source_mps.grad.device.type != "mps":
        raise AssertionError("qr backward did not produce an MPS grad")
    expected_grad = 2.0 * source
    max_grad_diff = (source_mps.grad.cpu() - expected_grad).abs().max().item()
    if max_grad_diff > 2e-3:
        raise AssertionError(f"qr backward reconstruction grad diff too high: {max_grad_diff}")
    print(f"qr backward: ok grad_diff={max_grad_diff:.3g}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--experimental-approx-svd",
        "--approx-svd",
        action="store_true",
        help="Also check the explicit experimental approximate SVD helper.",
    )
    parser.add_argument(
        "--matrix-exp",
        action="store_true",
        help="Also check the gated experimental matrix_exp compatibility route.",
    )
    parser.add_argument(
        "--qr",
        action="store_true",
        help="Also check the gated experimental QR compatibility route.",
    )
    parser.add_argument("--svd-iterations", type=int, default=64)
    args = parser.parse_args()

    assert_mps_available()
    (
        install_mps_compat_kernels,
        approximate_linalg_svd_mps,
        matrix_exp_mps,
        qr_mps,
    ) = load_mps_compat()
    check_adaptive_avg_pool3d_schema()
    check_heaviside_schema()
    check_gcd_schema()
    check_lcm_schema()
    check_reduction_out_schema()
    check_take_schema()
    check_logit_inplace_schema()
    check_addmm_activation_schema()
    check_channel_shuffle_schema()
    check_logspace_mvlgamma_vdot_schema()
    check_geqrf_schema()
    check_adaptive_avg_pool3d_baseline_fails()
    check_heaviside_baseline_fails()
    check_gcd_baseline_fails()
    check_lcm_baseline_fails()
    check_reduction_out_baseline_fails()
    check_take_baseline_fails()
    check_logit_inplace_baseline_fails()
    check_addmm_activation_baseline_fails()
    check_channel_shuffle_baseline_fails()
    check_logspace_mvlgamma_vdot_baseline_fails()
    check_geqrf_baseline_fails()
    check_adaptive_avg_pool3d(install_mps_compat_kernels)
    check_adaptive_avg_pool3d_backward(install_mps_compat_kernels)
    check_adaptive_avg_pool3d_backward_direct(install_mps_compat_kernels)
    check_heaviside(install_mps_compat_kernels)
    check_gcd(install_mps_compat_kernels)
    check_lcm(install_mps_compat_kernels)
    check_reduction_out(install_mps_compat_kernels)
    check_take(install_mps_compat_kernels)
    check_logit_inplace(install_mps_compat_kernels)
    check_addmm_activation(install_mps_compat_kernels)
    check_channel_shuffle(install_mps_compat_kernels)
    check_logspace_mvlgamma_vdot(install_mps_compat_kernels)
    check_frexp(install_mps_compat_kernels)
    check_geqrf(install_mps_compat_kernels)
    if args.matrix_exp:
        check_matrix_exp_schema()
        check_matrix_exp_baseline_fails()
        check_matrix_exp_helper(matrix_exp_mps)
        check_matrix_exp_registered(install_mps_compat_kernels)
        check_matrix_exp_backward(install_mps_compat_kernels)
    if args.qr:
        check_qr_schema()
        check_qr_baseline_fails()
        check_qr_helper(qr_mps)
        check_qr_registered(install_mps_compat_kernels)
        check_qr_backward(install_mps_compat_kernels)
    if args.experimental_approx_svd:
        check_experimental_approx_svd(approximate_linalg_svd_mps, args.svd_iterations)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
