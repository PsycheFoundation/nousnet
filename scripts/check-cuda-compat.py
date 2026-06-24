#!/usr/bin/env python3
"""
Smoke-check Psyche's opt-in Apple Silicon CUDA compatibility resolver.

Run on Apple Silicon:

    PSYCHE_CUDA_COMPAT=1 scripts/check-cuda-compat.py
"""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import torch


def load_cuda_compat():
    try:
        from psyche.cuda_compat import (
            resolve_device,
            resolve_device_with_status,
        )

        return resolve_device, resolve_device_with_status
    except ModuleNotFoundError as exc:
        print(f"package import unavailable ({exc}); loading cuda_compat.py directly")
        module_path = (
            Path(__file__).resolve().parents[1] / "python/python/psyche/cuda_compat.py"
        )
        spec = importlib.util.spec_from_file_location(
            "psyche_cuda_compat_check",
            module_path,
        )
        module = importlib.util.module_from_spec(spec)
        assert spec and spec.loader
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return (
            module.resolve_device,
            module.resolve_device_with_status,
        )


def assert_mps_available() -> None:
    if not torch.backends.mps.is_available():
        raise SystemExit("MPS is not available in this Python/PyTorch environment")


def check_cuda_compat_enables_mps_routes() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    child = r"""
import importlib.util
import os
import sys
from pathlib import Path

import torch

module_path = Path(os.environ["PSYCHE_REPO_ROOT"]) / "python/python/psyche/mps_compat.py"
spec = importlib.util.spec_from_file_location("psyche_mps_compat_cuda_check", module_path)
module = importlib.util.module_from_spec(spec)
assert spec and spec.loader
sys.modules[spec.name] = module
spec.loader.exec_module(module)

with module.mps_compat_context(torch.device("mps")):
    result = torch.take(
        torch.arange(6, device="mps"),
        torch.tensor([1, 4], device="mps"),
    )
    torch.mps.synchronize()

print(result.cpu().tolist())
"""

    env = os.environ.copy()
    env["PSYCHE_REPO_ROOT"] = str(repo_root)
    env["PYTORCH_ENABLE_MPS_FALLBACK"] = "0"
    env["PSYCHE_CUDA_COMPAT"] = "1"
    env.pop("PSYCHE_MPS_COMPAT", None)
    env.pop("PSYCHE_CUDA_COMPAT_MPS_ROUTES", None)
    enabled = subprocess.run(
        [sys.executable, "-c", child],
        cwd=repo_root,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    if enabled.returncode != 0:
        sys.stdout.write(enabled.stdout)
        sys.stderr.write(enabled.stderr)
        raise AssertionError("PSYCHE_CUDA_COMPAT did not enable exact MPS compat routes")
    if "[1, 4]" not in enabled.stdout:
        raise AssertionError(f"unexpected MPS route output: {enabled.stdout!r}")

    for disabled_value in ("0", "false", "no", "off"):
        disabled_env = dict(env)
        disabled_env["PSYCHE_CUDA_COMPAT_MPS_ROUTES"] = disabled_value
        disabled = subprocess.run(
            [sys.executable, "-c", child],
            cwd=repo_root,
            env=disabled_env,
            text=True,
            capture_output=True,
            check=False,
        )
        if disabled.returncode == 0:
            raise AssertionError(
                "PSYCHE_CUDA_COMPAT_MPS_ROUTES="
                f"{disabled_value!r} unexpectedly allowed the MPS take route"
            )
        error_text = disabled.stderr + disabled.stdout
        if "aten::take" not in error_text and "not currently implemented" not in error_text:
            raise AssertionError(
                "unexpected opt-out failure output for "
                f"{disabled_value!r}: {error_text}"
            )

    compat_disabled_env = dict(env)
    compat_disabled_env["PSYCHE_CUDA_COMPAT"] = "0"
    compat_disabled = subprocess.run(
        [sys.executable, "-c", child],
        cwd=repo_root,
        env=compat_disabled_env,
        text=True,
        capture_output=True,
        check=False,
    )
    if compat_disabled.returncode == 0:
        raise AssertionError("PSYCHE_CUDA_COMPAT=0 unexpectedly enabled MPS routes")
    error_text = compat_disabled.stderr + compat_disabled.stdout
    if "aten::take" not in error_text and "not currently implemented" not in error_text:
        raise AssertionError(f"unexpected disabled-compat output: {error_text}")

    print("CUDA compat enables exact MPS routes, with opt-outs: ok")


def main() -> int:
    assert_mps_available()
    resolve_device, resolve_device_with_status = load_cuda_compat()

    original_cuda_available = torch.cuda.is_available
    original_tensor_cuda = torch.Tensor.cuda
    original_module_cuda = torch.nn.Module.cuda

    os.environ.pop("PSYCHE_CUDA_COMPAT", None)
    disabled = resolve_device_with_status("cuda")
    if disabled.status != "disabled" or disabled.resolved.type != "cuda":
        raise AssertionError(f"disabled compat resolved incorrectly: {disabled}")
    for request in ("cpu", "mps", torch.device("cpu"), torch.device("mps")):
        status = resolve_device_with_status(request)
        if status.status != "not-cuda-request" or status.resolved.type != torch.device(request).type:
            raise AssertionError(f"{request!r} should pass through unchanged, got {status}")
    try:
        resolve_device(None)
    except ValueError:
        print("None is not treated as CUDA-shaped intent: ok")
    else:
        raise AssertionError("None unexpectedly resolved as a CUDA-shaped device")

    os.environ["PSYCHE_CUDA_COMPAT"] = "1"
    for request in (0, "cuda", "cuda:0", torch.device("cuda:0")):
        status = resolve_device_with_status(request)
        if status.resolved.type != "mps" or not status.redirected:
            raise AssertionError(f"{request!r} resolved as {status}, expected MPS redirect")
    print("device resolution: ok")
    check_cuda_compat_enables_mps_routes()

    if torch.cuda.is_available is not original_cuda_available:
        raise AssertionError("torch.cuda.is_available was monkeypatched")
    if torch.cuda.is_available():
        raise AssertionError("CUDA compatibility must not spoof torch.cuda.is_available()")
    if torch.cuda.device_count() != 0:
        raise AssertionError("CUDA compatibility must not spoof torch.cuda.device_count()")
    if torch.Tensor.cuda is not original_tensor_cuda:
        raise AssertionError("Tensor.cuda was monkeypatched")
    if torch.nn.Module.cuda is not original_module_cuda:
        raise AssertionError("Module.cuda was monkeypatched")
    print("torch.cuda remains honest: ok")

    for name, callback in {
        "Tensor.cuda()": lambda: torch.ones(2).cuda(),
        "Tensor.to('cuda')": lambda: torch.ones(2).to("cuda"),
        "Module.cuda()": lambda: torch.nn.Linear(2, 2).cuda(),
        "Module.to('cuda')": lambda: torch.nn.Linear(2, 2).to("cuda"),
        "torch.empty(device='cuda')": lambda: torch.empty(2, device="cuda"),
    }.items():
        try:
            callback()
        except Exception:
            print(f"{name} remains real CUDA and unsupported on this machine: ok")
        else:
            raise AssertionError(f"{name} unexpectedly succeeded")

    # Deliberately not patched. This avoids lying to code that needs true CUDA tensors.
    if torch.device("cuda").type != "cuda":
        raise AssertionError("torch.device was unexpectedly monkeypatched")
    if torch.ones(1, device="mps").is_cuda:
        raise AssertionError("MPS tensors must not claim tensor.is_cuda")
    print("torch.device remains honest: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
