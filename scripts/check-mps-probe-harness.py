#!/usr/bin/env python3
"""Self-check probe harness helpers that do not require MPS hardware."""

from __future__ import annotations

import importlib.util
import json
import os
import sys
from pathlib import Path


def load_probe_module():
    module_path = Path(__file__).resolve().parent / "probe-mps-unsupported-ops.py"
    spec = importlib.util.spec_from_file_location("psyche_probe_mps_unsupported_ops", module_path)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check_sentinel_parser(module) -> None:  # noqa: ANN001
    first = {"name": "old", "status": "error"}
    last = {"name": "take", "status": "ok"}
    stdout = "\n".join(
        [
            "debug noise before",
            module.PROBE_JSON_SENTINEL + json.dumps(first),
            "debug noise between",
            module.PROBE_JSON_SENTINEL + json.dumps(last),
            "debug noise after",
        ]
    )
    parsed = module.parse_probe_json_from_stdout(stdout)
    if parsed != last:
        raise AssertionError(f"sentinel parser did not return last row: {parsed}")

    try:
        module.parse_probe_json_from_stdout("debug noise without sentinel")
    except json.JSONDecodeError:
        pass
    else:
        raise AssertionError("sentinel parser accepted stdout without sentinel")


def check_route_state(module) -> None:  # noqa: ANN001
    install_result = {
        "installed": ["aten::take.default", "aten::logspace.default"],
        "already_registered": ["aten::heaviside.default"],
        "skipped_existing_mps": ["aten::channel_shuffle.default"],
        "disabled_by_env": ["aten::linalg_qr.default"],
    }
    cases = {
        "take": "installed",
        "logspace": "installed",
        "heaviside": "already_registered",
        "channel_shuffle": "skipped_existing_mps",
        "linalg_qr": "disabled_by_env",
        "linear_gelu_adamw_step": "not_a_psyche_route",
    }
    for probe_name, expected in cases.items():
        got = module.psyche_route_state_for_probe(probe_name, install_result)
        if got != expected:
            raise AssertionError(f"{probe_name}: expected {expected}, got {got}")

    got = module.psyche_route_state_for_probe("take", None)
    if got != "psyche_not_installed":
        raise AssertionError(f"expected psyche_not_installed, got {got}")


def check_experimental_probe_route_state(module) -> None:  # noqa: ANN001
    cases = [
        ("take", False, {}, "not_applicable"),
        ("linalg_svd", False, {}, "disabled"),
        ("linalg_svd", True, {}, "enabled_not_used"),
        (
            "linalg_svd",
            True,
            {"aten::linalg_svd": 1},
            "experimental_approximate_svd_dispatch",
        ),
    ]
    for probe_name, enabled, replacements, expected in cases:
        got = module.psyche_experimental_probe_route_state_for_probe(
            probe_name,
            enabled,
            replacements,
        )
        if got != expected:
            raise AssertionError(f"{probe_name}: expected {expected}, got {got}")


def check_experimental_env(module) -> None:  # noqa: ANN001
    previous = {name: os.environ.get(name) for name in module.EXPERIMENTAL_ROUTE_ENV}
    try:
        for name in module.EXPERIMENTAL_ROUTE_ENV:
            os.environ.pop(name, None)
        module.enable_experimental_psyche_routes()
        for name, expected in module.EXPERIMENTAL_ROUTE_ENV.items():
            got = os.environ.get(name)
            if got != expected:
                raise AssertionError(f"{name}: expected {expected}, got {got}")
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


def main() -> int:
    module = load_probe_module()
    check_sentinel_parser(module)
    check_route_state(module)
    check_experimental_probe_route_state(module)
    check_experimental_env(module)
    print("MPS probe harness helpers: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
