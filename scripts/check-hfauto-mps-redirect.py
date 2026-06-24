#!/usr/bin/env python3
"""
Validate Psyche's HfAuto CUDA-shaped MPS redirect with a real forward/backward.

Run on native silicon:

    PYTORCH_ENABLE_MPS_FALLBACK=0 PSYCHE_CUDA_COMPAT=1 scripts/check-hfauto-mps-redirect.py
"""

from __future__ import annotations

import json
import os
import sys
import types
from collections.abc import Mapping
from pathlib import Path

os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "0")
os.environ.setdefault("PSYCHE_CUDA_COMPAT", "1")

import torch
from transformers import AutoModelForCausalLM
from transformers.models.llama import LlamaConfig


def install_psyche_import_shell():
    # The repository package imports the compiled _psyche_ext from __init__.py.
    # This check only needs the pure-Python model factory, so it installs a
    # package shell that lets relative imports resolve without requiring a Rust
    # extension build first.
    repo_root = Path(__file__).resolve().parents[1]
    psyche_root = repo_root / "python/python/psyche"

    package = types.ModuleType("psyche")
    package.__path__ = [str(psyche_root)]
    sys.modules["psyche"] = package

    models_package = types.ModuleType("psyche.models")
    models_package.__path__ = [str(psyche_root / "models")]
    sys.modules["psyche.models"] = models_package

    from psyche.models.causal_lm import PretrainedSourceStateDict
    from psyche.models.factory import make_causal_lm

    return make_causal_lm, PretrainedSourceStateDict


def iter_named_parameters(model):
    named = model.named_parameters()
    if isinstance(named, Mapping):
        return named.items()
    return named


def assert_mps_available() -> None:
    fallback = os.environ.get("PYTORCH_ENABLE_MPS_FALLBACK")
    if fallback not in {"0", "false", "False"}:
        raise SystemExit(
            "PYTORCH_ENABLE_MPS_FALLBACK is set to "
            f"{fallback!r}; run with PYTORCH_ENABLE_MPS_FALLBACK=0 so "
            "unsupported MPS ops fail loudly."
        )
    if not torch.backends.mps.is_available():
        raise SystemExit("MPS is not available in this Python/PyTorch environment.")


def tiny_llama_source(pretrained_source_class):
    torch.manual_seed(20260606)
    config = LlamaConfig(
        vocab_size=32,
        hidden_size=16,
        intermediate_size=32,
        num_hidden_layers=1,
        num_attention_heads=2,
        num_key_value_heads=2,
        max_position_embeddings=16,
        rms_norm_eps=1e-5,
        attention_dropout=0.0,
        pad_token_id=0,
        bos_token_id=1,
        eos_token_id=2,
        tie_word_embeddings=False,
    )
    model = AutoModelForCausalLM.from_config(config, attn_implementation="sdpa")
    model.eval()
    state_dict = {
        name: tensor.detach().cpu().clone()
        for name, tensor in model.state_dict().items()
    }
    return pretrained_source_class(
        config_json=json.dumps(config.to_dict()),
        state_dict=state_dict,
    )


def assert_model_device(model, expected_type: str) -> None:
    bad = [
        (name, str(param.device))
        for name, param in iter_named_parameters(model)
        if param.device.type != expected_type
    ]
    if bad:
        sample = ", ".join(f"{name}={device}" for name, device in bad[:10])
        raise AssertionError(f"model parameters not on {expected_type}: {sample}")


def run_forward_backward(model, device: torch.device):
    input_ids = torch.tensor(
        [[1, 3, 4, 5, 2, 0], [1, 6, 7, 8, 9, 2]],
        dtype=torch.long,
        device=device,
    )
    labels = input_ids.clone()
    model.train()
    logits, loss = model.forward(input_ids=input_ids, labels=labels)
    if logits is None or loss is None:
        raise AssertionError("HfAuto forward did not return logits and loss")
    if logits.device.type != device.type or loss.device.type != device.type:
        raise AssertionError(
            f"forward returned logits={logits.device}, loss={loss.device}, expected {device}"
        )
    if not torch.isfinite(loss.detach().cpu()):
        raise AssertionError(f"non-finite loss: {loss}")
    loss.backward()
    grad_norm_sum = 0.0
    grad_count = 0
    for name, param in iter_named_parameters(model):
        if param.grad is None:
            continue
        grad = param.grad.detach()
        if grad.device.type != device.type:
            raise AssertionError(f"{name} grad on {grad.device}, expected {device}")
        if not torch.isfinite(grad.cpu()).all():
            raise AssertionError(f"{name} grad contains non-finite values")
        grad_norm_sum += float(grad.float().cpu().norm())
        grad_count += 1
    if grad_count == 0 or grad_norm_sum <= 0.0:
        raise AssertionError("backward produced no non-zero gradients")
    if device.type == "mps":
        torch.mps.synchronize()
    return logits.detach().cpu(), float(loss.detach().cpu()), grad_norm_sum


def assert_resolved_attention(model, expected: str) -> None:
    resolved = getattr(model.model.config, "_attn_implementation", None)
    if resolved != expected:
        raise AssertionError(
            f"redirected attention implementation resolved to {resolved!r}, "
            f"expected {expected!r}"
        )


def assert_non_allowlisted_architecture_rejected(make_causal_lm, source) -> None:
    try:
        make_causal_lm(
            "Torchtitan",
            source,
            device="cuda:0",
            attn_implementation="flash_attention_2",
            param_dtype=torch.float32,
        )
    except Exception as exc:
        if type(exc) is not RuntimeError:
            raise AssertionError(
                "non-allowlisted architecture failed for the wrong reason: "
                f"{type(exc).__name__}: {exc}"
            ) from exc
        message = str(exc)
        if "not validated on MPS" not in message:
            raise AssertionError(f"unexpected non-allowlist error: {message}") from exc
        print("non-allowlisted redirected architecture is rejected: ok")
        return
    raise AssertionError(
        "non-allowlisted architecture unexpectedly accepted MPS redirect"
    )


def main() -> int:
    assert_mps_available()
    make_causal_lm, pretrained_source_class = install_psyche_import_shell()
    source = tiny_llama_source(pretrained_source_class)
    assert_non_allowlisted_architecture_rejected(make_causal_lm, source)

    cpu_model = make_causal_lm(
        "HfAuto",
        source,
        device="cpu",
        attn_implementation="sdpa",
        param_dtype=torch.float32,
    )
    cpu_logits, cpu_loss, cpu_grad_norm = run_forward_backward(
        cpu_model,
        torch.device("cpu"),
    )
    print(f"cpu baseline: loss={cpu_loss:.6f} grad_norm={cpu_grad_norm:.6f}")

    redirected = make_causal_lm(
        "HfAuto",
        source,
        device="cuda:0",
        attn_implementation="flash_attention_2",
        param_dtype=torch.float32,
    )
    if redirected.device.type != "mps":
        raise AssertionError(
            f"redirected HfAuto device is {redirected.device}, expected mps"
        )
    assert_resolved_attention(redirected, "sdpa")
    assert_model_device(redirected, "mps")
    mps_logits, mps_loss, mps_grad_norm = run_forward_backward(
        redirected,
        torch.device("mps"),
    )
    print(f"mps redirect: loss={mps_loss:.6f} grad_norm={mps_grad_norm:.6f}")

    loss_delta = abs(cpu_loss - mps_loss)
    logits_delta = (cpu_logits.float() - mps_logits.float()).abs().max().item()
    if loss_delta > 2e-2 or logits_delta > 2e-2:
        raise AssertionError(
            f"CPU/MPS parity drift too high: loss_delta={loss_delta:.6g}, "
            f"logits_max_delta={logits_delta:.6g}"
        )
    print(
        "HfAuto redirected CUDA-shaped intent runs on MPS with fallback disabled: "
        f"loss_delta={loss_delta:.12g} logits_max_delta={logits_delta:.12g}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
