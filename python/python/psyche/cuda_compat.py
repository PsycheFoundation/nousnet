from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Literal

import torch


CudaCompatResolution = Literal[
    "real-cuda",
    "redirected-to-mps",
    "mps-unavailable",
    "disabled",
    "not-cuda-request",
]


def _env_enabled(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {
        "1",
        "true",
        "yes",
        "on",
        "force",
    }


def cuda_compat_enabled() -> bool:
    return _env_enabled("PSYCHE_CUDA_COMPAT")


def mps_available() -> bool:
    return bool(
        hasattr(torch.backends, "mps")
        and torch.backends.mps.is_built()
        and torch.backends.mps.is_available()
    )


def real_cuda_available() -> bool:
    try:
        return bool(torch.cuda.is_available())
    except Exception:
        return False


def is_cuda_request(device: torch.device | str | int | None) -> bool:
    if device is None:
        return False
    if isinstance(device, int):
        return True
    if isinstance(device, torch.device):
        return device.type == "cuda"
    if isinstance(device, str):
        return device == "cuda" or device.startswith("cuda:")
    return False


def _cuda_device(device: torch.device | str | int) -> torch.device:
    if isinstance(device, torch.device):
        return device
    if isinstance(device, int):
        return torch.device(f"cuda:{device}")
    return torch.device(device)


@dataclass(frozen=True)
class CudaCompatDeviceResolution:
    requested: torch.device | str | int | None
    resolved: torch.device
    status: CudaCompatResolution

    @property
    def redirected(self) -> bool:
        return self.status == "redirected-to-mps"


def resolve_device_with_status(
    device: torch.device | str | int | None,
) -> CudaCompatDeviceResolution:
    if device is None:
        raise ValueError(
            "None is not a CUDA-shaped device request. Pass an explicit device, "
            "or pass 0 at a Psyche-owned CUDA-style accelerator boundary."
        )
    if not is_cuda_request(device):
        return CudaCompatDeviceResolution(
            requested=device,
            resolved=torch.device(device),
            status="not-cuda-request",
        )
    if real_cuda_available():
        return CudaCompatDeviceResolution(
            requested=device,
            resolved=_cuda_device(device),
            status="real-cuda",
        )
    if not cuda_compat_enabled():
        return CudaCompatDeviceResolution(
            requested=device,
            resolved=_cuda_device(device),
            status="disabled",
        )
    if not mps_available():
        return CudaCompatDeviceResolution(
            requested=device,
            resolved=_cuda_device(device),
            status="mps-unavailable",
        )
    return CudaCompatDeviceResolution(
        requested=device,
        resolved=torch.device("mps"),
        status="redirected-to-mps",
    )


def should_redirect_cuda_to_mps(device: torch.device | str | int | None) -> bool:
    return resolve_device_with_status(device).redirected


def resolve_device(device: torch.device | str | int | None) -> torch.device:
    """Resolve explicit Psyche device intent without pretending MPS is CUDA.

    With `PSYCHE_CUDA_COMPAT=1`, CUDA-shaped requests are translated to a real
    `mps` device only when real CUDA is absent and MPS is available. `torch.cuda`
    itself is never monkeypatched, and MPS tensors never claim to be CUDA tensors.
    Integer devices are interpreted as Psyche-owned CUDA-style accelerator
    indices. `None` is rejected so optional-device callers do not accidentally
    widen into CUDA intent.
    """

    return resolve_device_with_status(device).resolved
