import torch

from .causal_lm import PretrainedSourceRepoFiles, PretrainedSourceStateDict, CausalLM
from ..cuda_compat import resolve_device_with_status
from typing import Optional, Iterable

# Only HfAuto has been tested with CUDA-shaped MPS redirection because it uses
# SDPA and the mps_compat_context path. Other architectures must opt in after
# explicit fallback-disabled forward/backward tests.
MPS_REDIRECT_VALIDATED_ARCHITECTURES = {"HfAuto"}


def make_causal_lm(
    architecture: str,
    source: PretrainedSourceRepoFiles | PretrainedSourceStateDict,
    device: torch.device | str | int,
    attn_implementation: str,
    dp: int = 1,
    tp: int = 1,
    override_max_position_embeddings: Optional[int] = None,
    param_dtype: torch.dtype = torch.bfloat16,
    reduce_dtype: torch.dtype = torch.float32,
    fsdp_modules: Optional[Iterable[str]] = None,
) -> CausalLM:
    device_resolution = resolve_device_with_status(device)
    device = device_resolution.resolved
    if (
        device_resolution.redirected
        and architecture not in MPS_REDIRECT_VALIDATED_ARCHITECTURES
    ):
        raise RuntimeError(
            "PSYCHE_CUDA_COMPAT redirected CUDA-shaped device intent to MPS, "
            f"but architecture={architecture!r} is not validated on MPS. "
            "Use architecture='HfAuto', request device='mps' explicitly for "
            "native MPS paths, or disable PSYCHE_CUDA_COMPAT to preserve normal "
            "CUDA failure behavior."
        )
    if architecture == "HfAuto":
        from .hf_transformers import HfTransformersAuto

        return HfTransformersAuto.from_pretrained(
            source=source,
            device=device,
            attn_implementation=attn_implementation,
            dp=dp,
            tp=tp,
            override_max_position_embeddings=override_max_position_embeddings,
            param_dtype=param_dtype,
            reduce_dtype=reduce_dtype,
            fsdp_modules=fsdp_modules,
        )
    elif architecture == "Torchtitan":
        from .ttitan import TorchtitanAuto

        return TorchtitanAuto.from_pretrained(
            source=source,
            device=device,
            attn_implementation=attn_implementation,
            dp=dp,
            tp=tp,
            override_max_position_embeddings=override_max_position_embeddings,
            param_dtype=param_dtype,
            reduce_dtype=reduce_dtype,
            fsdp_modules=fsdp_modules,
        )
    raise ValueError(f"Unknown architecture {architecture}")
