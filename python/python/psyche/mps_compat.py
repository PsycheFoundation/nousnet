import math
import os
from contextlib import nullcontext
from dataclasses import dataclass, field
from itertools import product
from typing import Any, Optional

import torch
# Private API, intentionally isolated to the opt-in AutogradMPS registration.
# `torch.library.register_autograd` targets the broad Autograd key on this stack.
from torch._library import autograd as torch_library_autograd
from torch.utils._python_dispatch import TorchDispatchMode


_MPS_COMPAT_LIBRARY = None
_MPS_COMPAT_AUTOGRAD_LIBRARY = None
_REGISTERED_KERNELS: set[str] = set()
_ADAPTIVE_AVG_POOL3D = "aten::_adaptive_avg_pool3d"
_ADAPTIVE_AVG_POOL3D_BACKWARD = "aten::_adaptive_avg_pool3d_backward"
_HEAVISIDE = "aten::heaviside"
_HEAVISIDE_OUT = "aten::heaviside.out"
_GCD = "aten::gcd"
_GCD_OUT = "aten::gcd.out"
_LCM = "aten::lcm"
_LCM_OUT = "aten::lcm.out"
_STD_CORRECTION = "aten::std.correction"
_STD_CORRECTION_OUT = "aten::std.correction_out"
_VAR_CORRECTION = "aten::var.correction"
_VAR_CORRECTION_OUT = "aten::var.correction_out"
_TAKE = "aten::take"
_TAKE_OUT = "aten::take.out"
_LOGIT_INPLACE = "aten::logit_"
_ADDMM_ACTIVATION = "aten::_addmm_activation"
_ADDMM_ACTIVATION_OUT = "aten::_addmm_activation.out"
_ADDMM_ACTIVATION_AUTOGRAD = "aten::_addmm_activation.autograd"
_CHANNEL_SHUFFLE = "aten::channel_shuffle"
_LOGSPACE = "aten::logspace"
_LOGSPACE_OUT = "aten::logspace.out"
_MVLGAMMA = "aten::mvlgamma"
_MVLGAMMA_OUT = "aten::mvlgamma.out"
_VDOT = "aten::vdot"
_VDOT_OUT = "aten::vdot.out"
_FREXP = "aten::frexp.Tensor"
_FREXP_OUT = "aten::frexp.Tensor_out"
_GEQRF = "aten::geqrf"
_LINALG_MATRIX_EXP = "aten::linalg_matrix_exp"
_LINALG_MATRIX_EXP_OUT = "aten::linalg_matrix_exp.out"
_LINALG_QR = "aten::linalg_qr"
_LINALG_QR_OUT = "aten::linalg_qr.out"
_INTEGER_DTYPES = {
    torch.bool,
    torch.int8,
    torch.uint8,
    torch.int16,
    torch.int32,
    torch.int64,
}
_MPS_REAL_FLOAT_DTYPES = {
    torch.float16,
    torch.bfloat16,
    torch.float32,
}
_GCD_RESULT_DTYPES = {
    torch.int8: (True, False),
    torch.uint8: (False, False),
    torch.int16: (True, False),
    torch.int32: (False, True),
    torch.int64: (False, True),
}
_GCD_ITERATIONS = {
    torch.int8: 16,
    torch.uint8: 16,
    torch.int16: 32,
    torch.int32: 64,
    torch.int64: 128,
}
_PADE13_COEFFICIENTS = (
    1.0,
    0.5,
    0.12,
    0.018333333333333333,
    0.001992063492063492,
    0.00016304347826086957,
    0.000010351966873706,
    0.0000005175983436853002,
    0.00000002043151389366347,
    0.000000000630665961333586,
    0.00000000001483027285835858,
    0.0000000000002529153491597568,
    0.0000000000000028101705462199623,
    0.000000000000000015442178324409563,
)
_PADE13_THETA = 5.371920351148152

_DEFAULT_EXACT_MPS_COMPAT_ROUTES = (
    f"{_ADAPTIVE_AVG_POOL3D}.default",
    f"{_ADAPTIVE_AVG_POOL3D_BACKWARD}.default",
    f"{_HEAVISIDE}.default",
    _HEAVISIDE_OUT,
    f"{_GCD}.default",
    _GCD_OUT,
    f"{_LCM}.default",
    _LCM_OUT,
    _STD_CORRECTION_OUT,
    _VAR_CORRECTION_OUT,
    f"{_TAKE}.default",
    _TAKE_OUT,
    _LOGIT_INPLACE,
    f"{_ADDMM_ACTIVATION}.default",
    _ADDMM_ACTIVATION_OUT,
    f"{_CHANNEL_SHUFFLE}.default",
    f"{_LOGSPACE}.default",
    _LOGSPACE_OUT,
    _MVLGAMMA_OUT,
    f"{_VDOT}.default",
    _VDOT_OUT,
    _FREXP,
    _FREXP_OUT,
    f"{_GEQRF}.default",
)

_EXPERIMENTAL_EXACT_MPS_COMPAT_ROUTES = (
    f"{_LINALG_MATRIX_EXP}.default",
    _LINALG_MATRIX_EXP_OUT,
    f"{_LINALG_QR}.default",
    _LINALG_QR_OUT,
)

_GATED_AUTOGRAD_MPS_COMPAT_ROUTES = (
    _ADDMM_ACTIVATION_AUTOGRAD,
)


def mps_compat_route_manifest() -> dict[str, tuple[str, ...]]:
    """Return the route names that Psyche intentionally owns for MPS compat."""

    return {
        "default_exact": _DEFAULT_EXACT_MPS_COMPAT_ROUTES,
        "experimental_exact": _EXPERIMENTAL_EXACT_MPS_COMPAT_ROUTES,
        "gated_autograd": _GATED_AUTOGRAD_MPS_COMPAT_ROUTES,
    }


def _env_enabled(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {
        "1",
        "true",
        "yes",
        "on",
        "force",
    }


def _env_disabled(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {
        "0",
        "false",
        "no",
        "off",
    }


def _mps_compat_enabled_for_cuda_compat() -> bool:
    return _env_enabled("PSYCHE_CUDA_COMPAT") and not _env_disabled(
        "PSYCHE_CUDA_COMPAT_MPS_ROUTES"
    )


def _op_name(func: Any) -> str:
    return getattr(func, "__name__", str(func))


def _is_mps_tensor(value: Any) -> bool:
    return isinstance(value, torch.Tensor) and value.device.type == "mps"


def _tree_has_mps_tensor(value: Any) -> bool:
    if _is_mps_tensor(value):
        return True
    if isinstance(value, (tuple, list)):
        return any(_tree_has_mps_tensor(item) for item in value)
    if isinstance(value, dict):
        return any(_tree_has_mps_tensor(item) for item in value.values())
    return False


def _accumulation_dtype(dtype: torch.dtype) -> torch.dtype:
    if dtype in (torch.float16, torch.bfloat16):
        return torch.float32
    return dtype


def _normalize_output_size(
    output_size: Any,
    input_shape: tuple[int, int, int],
) -> tuple[int, int, int]:
    if isinstance(output_size, torch.Tensor):
        output_size = output_size.tolist()
    if isinstance(output_size, int):
        output_size = (output_size, output_size, output_size)
    if len(output_size) != 3:
        raise ValueError(
            f"adaptive_avg_pool3d output_size must have 3 dims, got {output_size!r}"
        )

    normalized = []
    for requested, input_dim in zip(output_size, input_shape):
        if requested is None:
            normalized.append(int(input_dim))
            continue
        requested_int = int(requested)
        if requested_int <= 0:
            raise ValueError(
                f"adaptive_avg_pool3d output dimensions must be positive, got {output_size!r}"
            )
        normalized.append(requested_int)
    return tuple(normalized)


def _adaptive_avg_pool3d_mps(
    input_tensor: torch.Tensor,
    output_size: Any,
) -> torch.Tensor:
    input_depth, input_height, input_width = input_tensor.shape[-3:]
    output_depth, output_height, output_width = _normalize_output_size(
        output_size,
        (input_depth, input_height, input_width),
    )

    depth_planes = []
    for od in range(output_depth):
        depth_start = math.floor(od * input_depth / output_depth)
        depth_end = math.ceil((od + 1) * input_depth / output_depth)
        height_rows = []
        for oh in range(output_height):
            height_start = math.floor(oh * input_height / output_height)
            height_end = math.ceil((oh + 1) * input_height / output_height)
            width_cols = []
            for ow in range(output_width):
                width_start = math.floor(ow * input_width / output_width)
                width_end = math.ceil((ow + 1) * input_width / output_width)
                region = input_tensor[
                    ...,
                    depth_start:depth_end,
                    height_start:height_end,
                    width_start:width_end,
                ]
                mean = region.to(_accumulation_dtype(region.dtype)).mean(dim=(-3, -2, -1))
                width_cols.append(mean.to(input_tensor.dtype))
            height_rows.append(torch.stack(width_cols, dim=-1))
        depth_planes.append(torch.stack(height_rows, dim=-2))
    return torch.stack(depth_planes, dim=-3)


def _adaptive_avg_pool3d_backward_mps(
    grad_output: torch.Tensor,
    input_tensor: torch.Tensor,
) -> torch.Tensor:
    input_depth, input_height, input_width = input_tensor.shape[-3:]
    output_depth, output_height, output_width = grad_output.shape[-3:]
    accumulation_dtype = _accumulation_dtype(input_tensor.dtype)
    grad_input = torch.zeros_like(input_tensor, dtype=accumulation_dtype)
    grad_output = grad_output.to(accumulation_dtype)

    for od in range(output_depth):
        depth_start = math.floor(od * input_depth / output_depth)
        depth_end = math.ceil((od + 1) * input_depth / output_depth)
        for oh in range(output_height):
            height_start = math.floor(oh * input_height / output_height)
            height_end = math.ceil((oh + 1) * input_height / output_height)
            for ow in range(output_width):
                width_start = math.floor(ow * input_width / output_width)
                width_end = math.ceil((ow + 1) * input_width / output_width)
                volume = (
                    (depth_end - depth_start)
                    * (height_end - height_start)
                    * (width_end - width_start)
                )
                contribution = grad_output[..., od, oh, ow] / volume
                grad_input[
                    ...,
                    depth_start:depth_end,
                    height_start:height_end,
                    width_start:width_end,
                ] = (
                    grad_input[
                        ...,
                        depth_start:depth_end,
                        height_start:height_end,
                        width_start:width_end,
                    ]
                    + contribution[..., None, None, None]
                )

    return grad_input.to(input_tensor.dtype)


def _safe_vector_norm(vector: torch.Tensor) -> torch.Tensor:
    norm = vector.float().pow(2).sum().sqrt()
    return torch.clamp(norm, min=1e-12)


def _matrix_identity_like(matrix: torch.Tensor) -> torch.Tensor:
    eye = torch.eye(
        matrix.shape[-1],
        dtype=matrix.dtype,
        device=matrix.device,
    )
    return eye.expand(matrix.shape)


def _pade13_matrix_exp_mps(matrix: torch.Tensor) -> torch.Tensor:
    b = _PADE13_COEFFICIENTS
    ident = _matrix_identity_like(matrix)
    matrix2 = matrix @ matrix
    matrix4 = matrix2 @ matrix2
    matrix6 = matrix4 @ matrix2

    u = matrix @ (
        matrix6 @ (b[13] * matrix6 + b[11] * matrix4 + b[9] * matrix2)
        + b[7] * matrix6
        + b[5] * matrix4
        + b[3] * matrix2
        + b[1] * ident
    )
    v = (
        matrix6 @ (b[12] * matrix6 + b[10] * matrix4 + b[8] * matrix2)
        + b[6] * matrix6
        + b[4] * matrix4
        + b[2] * matrix2
        + b[0] * ident
    )
    return torch.linalg.solve(v - u, v + u)


def _linalg_matrix_exp_mps(matrix: torch.Tensor) -> torch.Tensor:
    if matrix.ndim < 2:
        raise RuntimeError(
            f"linalg.matrix_exp: A must have at least 2 dimensions, got {matrix.ndim}"
        )
    if matrix.shape[-1] != matrix.shape[-2]:
        raise RuntimeError(
            "linalg.matrix_exp: A must be batches of square matrices, "
            f"but they are {matrix.shape[-2]} by {matrix.shape[-1]} matrices"
        )
    if matrix.is_complex():
        raise NotImplementedError("MPS matrix_exp compatibility route does not support complex tensors")
    if matrix.dtype not in (torch.float16, torch.bfloat16, torch.float32):
        raise NotImplementedError(f"MPS matrix_exp compatibility route does not support {matrix.dtype}")

    result_dtype = matrix.dtype
    work = matrix.to(torch.float32)
    if work.numel() == 0:
        return torch.empty_like(matrix)
    original_shape = work.shape
    matrix_size = work.shape[-1]
    work = work.reshape(-1, matrix_size, matrix_size)

    norm = torch.linalg.matrix_norm(work, ord=1)
    norm_scalar = float(norm.max().detach().cpu())
    if norm_scalar == 0.0:
        return _matrix_identity_like(work).clone().reshape(original_shape).to(result_dtype)

    scale = max(0, math.ceil(math.log2(norm_scalar / _PADE13_THETA)))
    result = _pade13_matrix_exp_mps(work / (2.0**scale))
    for _ in range(scale):
        result = result @ result
    return result.reshape(original_shape).to(result_dtype)


def _validate_qr_input(matrix: torch.Tensor, mode: str) -> None:
    if mode not in {"reduced", "complete", "r"}:
        raise RuntimeError(
            f"qr received unrecognized mode '{mode}' but expected one of "
            "'reduced', 'r', or 'complete'"
        )
    if matrix.ndim < 2:
        raise RuntimeError(
            f"linalg.qr: The input tensor A must have at least 2 dimensions, got {matrix.ndim}"
        )
    if matrix.is_complex():
        raise NotImplementedError("MPS QR compatibility route does not support complex tensors")
    if matrix.dtype not in (torch.float16, torch.bfloat16, torch.float32):
        raise NotImplementedError(f"MPS QR compatibility route does not support {matrix.dtype}")


def _orthogonalize_vector(
    vector: torch.Tensor,
    basis_columns: list[torch.Tensor],
) -> torch.Tensor:
    for basis in basis_columns:
        projection = (basis * vector).sum(dim=-1, keepdim=True)
        vector = vector - projection * basis
    return vector


def _normalize_with_basis_fallback(
    vector: torch.Tensor,
    basis_columns: list[torch.Tensor],
    *,
    rows: int,
    cols: int,
    batch_shape: tuple[int, ...],
    preferred_basis_index: int,
    scale: torch.Tensor,
) -> torch.Tensor:
    relative_threshold = max(rows, cols) * torch.finfo(vector.dtype).eps
    threshold = scale * relative_threshold
    norm = vector.pow(2).sum(dim=-1, keepdim=True).sqrt()
    selected = (scale > 0) & (norm > threshold)
    normalized = vector / torch.clamp(norm, min=torch.finfo(vector.dtype).tiny)
    result = torch.where(selected, normalized, torch.zeros_like(normalized))

    eye = torch.eye(rows, dtype=vector.dtype, device=vector.device)
    eye = eye.expand(batch_shape + (rows, rows))
    for offset in range(rows):
        basis_index = (preferred_basis_index + offset) % rows
        fallback = _orthogonalize_vector(eye[..., :, basis_index], basis_columns)
        fallback_norm = fallback.pow(2).sum(dim=-1, keepdim=True).sqrt()
        # This is a unit standard-basis candidate, not a residual from the
        # scaled input column, so the gate is dimensionless.
        use_fallback = (~selected) & (fallback_norm > relative_threshold)
        fallback = fallback / torch.clamp(fallback_norm, min=torch.finfo(vector.dtype).tiny)
        result = torch.where(use_fallback, fallback, result)
        selected = selected | use_fallback

    return result


def _modified_gram_schmidt_qr_mps(
    matrix: torch.Tensor,
    mode: str,
) -> tuple[torch.Tensor, torch.Tensor]:
    _validate_qr_input(matrix, mode)

    rows, cols = matrix.shape[-2:]
    rank = min(rows, cols)
    output_dtype = matrix.dtype
    work = matrix.float()
    batch_shape = work.shape[:-2]
    input_columns = [work[..., :, col] for col in range(cols)]
    q_columns: list[torch.Tensor] = []

    for col in range(rank):
        vector = _orthogonalize_vector(input_columns[col], q_columns)
        vector = _orthogonalize_vector(vector, q_columns)
        scale = input_columns[col].pow(2).sum(dim=-1, keepdim=True).sqrt()
        q_columns.append(
            _normalize_with_basis_fallback(
                vector,
                q_columns,
                rows=rows,
                cols=cols,
                batch_shape=batch_shape,
                preferred_basis_index=col,
                scale=scale,
            )
        )

    if mode == "complete" and len(q_columns) < rows:
        for basis_index in range(rows):
            if len(q_columns) >= rows:
                break
            eye = torch.eye(rows, dtype=work.dtype, device=work.device)
            eye = eye.expand(batch_shape + (rows, rows))
            vector = _orthogonalize_vector(eye[..., :, basis_index], q_columns)
            vector = _orthogonalize_vector(vector, q_columns)
            q_columns.append(
                _normalize_with_basis_fallback(
                    vector,
                    q_columns,
                    rows=rows,
                    cols=cols,
                    batch_shape=batch_shape,
                    preferred_basis_index=basis_index,
                    scale=torch.ones(
                        batch_shape + (1,),
                        dtype=work.dtype,
                        device=work.device,
                    ),
                )
            )

    if q_columns:
        q_matrix = torch.stack(q_columns, dim=-1)
    else:
        q_matrix = torch.empty(
            batch_shape + (rows, 0),
            dtype=work.dtype,
            device=work.device,
        )
    r_full = q_matrix.transpose(-2, -1) @ work

    if mode == "r":
        q_out = torch.empty((0,), dtype=output_dtype, device=matrix.device)
        r_out = r_full[..., :rank, :].to(output_dtype)
    elif mode == "complete":
        q_out = q_matrix[..., :, :rows].to(output_dtype)
        r_out = r_full[..., :rows, :].to(output_dtype)
    else:
        q_out = q_matrix[..., :, :rank].to(output_dtype)
        r_out = r_full[..., :rank, :].to(output_dtype)

    return q_out, r_out


def _validate_geqrf_input(matrix: torch.Tensor) -> None:
    if matrix.ndim < 2:
        raise RuntimeError("torch.geqrf: input must have at least 2 dimensions.")
    if matrix.is_complex():
        raise NotImplementedError("MPS geqrf compatibility route does not support complex tensors")
    if matrix.dtype != torch.float32:
        raise NotImplementedError(f"MPS geqrf compatibility route does not support {matrix.dtype}")


def _geqrf_mps_no_autograd(matrix: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    _validate_geqrf_input(matrix)

    output_dtype = matrix.dtype
    work = matrix.to(torch.float32).clone()
    rows, cols = work.shape[-2:]
    rank = min(rows, cols)
    tau_values: list[torch.Tensor] = []

    for col in range(rank):
        column = work[..., col:, col]
        alpha = column[..., :1]
        tail = column[..., 1:]
        if tail.shape[-1] == 0:
            tail_scale = torch.zeros_like(alpha)
        else:
            tail_scale = tail.abs().amax(dim=-1, keepdim=True)
        scale = column.abs().amax(dim=-1, keepdim=True)
        safe_scale = torch.where(scale > 0, scale, torch.ones_like(scale))
        scaled_column = column / safe_scale
        norm = scale * torch.sqrt((scaled_column * scaled_column).sum(dim=-1, keepdim=True))
        beta = -torch.where(alpha >= 0, norm, -norm)
        active = tail_scale > 0

        safe_beta = torch.where(beta == 0, torch.ones_like(beta), beta)
        tau = torch.where(active, (beta - alpha) / safe_beta, torch.zeros_like(alpha))
        denom = alpha - beta
        safe_denom = torch.where(denom == 0, torch.ones_like(denom), denom)
        householder_tail = torch.where(active, tail / safe_denom, torch.zeros_like(tail))
        diagonal = torch.where(active, beta, alpha)

        if col + 1 < cols:
            reflector = torch.cat([torch.ones_like(alpha), householder_tail], dim=-1)
            trailing = work[..., col:, col + 1 :]
            projection = (reflector.unsqueeze(-2) @ trailing).squeeze(-2)
            updated = trailing - (
                tau.unsqueeze(-1)
                * reflector.unsqueeze(-1)
                * projection.unsqueeze(-2)
            )
            work[..., col:, col + 1 :] = updated

        work[..., col, col] = diagonal.squeeze(-1)
        if col + 1 < rows:
            work[..., col + 1 :, col] = householder_tail
        tau_values.append(tau.squeeze(-1))

    if tau_values:
        tau_out = torch.stack(tau_values, dim=-1)
    else:
        tau_out = torch.empty(
            work.shape[:-2] + (0,),
            dtype=work.dtype,
            device=work.device,
        )
    return work.to(output_dtype), tau_out.to(output_dtype)


class _GeqrfAutogradNotImplemented(torch.autograd.Function):
    @staticmethod
    def forward(ctx: Any, matrix: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:  # noqa: ARG004
        return _geqrf_mps_no_autograd(matrix)

    @staticmethod
    def backward(ctx: Any, grad_a: torch.Tensor, grad_tau: torch.Tensor) -> tuple[torch.Tensor]:  # noqa: ARG004
        raise NotImplementedError("the derivative for 'geqrf' is not implemented.")


def _geqrf_mps(matrix: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    if torch.is_grad_enabled() and matrix.requires_grad:
        return _GeqrfAutogradNotImplemented.apply(matrix)
    return _geqrf_mps_no_autograd(matrix)


def _heaviside_mps_no_autograd(
    input_tensor: torch.Tensor,
    values: torch.Tensor,
) -> torch.Tensor:
    if input_tensor.dtype != values.dtype:
        raise RuntimeError("heaviside is not yet implemented for tensors with different dtypes.")
    if input_tensor.device != values.device:
        raise RuntimeError(
            "Expected all tensors to be on the same device, but found at least two devices, "
            f"{input_tensor.device.type} and {values.device.type}!"
        )

    zero = torch.zeros((), dtype=input_tensor.dtype, device=input_tensor.device)
    one = torch.ones((), dtype=input_tensor.dtype, device=input_tensor.device)
    result = torch.zeros_like(input_tensor)
    result = torch.where(input_tensor > zero, one, result)
    return torch.where(input_tensor == zero, values, result)


class _HeavisideAutogradNotImplemented(torch.autograd.Function):
    @staticmethod
    def forward(ctx, input_tensor, values):  # noqa: ANN001, ARG004
        return _heaviside_mps_no_autograd(input_tensor, values)

    @staticmethod
    def backward(ctx, grad_output):  # noqa: ANN001, ARG004
        raise RuntimeError("derivative for aten::heaviside is not implemented")


def _heaviside_mps(input_tensor: torch.Tensor, values: torch.Tensor) -> torch.Tensor:
    if input_tensor.requires_grad or values.requires_grad:
        return _HeavisideAutogradNotImplemented.apply(input_tensor, values)
    return _heaviside_mps_no_autograd(input_tensor, values)


def _integer_abs_cpu_like(value: torch.Tensor) -> torch.Tensor:
    if value.dtype == torch.uint8:
        return value
    zero = torch.zeros((), dtype=value.dtype, device=value.device)
    return torch.where(value < zero, -value, value)


def _same_logical_tensor(left: torch.Tensor, right: torch.Tensor) -> bool:
    return (
        left.device == right.device
        and left.dtype == right.dtype
        and left.data_ptr() == right.data_ptr()
        and left.storage_offset() == right.storage_offset()
        and tuple(left.shape) == tuple(right.shape)
        and tuple(left.stride()) == tuple(right.stride())
    )


def _has_definite_internal_overlap(value: torch.Tensor) -> bool:
    checker = getattr(torch, "_debug_has_internal_overlap", None)
    if checker is None:
        return False
    overlap_status = checker(value)
    if overlap_status == 0 or value.numel() <= 1:
        return False
    if overlap_status == 1:
        return True
    if value.numel() > 4096:
        return False

    seen_offsets: set[int] = set()
    shape = tuple(value.shape)
    strides = tuple(value.stride())
    storage_offset = int(value.storage_offset())
    for index in product(*(range(size) for size in shape)):
        offset = storage_offset + sum(dim_index * stride for dim_index, stride in zip(index, strides))
        if offset in seen_offsets:
            return True
        seen_offsets.add(offset)
    return False


def _prepare_integer_out(
    *,
    op_name: str,
    computed: torch.Tensor,
    out: torch.Tensor,
    inputs: tuple[torch.Tensor, torch.Tensor],
) -> None:
    if out.device.type != "mps":
        raise RuntimeError(f"{op_name}.out MPS compatibility route requires MPS out")
    if not torch.can_cast(computed.dtype, out.dtype):
        raise RuntimeError(
            f"result type {computed.dtype} can't be cast to the desired output type {out.dtype}"
        )
    if _has_definite_internal_overlap(out):
        raise RuntimeError(
            "unsupported operation: more than one element of the written-to tensor "
            "refers to a single memory location. Please clone() the tensor before "
            "performing the operation."
        )

    for source in inputs:
        if not torch._C._overlaps(out, source):
            continue
        if _same_logical_tensor(out, source):
            if out.shape != computed.shape:
                raise RuntimeError(
                    f"output with shape {list(out.shape)} doesn't match the broadcast shape "
                    f"{list(computed.shape)}"
                )
            continue
        raise RuntimeError(
            "unsupported operation: some elements of the input tensor and the written-to "
            "tensor refer to a single memory location. Please clone() the tensor before "
            "performing the operation."
        )

    if out.shape != computed.shape:
        out.resize_(computed.shape)


def _copy_reduction_out(
    *,
    op_name: str,
    default_impl: Any,
    input_tensor: torch.Tensor,
    dim: Any,
    correction: Any,
    keepdim: bool,
    out: torch.Tensor,
) -> torch.Tensor:
    if input_tensor.device.type != "mps":
        raise RuntimeError(f"{op_name}.correction_out MPS compatibility route requires MPS input")
    if out.device.type != "mps":
        raise RuntimeError(f"{op_name}.correction_out MPS compatibility route requires MPS out")
    if input_tensor.is_complex():
        raise NotImplementedError(
            f"MPS {op_name}.correction_out compatibility route does not support complex input"
        )
    if out.is_complex():
        raise NotImplementedError(
            f"MPS {op_name}.correction_out compatibility route does not support complex output"
        )

    computed = default_impl(input_tensor, dim, correction=correction, keepdim=keepdim)
    if not torch.can_cast(computed.dtype, out.dtype):
        raise RuntimeError(
            f"result type {computed.dtype} can't be cast to the desired output type {out.dtype}"
        )
    if out.numel() > 0 and _has_definite_internal_overlap(out):
        raise RuntimeError(
            "unsupported operation: more than one element of the written-to tensor "
            "refers to a single memory location. Please clone() the tensor before "
            "performing the operation."
        )

    if out.shape != computed.shape:
        out.resize_(computed.shape)
    out.copy_(computed)
    return out


def _check_take_indices_mps(index: torch.Tensor, input_numel: int) -> None:
    # MPS gather currently does not reliably bounds-check. Keep this validation
    # before the gather so bad indices fail instead of producing silent garbage.
    if index.numel() == 0:
        return

    message = "take(): MPS compatibility route index out of range"
    valid = (index >= -input_numel) & (index < input_numel)
    checker = getattr(torch, "_check_tensor_all", None)
    if checker is not None:
        try:
            # `_check_tensor_all` raises RuntimeError for false tensor predicates
            # on torch 2.10; keep the conversion narrow so unrelated MPS errors
            # are not mislabeled as indexing errors.
            checker(valid, lambda: message)
        except RuntimeError as exc:
            if message not in str(exc):
                raise
            raise IndexError(message) from None
    elif not bool(valid.all().item()):
        raise IndexError(message)


def _take_mps(input_tensor: torch.Tensor, index: torch.Tensor) -> torch.Tensor:
    if input_tensor.device.type != "mps":
        raise RuntimeError("take MPS compatibility route requires input to be on MPS")
    if index.device.type != "mps":
        raise RuntimeError("take MPS compatibility route requires index to be on MPS")
    if index.dtype != torch.int64:
        raise RuntimeError(f"take(): Expected a long tensor for index, but got {index.dtype}")
    if input_tensor.is_complex():
        raise NotImplementedError("MPS take compatibility route does not support complex tensors")

    if index.numel() == 0:
        return input_tensor.new_empty(tuple(index.shape))

    input_numel = input_tensor.numel()
    _check_take_indices_mps(index, input_numel)
    normalized = torch.where(index < 0, index + input_numel, index)
    gathered = torch.gather(input_tensor.reshape(-1), 0, normalized.reshape(-1))
    return gathered.reshape(index.shape)


def _copy_take_out(
    *,
    input_tensor: torch.Tensor,
    index: torch.Tensor,
    out: torch.Tensor,
) -> torch.Tensor:
    if index.dtype != torch.int64:
        raise RuntimeError(f"take(): Expected a long tensor for index, but got {index.dtype}")
    if out.device.type != "mps":
        raise RuntimeError("take.out MPS compatibility route requires MPS out")
    if out.dtype != input_tensor.dtype:
        raise RuntimeError(
            "take(): self and out expected to have the same dtype, "
            f"but got self.dtype = {input_tensor.dtype} and out.dtype = {out.dtype}"
        )

    computed = _take_mps(input_tensor, index)
    if out.numel() > 0 and _has_definite_internal_overlap(out):
        raise RuntimeError(
            "unsupported operation: more than one element of the written-to tensor "
            "refers to a single memory location. Please clone() the tensor before "
            "performing the operation."
        )
    for source in (input_tensor, index):
        if torch._C._overlaps(out, source):
            raise RuntimeError(
                "unsupported operation: some elements of the input tensor and the written-to "
                "tensor refer to a single memory location. Please clone() the tensor before "
                "performing the operation."
            )
    if out.shape != computed.shape:
        out.resize_(computed.shape)
    out.copy_(computed)
    return out


def _require_mps_tensor(
    *,
    op_name: str,
    tensor: torch.Tensor,
    arg_name: str,
) -> None:
    if tensor.device.type != "mps":
        raise RuntimeError(
            f"{op_name} MPS compatibility route requires {arg_name} to be on MPS"
        )


def _addmm_activation_decomposition_mps(
    input_tensor: torch.Tensor,
    mat1: torch.Tensor,
    mat2: torch.Tensor,
    *,
    beta: Any = 1,
    alpha: Any = 1,
    use_gelu: bool = False,
) -> torch.Tensor:
    _require_mps_tensor(op_name="_addmm_activation", tensor=input_tensor, arg_name="input")
    _require_mps_tensor(op_name="_addmm_activation", tensor=mat1, arg_name="mat1")
    _require_mps_tensor(op_name="_addmm_activation", tensor=mat2, arg_name="mat2")

    computed = torch.addmm(input_tensor, mat1, mat2, beta=beta, alpha=alpha)
    if use_gelu:
        return torch.nn.functional.gelu(computed, approximate="none")
    return torch.relu(computed)


class _AddmmActivationAutogradNotImplemented(torch.autograd.Function):
    @staticmethod
    def forward(  # noqa: ANN001
        ctx,
        input_tensor,
        mat1,
        mat2,
        beta,
        alpha,
        use_gelu,
    ):
        return _addmm_activation_decomposition_mps(
            input_tensor,
            mat1,
            mat2,
            beta=beta,
            alpha=alpha,
            use_gelu=use_gelu,
        )

    @staticmethod
    def backward(ctx, grad_output):  # noqa: ANN001, ARG004
        raise RuntimeError("derivative for aten::_addmm_activation is not implemented")


def _setup_addmm_activation_autograd(ctx, inputs, keyword_only_inputs, output):  # noqa: ANN001, ARG001
    input_tensor, mat1, mat2 = inputs
    ctx.save_for_backward(input_tensor, mat1, mat2)
    ctx.beta = keyword_only_inputs.get("beta", 1)
    ctx.alpha = keyword_only_inputs.get("alpha", 1)
    ctx.use_gelu = keyword_only_inputs.get("use_gelu", False)


def _addmm_activation_autograd_backward(ctx, grad_output):  # noqa: ANN001
    input_tensor, mat1, mat2 = ctx.saved_tensors
    beta = ctx.beta
    alpha = ctx.alpha
    z = torch.addmm(input_tensor, mat1, mat2, beta=beta, alpha=alpha)

    if ctx.use_gelu:
        grad_z = torch.ops.aten.gelu_backward.default(
            grad_output,
            z,
            approximate="none",
        )
    else:
        grad_z = torch.where(z > 0, grad_output, torch.zeros_like(grad_output))

    grad_input = (grad_z * beta).sum_to_size(tuple(input_tensor.shape))
    grad_mat1 = (grad_z @ mat2.transpose(-2, -1)) * alpha
    grad_mat2 = (mat1.transpose(-2, -1) @ grad_z) * alpha
    return grad_input, grad_mat1, grad_mat2


def _addmm_activation_mps(
    input_tensor: torch.Tensor,
    mat1: torch.Tensor,
    mat2: torch.Tensor,
    *,
    beta: Any = 1,
    alpha: Any = 1,
    use_gelu: bool = False,
) -> torch.Tensor:
    if _env_enabled("PSYCHE_MPS_COMPAT_ADDMM_ACTIVATION_GRAD"):
        return _addmm_activation_decomposition_mps(
            input_tensor,
            mat1,
            mat2,
            beta=beta,
            alpha=alpha,
            use_gelu=use_gelu,
        )
    if torch.is_grad_enabled() and any(
        tensor.requires_grad for tensor in (input_tensor, mat1, mat2)
    ):
        return _AddmmActivationAutogradNotImplemented.apply(
            input_tensor,
            mat1,
            mat2,
            beta,
            alpha,
            use_gelu,
        )
    return _addmm_activation_decomposition_mps(
        input_tensor,
        mat1,
        mat2,
        beta=beta,
        alpha=alpha,
        use_gelu=use_gelu,
    )


def _copy_addmm_activation_out(
    *,
    input_tensor: torch.Tensor,
    mat1: torch.Tensor,
    mat2: torch.Tensor,
    beta: Any,
    alpha: Any,
    use_gelu: bool,
    out: torch.Tensor,
) -> torch.Tensor:
    if out.device.type != "mps":
        raise RuntimeError("_addmm_activation.out MPS compatibility route requires MPS out")
    if out.numel() > 0 and _has_definite_internal_overlap(out):
        raise RuntimeError(
            "unsupported operation: more than one element of the written-to tensor "
            "refers to a single memory location. Please clone() the tensor before "
            "performing the operation."
        )
    for source in (input_tensor, mat1, mat2):
        if torch._C._overlaps(out, source):
            raise RuntimeError(
                "unsupported operation: some elements of the input tensor and the written-to "
                "tensor refer to a single memory location. Please clone() the tensor before "
                "performing the operation."
            )

    computed = _addmm_activation_mps(
        input_tensor,
        mat1,
        mat2,
        beta=beta,
        alpha=alpha,
        use_gelu=use_gelu,
    )
    if out.dtype != computed.dtype:
        raise RuntimeError(
            f"Expected out tensor to have dtype {computed.dtype}, but got {out.dtype}"
        )
    if out.shape != computed.shape:
        out.resize_(computed.shape)
    out.copy_(computed)
    return out


def _channel_shuffle_mps(input_tensor: torch.Tensor, groups: int) -> torch.Tensor:
    _require_mps_tensor(op_name="channel_shuffle", tensor=input_tensor, arg_name="input")
    if input_tensor.dim() <= 2:
        raise RuntimeError(
            "channel_shuffle expects input with > 2 dims, "
            f"but got input with sizes {list(input_tensor.size())}"
        )
    group_count = int(groups)
    if group_count <= 0:
        raise RuntimeError(
            "Number of groups to divide channels in must be positive. "
            f"Value of groups:{group_count}"
        )
    channels = input_tensor.shape[1]
    if channels % group_count != 0:
        raise RuntimeError(
            f"Number of channels must be divisible by groups. Got {channels} "
            f"channels and {group_count} groups."
        )
    if input_tensor.numel() == 0:
        return input_tensor.reshape(tuple(input_tensor.shape))

    batch = input_tensor.shape[0]
    channels_per_group = channels // group_count
    rest = tuple(input_tensor.shape[2:])
    return (
        input_tensor.reshape(batch, group_count, channels_per_group, *rest)
        .transpose(1, 2)
        .reshape(tuple(input_tensor.shape))
        .contiguous()
    )


def _normalize_mps_factory_device(device: Any, op_name: str) -> torch.device:
    if device is None:
        return torch.device("mps")
    normalized = torch.device(device)
    if normalized.type != "mps":
        raise RuntimeError(f"{op_name} MPS compatibility route requires an MPS device")
    return normalized


def _validate_logspace_dtype(dtype: Optional[torch.dtype]) -> torch.dtype:
    result_dtype = torch.get_default_dtype() if dtype is None else dtype
    if result_dtype not in _MPS_REAL_FLOAT_DTYPES:
        raise NotImplementedError(
            "MPS logspace compatibility route only supports float16, "
            "bfloat16, and float32 dtypes"
        )
    return result_dtype


def _is_complex_logspace_value(value: Any) -> bool:
    if isinstance(value, torch.Tensor):
        return value.is_complex()
    return isinstance(value, complex)


def _logspace_scalar_float(value: Any) -> float:
    if isinstance(value, torch.Tensor):
        if value.numel() != 1:
            raise NotImplementedError("MPS logspace compatibility route requires scalar endpoints")
        value = value.detach().cpu().item()
    return float(value)


def _integer_logspace_grid(start: Any, end: Any, steps: int) -> bool:
    # Check the symbolic endpoint/step grid, not the materialized MPS linspace:
    # CPU and MPS can round fractional grids differently, changing NaN positions
    # for negative-base pow.
    if steps == 0:
        return True
    start_value = _logspace_scalar_float(start)
    end_value = _logspace_scalar_float(end)
    if not (math.isfinite(start_value) and math.isfinite(end_value)):
        return False
    if steps == 1:
        return start_value.is_integer()
    step = (end_value - start_value) / (steps - 1)
    return start_value.is_integer() and step.is_integer()


def _logspace_mps(
    start: Any,
    end: Any,
    steps: int,
    base: float = 10.0,
    *,
    dtype: Optional[torch.dtype] = None,
    layout: Optional[torch.layout] = None,
    device: Any = None,
    pin_memory: Optional[bool] = None,
) -> torch.Tensor:
    if layout is not None and layout != torch.strided:
        raise NotImplementedError("MPS logspace compatibility route only supports strided layout")
    if pin_memory:
        raise RuntimeError("Need to provide pin_memory allocator to use pin memory.")
    steps = int(steps)
    if steps < 0:
        raise RuntimeError("number of steps must be non-negative")
    if _is_complex_logspace_value(start) or _is_complex_logspace_value(end):
        raise NotImplementedError("MPS logspace compatibility route does not support complex endpoints")
    if base < 0 and not _integer_logspace_grid(start, end, steps):
        raise NotImplementedError(
            "MPS logspace compatibility route only supports negative bases "
            "when every generated exponent is integer-valued; fractional exponent "
            "grids can change NaN positions because CPU and MPS linspace round differently"
        )

    result_dtype = _validate_logspace_dtype(dtype)
    target_device = _normalize_mps_factory_device(device, "logspace")
    exponents = torch.linspace(
        start,
        end,
        steps,
        dtype=result_dtype,
        device=target_device,
    )
    return torch.pow(base, exponents)


def _copy_unary_out(
    *,
    op_name: str,
    computed: torch.Tensor,
    out: torch.Tensor,
    inputs: tuple[torch.Tensor, ...],
    exact_dtype: bool = True,
) -> torch.Tensor:
    if out.device.type != "mps":
        raise RuntimeError(f"{op_name}.out MPS compatibility route requires MPS out")
    if exact_dtype and out.dtype != computed.dtype:
        raise RuntimeError(
            f"Expected out tensor to have dtype {computed.dtype}, but got {out.dtype}"
        )
    if not exact_dtype and not torch.can_cast(computed.dtype, out.dtype):
        raise RuntimeError(
            f"result type {computed.dtype} can't be cast to the desired output type {out.dtype}"
        )
    if out.numel() > 0 and _has_definite_internal_overlap(out):
        raise RuntimeError(
            "unsupported operation: more than one element of the written-to tensor "
            "refers to a single memory location. Please clone() the tensor before "
            "performing the operation."
        )
    for source in inputs:
        if torch._C._overlaps(out, source):
            raise RuntimeError(
                "unsupported operation: some elements of the input tensor and the written-to "
                "tensor refer to a single memory location. Please clone() the tensor before "
                "performing the operation."
            )
    if out.shape != computed.shape:
        out.resize_(computed.shape)
    out.copy_(computed)
    return out


def _copy_logspace_out(
    start: Any,
    end: Any,
    steps: int,
    base: float,
    *,
    out: torch.Tensor,
) -> torch.Tensor:
    computed = _logspace_mps(
        start,
        end,
        steps,
        base=base,
        dtype=out.dtype,
        device=out.device,
    )
    return _copy_unary_out(
        op_name="logspace",
        computed=computed,
        out=out,
        inputs=(),
    )


def _copy_mvlgamma_out(input_tensor: torch.Tensor, p: int, *, out: torch.Tensor) -> torch.Tensor:
    _require_mps_tensor(op_name="mvlgamma", tensor=input_tensor, arg_name="input")
    if not out.is_floating_point():
        raise RuntimeError(
            f"mvlgamma: result type Float can't be cast to the desired output type {out.dtype}"
        )
    computed = torch.mvlgamma(input_tensor, p)
    return _copy_unary_out(
        op_name="mvlgamma",
        computed=computed,
        out=out,
        inputs=(input_tensor,),
        exact_dtype=False,
    )


def _vdot_mps(input_tensor: torch.Tensor, other: torch.Tensor) -> torch.Tensor:
    _require_mps_tensor(op_name="vdot", tensor=input_tensor, arg_name="input")
    _require_mps_tensor(op_name="vdot", tensor=other, arg_name="other")
    if input_tensor.is_complex() or other.is_complex():
        raise NotImplementedError("MPS vdot compatibility route does not support complex tensors")
    if input_tensor.dtype == torch.bool or other.dtype == torch.bool:
        raise NotImplementedError('"dot" not implemented for \'Bool\'')
    return torch.dot(input_tensor, other)


def _copy_vdot_out(
    input_tensor: torch.Tensor,
    other: torch.Tensor,
    *,
    out: torch.Tensor,
) -> torch.Tensor:
    computed = _vdot_mps(input_tensor, other)
    return _copy_unary_out(
        op_name="vdot",
        computed=computed.reshape(()),
        out=out,
        inputs=(input_tensor, other),
    )


def _floor_log2_unsigned_mps(value: torch.Tensor, max_bit: int) -> torch.Tensor:
    value_i32 = value.to(torch.int32)
    result = torch.zeros_like(value_i32, dtype=torch.int32)
    for bit in range(max_bit + 1):
        result = torch.where(
            value_i32 >= (1 << bit),
            torch.full_like(result, bit),
            result,
        )
    return result


def _frexp_float32_bits_mps(input_tensor: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    bits = input_tensor.view(torch.int32)
    abs_bits = bits & 0x7FFFFFFF
    exponent_bits = (abs_bits >> 23) & 0xFF
    fraction_bits = abs_bits & 0x007FFFFF
    zero = abs_bits == 0
    special = exponent_bits == 0xFF
    normal = (exponent_bits != 0) & (~special)
    subnormal = (exponent_bits == 0) & (fraction_bits != 0)

    normal_exponent = (exponent_bits - 126).to(torch.int32)
    normal_mantissa_bits = (bits & -2147483648) | (126 << 23) | fraction_bits
    normal_mantissa = normal_mantissa_bits.view(torch.float32)

    fraction_float = fraction_bits.to(torch.float32)
    subnormal_log2 = _floor_log2_unsigned_mps(fraction_bits, 22)
    subnormal_exponent = subnormal_log2 - 148
    subnormal_scale = torch.pow(2.0, (subnormal_log2 + 1).to(torch.float32))
    sign = torch.where(
        bits < 0,
        torch.full_like(input_tensor, -1.0),
        torch.ones_like(input_tensor),
    )
    subnormal_mantissa = sign * (fraction_float / subnormal_scale)

    exponent = torch.where(
        normal,
        normal_exponent,
        torch.zeros_like(exponent_bits, dtype=torch.int32),
    )
    exponent = torch.where(subnormal, subnormal_exponent, exponent)
    mantissa = torch.where(normal, normal_mantissa, input_tensor)
    mantissa = torch.where(subnormal, subnormal_mantissa, mantissa)
    mantissa = torch.where(zero | special, input_tensor, mantissa)
    return mantissa, exponent


def _frexp_16bit_bits_mps(
    input_tensor: torch.Tensor,
    *,
    exponent_shift: int,
    exponent_mask: int,
    fraction_mask: int,
    exponent_bias: int,
) -> tuple[torch.Tensor, torch.Tensor]:
    bits = input_tensor.view(torch.int16)
    abs_bits = bits & 0x7FFF
    exponent_bits_i16 = (abs_bits >> exponent_shift) & exponent_mask
    fraction_bits_i16 = abs_bits & fraction_mask
    exponent_bits = exponent_bits_i16.to(torch.int32)
    fraction_bits = fraction_bits_i16.to(torch.int32)
    zero = abs_bits == 0
    special = exponent_bits_i16 == exponent_mask
    normal = (exponent_bits_i16 != 0) & (~special)
    subnormal = (exponent_bits_i16 == 0) & (fraction_bits_i16 != 0)

    normal_exponent = exponent_bits - (exponent_bias - 1)
    normal_mantissa_bits = (
        (bits & -32768)
        | ((exponent_bias - 1) << exponent_shift)
        | fraction_bits_i16
    )
    normal_mantissa = normal_mantissa_bits.view(input_tensor.dtype)

    fraction_float = fraction_bits.to(torch.float32)
    subnormal_log2 = _floor_log2_unsigned_mps(fraction_bits, exponent_shift - 1)
    subnormal_exponent = subnormal_log2 - (exponent_bias + exponent_shift - 2)
    subnormal_scale = torch.pow(2.0, (subnormal_log2 + 1).to(torch.float32))
    sign = torch.where(
        bits < 0,
        torch.full_like(input_tensor, -1.0),
        torch.ones_like(input_tensor),
    )
    subnormal_mantissa = (sign * (fraction_float / subnormal_scale)).to(input_tensor.dtype)

    exponent = torch.where(
        normal,
        normal_exponent,
        torch.zeros_like(exponent_bits, dtype=torch.int32),
    )
    exponent = torch.where(subnormal, subnormal_exponent, exponent)
    mantissa = torch.where(normal, normal_mantissa, input_tensor)
    mantissa = torch.where(subnormal, subnormal_mantissa, mantissa)
    mantissa = torch.where(zero | special, input_tensor, mantissa)
    return mantissa, exponent


def _frexp_mps_no_autograd(input_tensor: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    _require_mps_tensor(op_name="frexp", tensor=input_tensor, arg_name="input")
    if input_tensor.dtype not in _MPS_REAL_FLOAT_DTYPES:
        raise NotImplementedError(
            "MPS frexp compatibility route only supports float16, bfloat16, and float32"
    )
    if input_tensor.dtype == torch.float32:
        return _frexp_float32_bits_mps(input_tensor)
    if input_tensor.dtype == torch.float16:
        return _frexp_16bit_bits_mps(
            input_tensor,
            exponent_shift=10,
            exponent_mask=0x1F,
            fraction_mask=0x03FF,
            exponent_bias=15,
        )
    return _frexp_16bit_bits_mps(
        input_tensor,
        exponent_shift=7,
        exponent_mask=0xFF,
        fraction_mask=0x007F,
        exponent_bias=127,
    )


class _FrexpMps(torch.autograd.Function):
    @staticmethod
    def forward(ctx: Any, input_tensor: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        mantissa, exponent = _frexp_mps_no_autograd(input_tensor)
        ctx.save_for_backward(exponent)
        ctx.mark_non_differentiable(exponent)
        return mantissa, exponent

    @staticmethod
    def backward(
        ctx: Any,
        grad_mantissa: torch.Tensor,
        grad_exponent: torch.Tensor,
    ) -> tuple[torch.Tensor]:  # noqa: ARG004
        (exponent,) = ctx.saved_tensors
        scale = torch.pow(2.0, -exponent.to(torch.float32))
        return ((grad_mantissa.float() * scale).to(grad_mantissa.dtype),)


def _frexp_mps(input_tensor: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    if torch.is_grad_enabled() and input_tensor.requires_grad:
        return _FrexpMps.apply(input_tensor)
    return _frexp_mps_no_autograd(input_tensor)


def _validate_frexp_out(
    input_tensor: torch.Tensor,
    mantissa: torch.Tensor,
    exponent: torch.Tensor,
) -> None:
    if mantissa.device.type != "mps" or exponent.device.type != "mps":
        raise RuntimeError("frexp.out MPS compatibility route requires MPS outputs")
    if mantissa.dtype != input_tensor.dtype:
        raise RuntimeError(
            f"torch.frexp() expects mantissa to have dtype {input_tensor.dtype} "
            f"but got {mantissa.dtype}"
        )
    if exponent.dtype != torch.int32:
        raise RuntimeError(f"torch.frexp() expects exponent to have int dtype but got {exponent.dtype}")
    for out in (mantissa, exponent):
        if out.numel() > 0 and _has_definite_internal_overlap(out):
            raise RuntimeError(
                "unsupported operation: more than one element of the written-to tensor "
                "refers to a single memory location. Please clone() the tensor before "
                "performing the operation."
            )
    if torch._C._overlaps(mantissa, input_tensor) and not _same_logical_tensor(
        mantissa,
        input_tensor,
    ):
        raise RuntimeError(
            "unsupported operation: some elements of the input tensor and the written-to "
            "tensor refer to a single memory location. Please clone() the tensor before "
            "performing the operation."
        )
    if torch._C._overlaps(exponent, input_tensor) or torch._C._overlaps(mantissa, exponent):
        raise RuntimeError(
            "unsupported operation: output tensors must not overlap input or each other"
        )


def _copy_frexp_out(
    input_tensor: torch.Tensor,
    *,
    mantissa: torch.Tensor,
    exponent: torch.Tensor,
) -> tuple[torch.Tensor, torch.Tensor]:
    computed_mantissa, computed_exponent = _frexp_mps(input_tensor)
    _validate_frexp_out(input_tensor, mantissa, exponent)
    if mantissa.shape != computed_mantissa.shape:
        mantissa.resize_(computed_mantissa.shape)
    if exponent.shape != computed_exponent.shape:
        exponent.resize_(computed_exponent.shape)
    mantissa.copy_(computed_mantissa)
    exponent.copy_(computed_exponent)
    return mantissa, exponent


class _LogitInplaceMps(torch.autograd.Function):
    @staticmethod
    def forward(ctx: Any, input_tensor: torch.Tensor, eps: Optional[float]) -> torch.Tensor:
        original = input_tensor.clone()
        ctx.save_for_backward(original)
        ctx.eps = eps
        ctx.mark_dirty(input_tensor)
        input_tensor.copy_(_logit_cpu_eps_semantics_mps(original, eps))
        return input_tensor

    @staticmethod
    def backward(ctx: Any, grad_output: torch.Tensor) -> tuple[torch.Tensor, None]:
        (original,) = ctx.saved_tensors
        eps = ctx.eps
        grad = grad_output / (original * (1 - original))

        if eps is None or eps < 0:
            in_domain = (original >= 0) & (original <= 1)
            grad = torch.where(in_domain, grad, torch.full_like(grad, float("nan")))
            return grad, None

        active = (original >= eps) & (original <= 1 - eps)
        grad = torch.where(active, grad, torch.zeros_like(grad))
        return grad, None


def _logit_inplace_mps(input_tensor: torch.Tensor, eps: Optional[float] = None) -> torch.Tensor:
    if input_tensor.device.type != "mps":
        raise RuntimeError("logit_ MPS compatibility route requires input to be on MPS")
    if input_tensor.is_complex():
        raise NotImplementedError("MPS logit_ compatibility route does not support complex tensors")
    if input_tensor.dtype == torch.float64:
        raise RuntimeError("MPS logit_ compatibility route does not support float64 tensors")
    if not input_tensor.is_floating_point():
        raise RuntimeError(
            f"result type Float can't be cast to the desired output type {input_tensor.dtype}"
        )
    if eps is not None and eps > 0.5:
        raise RuntimeError(
            "MPS logit_ compatibility route does not support eps > 0.5 because "
            "PyTorch CPU behavior is vectorization-dependent on this stack"
        )

    if input_tensor.requires_grad and torch.is_grad_enabled():
        return _LogitInplaceMps.apply(input_tensor, eps)

    input_tensor.copy_(_logit_cpu_eps_semantics_mps(input_tensor.clone(), eps))
    return input_tensor


def _logit_cpu_eps_semantics_mps(
    input_tensor: torch.Tensor,
    eps: Optional[float],
) -> torch.Tensor:
    if eps is not None and eps >= 0:
        lower = torch.full((), eps, dtype=input_tensor.dtype, device=input_tensor.device)
        upper = torch.full((), 1.0 - eps, dtype=input_tensor.dtype, device=input_tensor.device)
        clamped = torch.where(
            input_tensor < lower,
            lower,
            torch.where(input_tensor > upper, upper, input_tensor),
        )
    else:
        clamped = input_tensor
    return torch.log(clamped / (1 - clamped))


def _gcd_mps(input_tensor: torch.Tensor, other: torch.Tensor) -> torch.Tensor:
    if input_tensor.dtype not in _INTEGER_DTYPES or other.dtype not in _INTEGER_DTYPES:
        raise NotImplementedError("MPS gcd compatibility route only supports integer tensors")
    if input_tensor.device != other.device:
        raise RuntimeError(
            "Expected all tensors to be on the same device, but found at least two devices, "
            f"{input_tensor.device.type} and {other.device.type}!"
        )

    result_dtype = torch.result_type(input_tensor, other)
    if result_dtype not in _GCD_RESULT_DTYPES:
        raise NotImplementedError(f"MPS gcd compatibility route does not support {result_dtype}")

    a, b = torch.broadcast_tensors(
        input_tensor.to(result_dtype),
        other.to(result_dtype),
    )
    a = _integer_abs_cpu_like(a)
    b = _integer_abs_cpu_like(b)
    use_fmod, final_abs = _GCD_RESULT_DTYPES[result_dtype]

    for _ in range(_GCD_ITERATIONS[result_dtype]):
        safe_b = torch.where(b == 0, torch.ones_like(b), b)
        remainder = torch.fmod(a, safe_b) if use_fmod else torch.remainder(a, safe_b)
        active = b != 0
        a, b = torch.where(active, b, a), torch.where(active, remainder, b)

    return _integer_abs_cpu_like(a) if final_abs else a


def _lcm_mps(input_tensor: torch.Tensor, other: torch.Tensor) -> torch.Tensor:
    if input_tensor.dtype not in _INTEGER_DTYPES or other.dtype not in _INTEGER_DTYPES:
        raise NotImplementedError("MPS lcm compatibility route only supports integer tensors")
    if input_tensor.device != other.device:
        raise RuntimeError(
            "Expected all tensors to be on the same device, but found at least two devices, "
            f"{input_tensor.device.type} and {other.device.type}!"
        )

    result_dtype = torch.result_type(input_tensor, other)
    if result_dtype not in _GCD_RESULT_DTYPES:
        raise NotImplementedError(f"MPS lcm compatibility route does not support {result_dtype}")

    a, b = torch.broadcast_tensors(
        input_tensor.to(result_dtype),
        other.to(result_dtype),
    )
    divisor = _integer_abs_cpu_like(_gcd_mps(a, b))
    safe_divisor = torch.where(divisor == 0, torch.ones_like(divisor), divisor)
    left_abs = _integer_abs_cpu_like(a)
    right_abs = _integer_abs_cpu_like(b)
    raw = torch.div(left_abs, safe_divisor, rounding_mode="trunc") * right_abs

    # CPU parity quirk: int8/int16 preserve wrapped signed overflow artifacts,
    # while int32/int64 normalize the final sign on the tested torch stack.
    if result_dtype in (torch.int32, torch.int64):
        raw = _integer_abs_cpu_like(raw)

    return torch.where((a == 0) | (b == 0), torch.zeros_like(raw), raw)


def _approx_linalg_svd_mps(
    matrix: torch.Tensor,
    *,
    full_matrices: bool,
    iterations: int,
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    if iterations <= 0:
        raise ValueError(f"iterations must be positive, got {iterations}")
    if matrix.ndim != 2:
        raise NotImplementedError("experimental MPS SVD route currently handles 2D tensors only")
    if matrix.is_complex():
        raise NotImplementedError("experimental MPS SVD route currently handles real tensors only")
    if matrix.requires_grad:
        raise NotImplementedError("experimental MPS SVD route does not support autograd")
    if matrix.numel() == 0:
        raise NotImplementedError("experimental MPS SVD route does not support empty matrices")
    if matrix.dtype not in (torch.float16, torch.bfloat16, torch.float32):
        raise NotImplementedError(f"experimental MPS SVD route does not support {matrix.dtype}")
    if full_matrices:
        raise NotImplementedError("experimental MPS SVD route supports full_matrices=False only")

    rows, cols = matrix.shape
    rank = min(rows, cols)

    work = matrix.float()
    gram = work.mT @ work
    right_vectors: list[torch.Tensor] = []
    left_vectors: list[torch.Tensor] = []
    singular_values: list[torch.Tensor] = []

    for index in range(rank):
        vector = torch.zeros(cols, device=matrix.device, dtype=torch.float32)
        vector[index % cols] = 1.0
        vector = vector + 0.01 * torch.linspace(
            0.0,
            1.0,
            cols,
            device=matrix.device,
            dtype=torch.float32,
        )

        for _ in range(iterations):
            vector = gram @ vector
            if right_vectors:
                basis = torch.stack(right_vectors, dim=1)
                vector = vector - basis @ (basis.mT @ vector)
            vector = vector / _safe_vector_norm(vector)

        projected = work @ vector
        singular_value = _safe_vector_norm(projected)
        left_vector = projected / torch.clamp(singular_value, min=1e-12)

        right_vectors.append(vector)
        left_vectors.append(left_vector)
        singular_values.append(singular_value)

        outer = vector[:, None] @ vector[None, :]
        gram = gram - singular_value.pow(2) * outer

    u = torch.stack(left_vectors, dim=1).to(matrix.dtype)
    s = torch.stack(singular_values).to(matrix.dtype)
    v = torch.stack(right_vectors, dim=1).to(matrix.dtype)
    return u, s, v.mT


def approximate_linalg_svd_mps(
    matrix: torch.Tensor,
    *,
    full_matrices: bool = False,
    iterations: int = 64,
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """Experimental GPU-resident SVD helper.

    This is not registered as `aten::linalg_svd` because it is approximate and
    does not provide PyTorch-compatible SVD autograd semantics.
    """

    return _approx_linalg_svd_mps(
        matrix,
        full_matrices=full_matrices,
        iterations=iterations,
    )


def matrix_exp_mps(matrix: torch.Tensor) -> torch.Tensor:
    """Experimental MPS-resident matrix exponential helper."""

    return _linalg_matrix_exp_mps(matrix)


def qr_mps(
    matrix: torch.Tensor,
    *,
    mode: str = "reduced",
) -> tuple[torch.Tensor, torch.Tensor]:
    """Experimental MPS-resident QR helper.

    Uses modified Gram-Schmidt over MPS primitives. This is separately gated
    because it does not expose PyTorch's GEQRF/Householder tau representation,
    and QR sign choices are not bitwise-stable across valid algorithms.
    """

    return _modified_gram_schmidt_qr_mps(matrix, mode)


@dataclass
class MpsCompatStats:
    replacements: dict[str, int] = field(default_factory=dict)

    def record(self, op: str) -> None:
        self.replacements[op] = self.replacements.get(op, 0) + 1


@dataclass
class MpsCompatInstallResult:
    installed: list[str] = field(default_factory=list)
    already_registered: list[str] = field(default_factory=list)
    skipped_existing_mps: list[str] = field(default_factory=list)
    disabled_by_env: list[str] = field(default_factory=list)


class MpsCompatibilityMode(TorchDispatchMode):
    """Intercept selected unsupported MPS ops and run GPU-resident replacements.

    The mode is intentionally small and opt-in. Exact production routes should
    be registered with `install_mps_compat_kernels`; this mode is for manual
    discovery and explicit experiments.
    """

    def __init__(
        self,
        *,
        allow_approximate_svd: bool = False,
        svd_iterations: int = 48,
        stats: Optional[MpsCompatStats] = None,
    ) -> None:
        super().__init__()
        self.allow_approximate_svd = allow_approximate_svd
        self.svd_iterations = svd_iterations
        self.stats = stats or MpsCompatStats()

    def __torch_dispatch__(self, func, types, args=(), kwargs=None):  # noqa: ANN001
        kwargs = kwargs or {}
        name = _op_name(func)

        if not _tree_has_mps_tensor((args, kwargs)):
            return func(*args, **kwargs)

        if name == "linalg_svd.default" and self.allow_approximate_svd:
            with torch._C._DisableTorchDispatch():
                full_matrices = bool(
                    args[1] if len(args) > 1 else kwargs.get("full_matrices", True)
                )
                driver = kwargs.get("driver")
                if driver is not None:
                    return func(*args, **kwargs)
                try:
                    result = approximate_linalg_svd_mps(
                        args[0],
                        full_matrices=full_matrices,
                        iterations=self.svd_iterations,
                    )
                except NotImplementedError:
                    if not _env_enabled("PYTORCH_ENABLE_MPS_FALLBACK"):
                        raise
                    return func(*args, **kwargs)
                self.stats.record("aten::linalg_svd")
                return result

        return func(*args, **kwargs)


def _has_mps_kernel(op_name: str) -> bool:
    try:
        return bool(torch._C._dispatch_has_kernel_for_dispatch_key(op_name, "MPS"))
    except RuntimeError:
        return False


def install_mps_compat_kernels() -> MpsCompatInstallResult:
    """Register exact opt-in compatibility routes at the MPS dispatch key.

    `TorchDispatchMode` remains useful for discovery and debugging, but the
    production path should not intercept every PyTorch op. Registering only
    specific missing kernels keeps the hot path narrow: native MPS ops go
    directly to PyTorch, and only claimed compatibility ops call these routes.
    Approximate numerical routes must stay explicit and must not be registered
    globally as aten kernels.
    """

    result = MpsCompatInstallResult()

    global _MPS_COMPAT_AUTOGRAD_LIBRARY, _MPS_COMPAT_LIBRARY
    if _MPS_COMPAT_LIBRARY is None:
        _MPS_COMPAT_LIBRARY = torch.library.Library("aten", "IMPL", "MPS")
    if _env_enabled("PSYCHE_MPS_COMPAT_ADDMM_ACTIVATION_GRAD"):
        if _ADDMM_ACTIVATION_AUTOGRAD in _REGISTERED_KERNELS:
            result.already_registered.append(_ADDMM_ACTIVATION_AUTOGRAD)
        else:
            if _MPS_COMPAT_AUTOGRAD_LIBRARY is None:
                _MPS_COMPAT_AUTOGRAD_LIBRARY = torch.library.Library("aten", "FRAGMENT")
            autograd_info = torch_library_autograd.Info(
                _addmm_activation_autograd_backward,
                _setup_addmm_activation_autograd,
            )
            autograd_kernel = torch_library_autograd.make_autograd_impl(
                torch.ops.aten._addmm_activation.default,
                autograd_info,
            )
            _MPS_COMPAT_AUTOGRAD_LIBRARY.impl(
                "_addmm_activation",
                autograd_kernel,
                "AutogradMPS",
                with_keyset=True,
            )
            _REGISTERED_KERNELS.add(_ADDMM_ACTIVATION_AUTOGRAD)
            result.installed.append(_ADDMM_ACTIVATION_AUTOGRAD)
    else:
        result.disabled_by_env.append(_ADDMM_ACTIVATION_AUTOGRAD)

    if f"{_ADAPTIVE_AVG_POOL3D}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_ADAPTIVE_AVG_POOL3D}.default")
    elif _has_mps_kernel(_ADAPTIVE_AVG_POOL3D):
        result.skipped_existing_mps.append(f"{_ADAPTIVE_AVG_POOL3D}.default")
    else:

        def adaptive_avg_pool3d_impl(input_tensor, output_size):  # noqa: ANN001
            return _adaptive_avg_pool3d_mps(input_tensor, output_size)

        _MPS_COMPAT_LIBRARY.impl("_adaptive_avg_pool3d", adaptive_avg_pool3d_impl)
        _REGISTERED_KERNELS.add(f"{_ADAPTIVE_AVG_POOL3D}.default")
        result.installed.append(f"{_ADAPTIVE_AVG_POOL3D}.default")

    if f"{_ADAPTIVE_AVG_POOL3D_BACKWARD}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_ADAPTIVE_AVG_POOL3D_BACKWARD}.default")
    elif _has_mps_kernel(_ADAPTIVE_AVG_POOL3D_BACKWARD):
        result.skipped_existing_mps.append(f"{_ADAPTIVE_AVG_POOL3D_BACKWARD}.default")
    else:

        def adaptive_avg_pool3d_backward_impl(grad_output, input_tensor):  # noqa: ANN001
            return _adaptive_avg_pool3d_backward_mps(grad_output, input_tensor)

        _MPS_COMPAT_LIBRARY.impl(
            "_adaptive_avg_pool3d_backward",
            adaptive_avg_pool3d_backward_impl,
        )
        _REGISTERED_KERNELS.add(f"{_ADAPTIVE_AVG_POOL3D_BACKWARD}.default")
        result.installed.append(f"{_ADAPTIVE_AVG_POOL3D_BACKWARD}.default")

    if f"{_HEAVISIDE}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_HEAVISIDE}.default")
    elif _has_mps_kernel(_HEAVISIDE):
        result.skipped_existing_mps.append(f"{_HEAVISIDE}.default")
    else:

        def heaviside_impl(input_tensor, values):  # noqa: ANN001
            return _heaviside_mps(input_tensor, values)

        _MPS_COMPAT_LIBRARY.impl("heaviside", heaviside_impl)
        _REGISTERED_KERNELS.add(f"{_HEAVISIDE}.default")
        result.installed.append(f"{_HEAVISIDE}.default")

    if _HEAVISIDE_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_HEAVISIDE_OUT)
    elif _has_mps_kernel(_HEAVISIDE_OUT):
        result.skipped_existing_mps.append(_HEAVISIDE_OUT)
    else:

        def heaviside_out_impl(input_tensor, values, *, out):  # noqa: ANN001
            computed = _heaviside_mps(input_tensor, values)
            if out.device.type != "mps":
                raise RuntimeError("heaviside.out MPS compatibility route requires MPS out")
            if out.dtype != computed.dtype:
                raise RuntimeError("heaviside is not yet implemented for tensors with different dtypes.")
            if out.shape != computed.shape:
                out.resize_(computed.shape)
            out.copy_(computed)
            return out

        _MPS_COMPAT_LIBRARY.impl("heaviside.out", heaviside_out_impl)
        _REGISTERED_KERNELS.add(_HEAVISIDE_OUT)
        result.installed.append(_HEAVISIDE_OUT)

    if f"{_GCD}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_GCD}.default")
    elif _has_mps_kernel(_GCD):
        result.skipped_existing_mps.append(f"{_GCD}.default")
    else:

        def gcd_impl(input_tensor, other):  # noqa: ANN001
            return _gcd_mps(input_tensor, other)

        _MPS_COMPAT_LIBRARY.impl("gcd", gcd_impl)
        _REGISTERED_KERNELS.add(f"{_GCD}.default")
        result.installed.append(f"{_GCD}.default")

    if _GCD_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_GCD_OUT)
    elif _has_mps_kernel(_GCD_OUT):
        result.skipped_existing_mps.append(_GCD_OUT)
    else:

        def gcd_out_impl(input_tensor, other, *, out):  # noqa: ANN001
            computed = _gcd_mps(input_tensor, other)
            _prepare_integer_out(
                op_name="gcd",
                computed=computed,
                out=out,
                inputs=(input_tensor, other),
            )
            out.copy_(computed)
            return out

        _MPS_COMPAT_LIBRARY.impl("gcd.out", gcd_out_impl)
        _REGISTERED_KERNELS.add(_GCD_OUT)
        result.installed.append(_GCD_OUT)

    if f"{_LCM}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_LCM}.default")
    elif _has_mps_kernel(_LCM):
        result.skipped_existing_mps.append(f"{_LCM}.default")
    else:

        def lcm_impl(input_tensor, other):  # noqa: ANN001
            return _lcm_mps(input_tensor, other)

        _MPS_COMPAT_LIBRARY.impl("lcm", lcm_impl)
        _REGISTERED_KERNELS.add(f"{_LCM}.default")
        result.installed.append(f"{_LCM}.default")

    if _LCM_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_LCM_OUT)
    elif _has_mps_kernel(_LCM_OUT):
        result.skipped_existing_mps.append(_LCM_OUT)
    else:

        def lcm_out_impl(input_tensor, other, *, out):  # noqa: ANN001
            computed = _lcm_mps(input_tensor, other)
            _prepare_integer_out(
                op_name="lcm",
                computed=computed,
                out=out,
                inputs=(input_tensor, other),
            )
            out.copy_(computed)
            return out

        _MPS_COMPAT_LIBRARY.impl("lcm.out", lcm_out_impl)
        _REGISTERED_KERNELS.add(_LCM_OUT)
        result.installed.append(_LCM_OUT)

    if _STD_CORRECTION_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_STD_CORRECTION_OUT)
    elif _has_mps_kernel(_STD_CORRECTION_OUT):
        result.skipped_existing_mps.append(_STD_CORRECTION_OUT)
    elif not _has_mps_kernel(_STD_CORRECTION):
        result.disabled_by_env.append(_STD_CORRECTION_OUT)
    else:

        def std_correction_out_impl(input_tensor, dim=None, *, correction=None, keepdim=False, out):  # noqa: ANN001
            return _copy_reduction_out(
                op_name="std",
                default_impl=torch.ops.aten.std.correction,
                input_tensor=input_tensor,
                dim=dim,
                correction=correction,
                keepdim=keepdim,
                out=out,
            )

        _MPS_COMPAT_LIBRARY.impl("std.correction_out", std_correction_out_impl)
        _REGISTERED_KERNELS.add(_STD_CORRECTION_OUT)
        result.installed.append(_STD_CORRECTION_OUT)

    if _VAR_CORRECTION_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_VAR_CORRECTION_OUT)
    elif _has_mps_kernel(_VAR_CORRECTION_OUT):
        result.skipped_existing_mps.append(_VAR_CORRECTION_OUT)
    elif not _has_mps_kernel(_VAR_CORRECTION):
        result.disabled_by_env.append(_VAR_CORRECTION_OUT)
    else:

        def var_correction_out_impl(input_tensor, dim=None, *, correction=None, keepdim=False, out):  # noqa: ANN001
            return _copy_reduction_out(
                op_name="var",
                default_impl=torch.ops.aten.var.correction,
                input_tensor=input_tensor,
                dim=dim,
                correction=correction,
                keepdim=keepdim,
                out=out,
            )

        _MPS_COMPAT_LIBRARY.impl("var.correction_out", var_correction_out_impl)
        _REGISTERED_KERNELS.add(_VAR_CORRECTION_OUT)
        result.installed.append(_VAR_CORRECTION_OUT)

    if f"{_TAKE}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_TAKE}.default")
    elif _has_mps_kernel(_TAKE):
        result.skipped_existing_mps.append(f"{_TAKE}.default")
    else:

        def take_impl(input_tensor, index):  # noqa: ANN001
            return _take_mps(input_tensor, index)

        _MPS_COMPAT_LIBRARY.impl("take", take_impl)
        _REGISTERED_KERNELS.add(f"{_TAKE}.default")
        result.installed.append(f"{_TAKE}.default")

    if _TAKE_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_TAKE_OUT)
    elif _has_mps_kernel(_TAKE_OUT):
        result.skipped_existing_mps.append(_TAKE_OUT)
    else:

        def take_out_impl(input_tensor, index, *, out):  # noqa: ANN001
            return _copy_take_out(input_tensor=input_tensor, index=index, out=out)

        _MPS_COMPAT_LIBRARY.impl("take.out", take_out_impl)
        _REGISTERED_KERNELS.add(_TAKE_OUT)
        result.installed.append(_TAKE_OUT)

    if _LOGIT_INPLACE in _REGISTERED_KERNELS:
        result.already_registered.append(_LOGIT_INPLACE)
    elif _has_mps_kernel(_LOGIT_INPLACE):
        result.skipped_existing_mps.append(_LOGIT_INPLACE)
    else:

        def logit_inplace_impl(input_tensor, eps=None):  # noqa: ANN001
            return _logit_inplace_mps(input_tensor, eps=eps)

        _MPS_COMPAT_LIBRARY.impl("logit_", logit_inplace_impl)
        _REGISTERED_KERNELS.add(_LOGIT_INPLACE)
        result.installed.append(_LOGIT_INPLACE)

    if f"{_ADDMM_ACTIVATION}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_ADDMM_ACTIVATION}.default")
    elif _has_mps_kernel(_ADDMM_ACTIVATION):
        result.skipped_existing_mps.append(f"{_ADDMM_ACTIVATION}.default")
    else:

        def addmm_activation_impl(
            input_tensor,
            mat1,
            mat2,
            *,
            beta=1,
            alpha=1,
            use_gelu=False,
        ):  # noqa: ANN001
            return _addmm_activation_mps(
                input_tensor,
                mat1,
                mat2,
                beta=beta,
                alpha=alpha,
                use_gelu=use_gelu,
            )

        _MPS_COMPAT_LIBRARY.impl("_addmm_activation", addmm_activation_impl)
        _REGISTERED_KERNELS.add(f"{_ADDMM_ACTIVATION}.default")
        result.installed.append(f"{_ADDMM_ACTIVATION}.default")

    if _ADDMM_ACTIVATION_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_ADDMM_ACTIVATION_OUT)
    elif _has_mps_kernel(_ADDMM_ACTIVATION_OUT):
        result.skipped_existing_mps.append(_ADDMM_ACTIVATION_OUT)
    else:

        def addmm_activation_out_impl(
            input_tensor,
            mat1,
            mat2,
            *,
            beta=1,
            alpha=1,
            use_gelu=False,
            out,
        ):  # noqa: ANN001
            return _copy_addmm_activation_out(
                input_tensor=input_tensor,
                mat1=mat1,
                mat2=mat2,
                beta=beta,
                alpha=alpha,
                use_gelu=use_gelu,
                out=out,
            )

        _MPS_COMPAT_LIBRARY.impl("_addmm_activation.out", addmm_activation_out_impl)
        _REGISTERED_KERNELS.add(_ADDMM_ACTIVATION_OUT)
        result.installed.append(_ADDMM_ACTIVATION_OUT)

    if f"{_CHANNEL_SHUFFLE}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_CHANNEL_SHUFFLE}.default")
    elif _has_mps_kernel(_CHANNEL_SHUFFLE):
        result.skipped_existing_mps.append(f"{_CHANNEL_SHUFFLE}.default")
    else:

        def channel_shuffle_impl(input_tensor, groups):  # noqa: ANN001
            return _channel_shuffle_mps(input_tensor, groups)

        _MPS_COMPAT_LIBRARY.impl("channel_shuffle", channel_shuffle_impl)
        _REGISTERED_KERNELS.add(f"{_CHANNEL_SHUFFLE}.default")
        result.installed.append(f"{_CHANNEL_SHUFFLE}.default")

    if f"{_LOGSPACE}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_LOGSPACE}.default")
    elif _has_mps_kernel(_LOGSPACE):
        result.skipped_existing_mps.append(f"{_LOGSPACE}.default")
    else:

        def logspace_impl(
            start,
            end,
            steps,
            base=10.0,
            *,
            dtype=None,
            layout=None,
            device=None,
            pin_memory=None,
        ):  # noqa: ANN001
            return _logspace_mps(
                start,
                end,
                steps,
                base=base,
                dtype=dtype,
                layout=layout,
                device=device,
                pin_memory=pin_memory,
            )

        _MPS_COMPAT_LIBRARY.impl("logspace", logspace_impl)
        _REGISTERED_KERNELS.add(f"{_LOGSPACE}.default")
        result.installed.append(f"{_LOGSPACE}.default")

    if _LOGSPACE_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_LOGSPACE_OUT)
    elif _has_mps_kernel(_LOGSPACE_OUT):
        result.skipped_existing_mps.append(_LOGSPACE_OUT)
    else:

        def logspace_out_impl(start, end, steps, base=10.0, *, out):  # noqa: ANN001
            return _copy_logspace_out(start, end, steps, base, out=out)

        _MPS_COMPAT_LIBRARY.impl("logspace.out", logspace_out_impl)
        _REGISTERED_KERNELS.add(_LOGSPACE_OUT)
        result.installed.append(_LOGSPACE_OUT)

    if _MVLGAMMA_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_MVLGAMMA_OUT)
    elif _has_mps_kernel(_MVLGAMMA_OUT):
        result.skipped_existing_mps.append(_MVLGAMMA_OUT)
    else:

        def mvlgamma_out_impl(input_tensor, p, *, out):  # noqa: ANN001
            return _copy_mvlgamma_out(input_tensor, p, out=out)

        _MPS_COMPAT_LIBRARY.impl("mvlgamma.out", mvlgamma_out_impl)
        _REGISTERED_KERNELS.add(_MVLGAMMA_OUT)
        result.installed.append(_MVLGAMMA_OUT)

    if f"{_VDOT}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_VDOT}.default")
    elif _has_mps_kernel(_VDOT):
        result.skipped_existing_mps.append(f"{_VDOT}.default")
    else:

        def vdot_impl(input_tensor, other):  # noqa: ANN001
            return _vdot_mps(input_tensor, other)

        _MPS_COMPAT_LIBRARY.impl("vdot", vdot_impl)
        _REGISTERED_KERNELS.add(f"{_VDOT}.default")
        result.installed.append(f"{_VDOT}.default")

    if _VDOT_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_VDOT_OUT)
    elif _has_mps_kernel(_VDOT_OUT):
        result.skipped_existing_mps.append(_VDOT_OUT)
    else:

        def vdot_out_impl(input_tensor, other, *, out):  # noqa: ANN001
            return _copy_vdot_out(input_tensor, other, out=out)

        _MPS_COMPAT_LIBRARY.impl("vdot.out", vdot_out_impl)
        _REGISTERED_KERNELS.add(_VDOT_OUT)
        result.installed.append(_VDOT_OUT)

    if _FREXP in _REGISTERED_KERNELS:
        result.already_registered.append(_FREXP)
    elif _has_mps_kernel(_FREXP):
        result.skipped_existing_mps.append(_FREXP)
    else:
        _MPS_COMPAT_LIBRARY.impl("frexp.Tensor", _frexp_mps)
        _REGISTERED_KERNELS.add(_FREXP)
        result.installed.append(_FREXP)

    if _FREXP_OUT in _REGISTERED_KERNELS:
        result.already_registered.append(_FREXP_OUT)
    elif _has_mps_kernel(_FREXP_OUT):
        result.skipped_existing_mps.append(_FREXP_OUT)
    else:

        def frexp_out_impl(input_tensor, *, mantissa, exponent):  # noqa: ANN001
            return _copy_frexp_out(input_tensor, mantissa=mantissa, exponent=exponent)

        _MPS_COMPAT_LIBRARY.impl("frexp.Tensor_out", frexp_out_impl)
        _REGISTERED_KERNELS.add(_FREXP_OUT)
        result.installed.append(_FREXP_OUT)

    if f"{_GEQRF}.default" in _REGISTERED_KERNELS:
        result.already_registered.append(f"{_GEQRF}.default")
    elif _has_mps_kernel(_GEQRF):
        result.skipped_existing_mps.append(f"{_GEQRF}.default")
    else:

        def geqrf_impl(input_tensor):  # noqa: ANN001
            return _geqrf_mps(input_tensor)

        _MPS_COMPAT_LIBRARY.impl("geqrf", geqrf_impl)
        _REGISTERED_KERNELS.add(f"{_GEQRF}.default")
        result.installed.append(f"{_GEQRF}.default")

    if _env_enabled("PSYCHE_MPS_COMPAT_MATRIX_EXP"):
        if f"{_LINALG_MATRIX_EXP}.default" in _REGISTERED_KERNELS:
            result.already_registered.append(f"{_LINALG_MATRIX_EXP}.default")
        elif _has_mps_kernel(_LINALG_MATRIX_EXP):
            result.skipped_existing_mps.append(f"{_LINALG_MATRIX_EXP}.default")
        else:

            def linalg_matrix_exp_impl(input_tensor):  # noqa: ANN001
                return _linalg_matrix_exp_mps(input_tensor)

            _MPS_COMPAT_LIBRARY.impl("linalg_matrix_exp", linalg_matrix_exp_impl)
            _REGISTERED_KERNELS.add(f"{_LINALG_MATRIX_EXP}.default")
            result.installed.append(f"{_LINALG_MATRIX_EXP}.default")

        if _LINALG_MATRIX_EXP_OUT in _REGISTERED_KERNELS:
            result.already_registered.append(_LINALG_MATRIX_EXP_OUT)
        elif _has_mps_kernel(_LINALG_MATRIX_EXP_OUT):
            result.skipped_existing_mps.append(_LINALG_MATRIX_EXP_OUT)
        else:

            def linalg_matrix_exp_out_impl(input_tensor, *, out):  # noqa: ANN001
                source = input_tensor.clone() if out.data_ptr() == input_tensor.data_ptr() else input_tensor
                computed = _linalg_matrix_exp_mps(source)
                if out.device.type != "mps":
                    raise RuntimeError("linalg_matrix_exp.out MPS compatibility route requires MPS out")
                if out.dtype != computed.dtype:
                    raise RuntimeError(
                        f"linalg_matrix_exp.out dtype mismatch: out={out.dtype}, result={computed.dtype}"
                    )
                if out.shape != computed.shape:
                    out.resize_(computed.shape)
                out.copy_(computed)
                return out

            _MPS_COMPAT_LIBRARY.impl("linalg_matrix_exp.out", linalg_matrix_exp_out_impl)
            _REGISTERED_KERNELS.add(_LINALG_MATRIX_EXP_OUT)
            result.installed.append(_LINALG_MATRIX_EXP_OUT)
    else:
        result.disabled_by_env.extend(
            [
                f"{_LINALG_MATRIX_EXP}.default",
                _LINALG_MATRIX_EXP_OUT,
            ]
        )

    if _env_enabled("PSYCHE_MPS_COMPAT_QR"):
        if f"{_LINALG_QR}.default" in _REGISTERED_KERNELS:
            result.already_registered.append(f"{_LINALG_QR}.default")
        elif _has_mps_kernel(_LINALG_QR):
            result.skipped_existing_mps.append(f"{_LINALG_QR}.default")
        else:

            def linalg_qr_impl(input_tensor, mode="reduced"):  # noqa: ANN001
                return _modified_gram_schmidt_qr_mps(input_tensor, mode)

            _MPS_COMPAT_LIBRARY.impl("linalg_qr", linalg_qr_impl)
            _REGISTERED_KERNELS.add(f"{_LINALG_QR}.default")
            result.installed.append(f"{_LINALG_QR}.default")

        if _LINALG_QR_OUT in _REGISTERED_KERNELS:
            result.already_registered.append(_LINALG_QR_OUT)
        elif _has_mps_kernel(_LINALG_QR_OUT):
            result.skipped_existing_mps.append(_LINALG_QR_OUT)
        else:

            def linalg_qr_out_impl(input_tensor, mode="reduced", *, Q, R):  # noqa: ANN001, N803
                computed_q, computed_r = _modified_gram_schmidt_qr_mps(input_tensor, mode)
                if Q.device.type != "mps" or R.device.type != "mps":
                    raise RuntimeError("linalg_qr.out MPS compatibility route requires MPS outputs")
                if Q.dtype != computed_q.dtype or R.dtype != computed_r.dtype:
                    raise RuntimeError(
                        "linalg_qr.out dtype mismatch: "
                        f"Q={Q.dtype}, R={R.dtype}, result=({computed_q.dtype}, {computed_r.dtype})"
                    )
                if Q.shape != computed_q.shape:
                    Q.resize_(computed_q.shape)
                if R.shape != computed_r.shape:
                    R.resize_(computed_r.shape)
                Q.copy_(computed_q)
                R.copy_(computed_r)
                return (Q, R)

            _MPS_COMPAT_LIBRARY.impl("linalg_qr.out", linalg_qr_out_impl)
            _REGISTERED_KERNELS.add(_LINALG_QR_OUT)
            result.installed.append(_LINALG_QR_OUT)
    else:
        result.disabled_by_env.extend(
            [
                f"{_LINALG_QR}.default",
                _LINALG_QR_OUT,
            ]
        )

    return result


def enable_mps_compat_kernels(device: torch.device | str | None) -> bool:
    resolved = torch.device(device) if device is not None else None
    if resolved is None or resolved.type != "mps":
        return False
    if not (
        _env_enabled("PSYCHE_MPS_COMPAT")
        or _mps_compat_enabled_for_cuda_compat()
    ):
        return False
    install_mps_compat_kernels()
    return True


def mps_compat_context(device: torch.device | str | None):
    """Install process-global MPS compatibility kernels when enabled.

    The returned context is only an integration convenience. `torch.library`
    registrations cannot be undone; once installed, the kernels remain active
    for the lifetime of the Python process.

    CUDA-shaped Apple Silicon mode (`PSYCHE_CUDA_COMPAT=1`) enables the exact MPS
    routes by default because redirected CUDA intent otherwise lands on raw MPS
    gaps. Set `PSYCHE_CUDA_COMPAT_MPS_ROUTES=0` to audit the unmodified PyTorch
    MPS surface.
    """

    enable_mps_compat_kernels(device)
    return nullcontext()
