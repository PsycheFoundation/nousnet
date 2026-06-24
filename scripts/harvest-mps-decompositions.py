#!/usr/bin/env python3
"""
Find PyTorch decompositions that could become exact MPS compatibility kernels.

This is a development tool. It does not register kernels. It intersects the
current torch build's dispatcher gaps with `torch._decomp.decomposition_table`
and reports missing-MPS ops that already have an upstream decomposition.
"""

from __future__ import annotations

import argparse
import inspect
import json
import re
from collections import Counter
from pathlib import Path
from typing import Any


SOURCE_OP_PATTERN = re.compile(
    r"(?:torch\.ops\.)?aten\.([A-Za-z_][A-Za-z0-9_]*)(?:\.([A-Za-z_][A-Za-z0-9_]*))?"
)

PRIORITY_KEYWORDS = {
    "adaptive": 40,
    "pool": 35,
    "backward": 25,
    "norm": 20,
    "loss": 20,
    "index": 15,
    "scatter": 15,
    "gather": 15,
    "linalg": 10,
    "fft": -20,
    "sparse": -30,
    "quantized": -40,
    "c10d": -50,
    "onednn": -50,
    "prepacked": -50,
}


def import_torch():
    import torch
    import torch._decomp as decomp

    return torch, decomp


def has_kernel(torch: Any, op_name: str, key: str) -> bool:
    try:
        return bool(torch._C._dispatch_has_kernel_for_dispatch_key(op_name, key))
    except RuntimeError:
        return False


def op_overload_name(overload: Any) -> str:
    return overload.name()


def source_called_ops(fn: Any) -> list[str]:
    try:
        source = inspect.getsource(fn)
    except (OSError, TypeError):
        return []

    called = set()
    for match in SOURCE_OP_PATTERN.finditer(source):
        name, overload = match.groups()
        if overload and overload != "default":
            called.add(f"aten::{name}.{overload}")
        else:
            called.add(f"aten::{name}")
    return sorted(called)


def priority_score(op_name: str, called_ops: list[str]) -> int:
    score = 0
    haystack = " ".join([op_name, *called_ops]).lower()
    for keyword, value in PRIORITY_KEYWORDS.items():
        if keyword in haystack:
            score += value
    if op_name.startswith("aten::"):
        score += 10
    return score


def classify_op(torch: Any, op_name: str) -> str:
    if has_kernel(torch, op_name, "MPS"):
        return "direct_mps"
    if has_kernel(torch, op_name, "CompositeImplicitAutograd") or has_kernel(
        torch, op_name, "CompositeExplicitAutograd"
    ):
        return "composite_candidate"
    if has_kernel(torch, op_name, "CPU"):
        return "likely_cpu_fallback_or_not_implemented"
    return "no_mps_no_cpu_or_special_backend"


def harvest() -> dict[str, Any]:
    torch, decomp = import_torch()
    table = getattr(decomp, "decomposition_table", {})
    rows = []
    counts = Counter()

    for overload, fn in table.items():
        op_name = op_overload_name(overload)
        classification = classify_op(torch, op_name)
        counts[classification] += 1
        if classification != "likely_cpu_fallback_or_not_implemented":
            continue

        called = source_called_ops(fn)
        missing_called = [
            op
            for op in called
            if classify_op(torch, op) == "likely_cpu_fallback_or_not_implemented"
        ]
        rows.append(
            {
                "op": op_name,
                "schema": str(getattr(overload, "_schema", "")),
                "decomposition": f"{fn.__module__}.{getattr(fn, '__name__', '<unknown>')}",
                "source_called_ops": called,
                "missing_source_called_ops": missing_called,
                "priority": priority_score(op_name, called),
            }
        )

    rows.sort(
        key=lambda row: (
            len(row["missing_source_called_ops"]) > 0,
            -row["priority"],
            row["op"],
        )
    )

    return {
        "torch_version": torch.__version__,
        "total_decompositions": len(table),
        "decomposition_classification_counts": dict(counts),
        "candidate_count": len(rows),
        "candidates": rows,
    }


def write_markdown(report: dict[str, Any], path: Path, limit: int) -> None:
    lines = [
        "# MPS Decomposition Candidates",
        "",
        f"- Torch version: `{report['torch_version']}`",
        f"- Total decomposition table entries: `{report['total_decompositions']}`",
        f"- Likely fallback candidates with decompositions: `{report['candidate_count']}`",
        "",
        "## Classification Counts",
        "",
    ]
    for name, count in sorted(report["decomposition_classification_counts"].items()):
        lines.append(f"- `{name}`: `{count}`")

    lines.extend(["", "## Candidates", ""])
    for row in report["candidates"][:limit]:
        missing = ", ".join(f"`{op}`" for op in row["missing_source_called_ops"]) or "none detected"
        called = ", ".join(f"`{op}`" for op in row["source_called_ops"][:12]) or "none detected"
        if len(row["source_called_ops"]) > 12:
            called += ", ..."
        lines.extend(
            [
                f"### `{row['op']}`",
                "",
                f"- Priority: `{row['priority']}`",
                f"- Decomposition: `{row['decomposition']}`",
                f"- Missing source-called ops: {missing}",
                f"- Source-called ops: {called}",
                "",
                "```text",
                row["schema"],
                "```",
                "",
            ]
        )

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--markdown-out", type=Path)
    parser.add_argument("--markdown-limit", type=int, default=80)
    args = parser.parse_args()

    report = harvest()
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2, sort_keys=True))
    if args.markdown_out:
        write_markdown(report, args.markdown_out, args.markdown_limit)

    print(
        "decomposition candidates:",
        report["candidate_count"],
        "of",
        report["total_decompositions"],
        "decompositions",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
