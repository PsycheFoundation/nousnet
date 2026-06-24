#!/usr/bin/env python3
"""
Validate sidecar device resolution in a subprocess.

This does not claim to run a full distributed sidecar training loop. It checks
the sidecar boundary that matters for native silicon compatibility: explicit
device arguments, parser wiring, env propagation, CUDA-shaped MPS redirection,
the Gloo-only backend guard, and a single-rank Gloo CPU-staged collective smoke.

Run on native silicon:

    PSYCHE_CUDA_COMPAT=1 scripts/check-sidecar-mps-device.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import textwrap
from pathlib import Path


def child_code() -> str:
    return textwrap.dedent(r"""
        import json
        import os
        import socket
        import sys
        import types
        from datetime import timedelta
        from pathlib import Path

        import torch
        import torch.distributed as dist

        repo_root = Path(os.environ["PSYCHE_REPO_ROOT"])
        psyche_root = repo_root / "python/python/psyche"

        package = types.ModuleType("psyche")
        package.__path__ = [str(psyche_root)]
        package.make_causal_lm = object()
        package.PretrainedSourceRepoFiles = object
        package.PretrainedSourceStateDict = object
        package.Trainer = object
        package.DistroResult = object
        package.start_process_watcher = lambda *args, **kwargs: None
        sys.modules["psyche"] = package

        api_module = types.ModuleType("psyche.sidecar.api")
        for name in (
            "DistroResultsMetadata",
            "ForwardOperation",
            "Hyperparameters",
            "OptimizeOperation",
            "TrainOperation",
        ):
            setattr(api_module, name, type(name, (), {}))

        sidecar_package = types.ModuleType("psyche.sidecar")
        sidecar_package.__path__ = [str(psyche_root / "sidecar")]
        sys.modules["psyche.sidecar"] = sidecar_package
        sys.modules["psyche.sidecar.api"] = api_module

        from psyche.sidecar.__main__ import (
            build_arg_parser,
            resolve_and_validate_sidecar_args,
            resolve_sidecar_device,
            validate_sidecar_backend,
        )

        if not torch.backends.mps.is_available():
            raise SystemExit("MPS is not available in this Python/PyTorch environment.")

        parser = build_arg_parser()
        try:
            parser.parse_args(["--rank", "1", "--backend", "gloo"])
        except SystemExit:
            pass
        else:
            raise AssertionError("sidecar parser accepted missing --device")

        results = {}
        for request in ("0", "cuda", "cuda:0", "mps"):
            resolution = resolve_sidecar_device(request)
            validate_sidecar_backend(resolution, "gloo")
            results[request] = {
                "resolved": str(resolution.resolved),
                "redirected": resolution.redirected,
                "status": resolution.status,
            }

        for bad_request in (None, "", " "):
            try:
                resolve_sidecar_device(bad_request)
            except ValueError as exc:
                message = str(exc)
                if "explicit --device" not in message and "cannot be empty" not in message:
                    raise AssertionError(f"unexpected device error message: {message}") from exc
                continue
            raise AssertionError(f"{bad_request!r} unexpectedly resolved")

        parsed = parser.parse_args(
            ["--rank", "0", "--world-size", "1", "--backend", "gloo", "--device", "0"]
        )
        parsed_resolution = resolve_and_validate_sidecar_args(parsed)
        if parsed_resolution.resolved.type != "mps" or not parsed_resolution.redirected:
            raise AssertionError(f"parser path did not redirect to MPS: {parsed_resolution}")

        redirected = parser.parse_args(
            ["--rank", "0", "--world-size", "1", "--backend", "nccl", "--device", "0"]
        )
        try:
            resolve_and_validate_sidecar_args(redirected)
        except RuntimeError as exc:
            message = str(exc)
            if "redirected to MPS" not in message or "Use the gloo backend" not in message:
                raise AssertionError(f"unexpected backend guard message: {message}") from exc
        else:
            raise AssertionError("MPS sidecar unexpectedly accepted NCCL backend")

        multi_rank = parser.parse_args(
            ["--rank", "0", "--world-size", "2", "--backend", "gloo", "--device", "mps"]
        )
        try:
            resolve_and_validate_sidecar_args(multi_rank)
        except RuntimeError as exc:
            message = str(exc)
            if "single-rank only" not in message or "world_size=2" not in message:
                raise AssertionError(f"unexpected MPS world-size message: {message}") from exc
        else:
            raise AssertionError("MPS sidecar unexpectedly accepted world_size=2")

        nonzero_rank = parser.parse_args(
            ["--rank", "1", "--world-size", "1", "--backend", "gloo", "--device", "mps"]
        )
        try:
            resolve_and_validate_sidecar_args(nonzero_rank)
        except RuntimeError as exc:
            message = str(exc)
            if "single-rank only" not in message or "rank=1" not in message:
                raise AssertionError(f"unexpected MPS rank message: {message}") from exc
        else:
            raise AssertionError("MPS sidecar unexpectedly accepted rank=1")

        explicit_mps = resolve_sidecar_device("mps")
        try:
            validate_sidecar_backend(explicit_mps, None)
        except RuntimeError as exc:
            message = str(exc)
            if "An MPS device was requested" not in message or "--backend gloo" not in message:
                raise AssertionError(f"unexpected explicit MPS backend message: {message}") from exc
        else:
            raise AssertionError("MPS sidecar unexpectedly accepted missing backend")

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(("127.0.0.1", 0))
            _, port = sock.getsockname()

        dist.init_process_group(
            backend="gloo",
            init_method=f"tcp://127.0.0.1:{port}",
            world_size=1,
            rank=0,
            timeout=timedelta(seconds=30),
        )
        try:
            cpu_tensor = torch.tensor([7.0], dtype=torch.float32, device="cpu")
            dist.broadcast(cpu_tensor, 0)
            mps_tensor = cpu_tensor.to("mps")
            torch.mps.synchronize()
            value = float(mps_tensor.cpu().item())
            if value != 7.0:
                raise AssertionError(f"unexpected Gloo CPU-stage value: {value}")
            results["single_rank_gloo_cpu_stage"] = {
                "resolved": str(mps_tensor.device),
                "value": value,
            }
        finally:
            dist.destroy_process_group()

        print(json.dumps(results, sort_keys=True))
        """)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    env = os.environ.copy()
    env["PSYCHE_REPO_ROOT"] = str(repo_root)

    if env.get("PSYCHE_CUDA_COMPAT", "").strip().lower() not in {
        "1",
        "true",
        "yes",
        "on",
        "force",
    }:
        raise SystemExit(
            "Run with PSYCHE_CUDA_COMPAT=1 so CUDA-shaped sidecar requests redirect."
        )

    proc = subprocess.run(
        [sys.executable, "-c", child_code()],
        cwd=repo_root,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        return proc.returncode

    payload_lines = [line for line in proc.stdout.splitlines() if line.strip()]
    if not payload_lines:
        raise AssertionError("sidecar subprocess produced no JSON payload")
    results = json.loads(payload_lines[-1])
    expected_redirects = {"0", "cuda", "cuda:0"}
    for request in expected_redirects:
        result = results[request]
        if result["resolved"] != "mps" or not result["redirected"]:
            raise AssertionError(f"{request!r} did not redirect to MPS: {result}")
    if results["mps"]["resolved"] != "mps" or results["mps"]["redirected"]:
        raise AssertionError(
            f"explicit MPS request resolved incorrectly: {results['mps']}"
        )
    gloo_smoke = results["single_rank_gloo_cpu_stage"]
    if gloo_smoke["resolved"] != "mps:0" or gloo_smoke["value"] != 7.0:
        raise AssertionError(f"Gloo CPU-stage smoke failed: {gloo_smoke}")

    print(
        "sidecar subprocess device resolution, backend guard, and Gloo CPU-stage smoke: ok"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
