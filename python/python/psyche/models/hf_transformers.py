import torch
import json
import os
from contextlib import nullcontext
from functools import lru_cache

from .causal_lm import CausalLM, PretrainedSourceRepoFiles, PretrainedSourceStateDict
from ..mps_compat import mps_compat_context
from transformers import (
    AutoModelForCausalLM,
    GradientCheckpointingLayer,
    PreTrainedModel,
)
from typing import Union, Iterable, Optional, Tuple
from safetensors import safe_open
from transformers.models.auto.configuration_auto import CONFIG_MAPPING
from torch.distributed import init_device_mesh
from torch.distributed.fsdp.wrap import ModuleWrapPolicy
from torch.distributed.device_mesh import DeviceMesh
from torch.distributed._composable.fsdp import fully_shard, MixedPrecisionPolicy
from torch.distributed.tensor import DTensor, Replicate, distribute_tensor
from torch.distributed.tensor.parallel import (
    parallelize_module,
    ColwiseParallel,
    RowwiseParallel,
)
from torch.distributed.algorithms._checkpoint.checkpoint_wrapper import (
    apply_activation_checkpointing,
    _CHECKPOINT_PREFIX,
)


# adapted from https://github.com/pytorch/torchtitan/blob/49c6d6fc15ef644e5c3b1003ad4e0d9ea5fcb9a9/torchtitan/parallelisms/parallel_dims.py#L48
def build_mesh(device_type, pp=1, dp_replicate=1, dp_shard=1, cp=1, tp=1) -> DeviceMesh:
    dims = []
    names = []
    for d, name in zip(
        [pp, dp_replicate, dp_shard, cp, tp],
        ["pp", "dp_replicate", "dp_shard", "cp", "tp"],
    ):
        if d > 1:
            dims.append(d)
            names.append(name)

    names = tuple(names)
    mesh = init_device_mesh(device_type, dims, mesh_dim_names=names)

    # Create all the submesh here to ensure all required process groups are
    # initialized:
    # Mesh for data loading (no communication on this mesh)
    dp_mesh_dim_names = []
    # Mesh for param sharding
    dp_shard_cp_mesh_dim_names = []
    # Mesh for loss all-reduce
    dp_cp_mesh_dim_names = []

    if dp_replicate > 1:
        dp_mesh_dim_names.append("dp_replicate")
        dp_cp_mesh_dim_names.append("dp_replicate")

    if dp_shard > 1:
        dp_mesh_dim_names.append("dp_shard")
        dp_shard_cp_mesh_dim_names.append("dp_shard")
        dp_cp_mesh_dim_names.append("dp_shard")
    if cp > 1:
        dp_shard_cp_mesh_dim_names.append("cp")
        dp_cp_mesh_dim_names.append("cp")

    if dp_mesh_dim_names != []:
        mesh[tuple(dp_mesh_dim_names)]._flatten(mesh_dim_name="dp")
    if dp_shard_cp_mesh_dim_names != []:
        mesh[tuple(dp_shard_cp_mesh_dim_names)]._flatten(mesh_dim_name="dp_shard_cp")
    if dp_cp_mesh_dim_names != []:
        mesh[tuple(dp_cp_mesh_dim_names)]._flatten(mesh_dim_name="dp_cp")

    return mesh


def auto_config_from_dict(config: dict):
    model_type = config.get("model_type")
    if model_type is None:
        raise RuntimeError("model_type not present in config.json")
    try:
        config_class = CONFIG_MAPPING[model_type]
    except KeyError:
        raise ValueError(f"Unknown model_type {model_type}")

    return config_class.from_dict(config)


def _load_repo_source_index(
    files: Iterable[str],
) -> tuple[Optional[str], dict[str, str]]:
    config_json = None
    tensor_files = {}

    for file in files:
        basename = os.path.basename(file).lower()
        if basename.endswith(".safetensors"):
            with safe_open(file, framework="pt") as f:
                metadata = f.metadata()
                if metadata is not None and metadata.get("format") != "pt":
                    raise RuntimeError("Not a PyTorch safetensors file")
                for key in f.keys():
                    if key in tensor_files:
                        raise RuntimeError(
                            f"State dict tensor {key} appears in both {tensor_files[key]} and {file}"
                        )
                    tensor_files[key] = file
        elif basename == "config.json":
            with open(file, "r", encoding="utf-8") as f:
                config_json = f.read()

    return config_json, tensor_files


def _format_tensor_names(names: Iterable[str]) -> str:
    sorted_names = sorted(names)
    sample = ", ".join(sorted_names[:20])
    remaining = len(sorted_names) - 20
    if remaining > 0:
        sample = f"{sample}, ... and {remaining} more"
    return sample


def _add_alias_pair(aliases: dict[str, list[str]], left: str, right: str):
    if left == right:
        return
    aliases.setdefault(left, [])
    aliases.setdefault(right, [])
    if right not in aliases[left]:
        aliases[left].append(right)
    if left not in aliases[right]:
        aliases[right].append(left)


def _tied_weight_aliases(model: torch.nn.Module) -> dict[str, list[str]]:
    aliases = {}
    groups: dict[int, list[str]] = {}
    for name, _param in model.named_parameters(remove_duplicate=False):
        groups.setdefault(id(_param), []).append(name)
    for name, _buffer in model.named_buffers(remove_duplicate=False):
        groups.setdefault(id(_buffer), []).append(name)

    for names in groups.values():
        if len(names) <= 1:
            continue
        sorted_names = sorted(names)
        for name in names:
            aliases[name] = [other for other in sorted_names if other != name]

    if (
        getattr(getattr(model, "config", None), "tie_word_embeddings", False)
        and hasattr(model, "get_input_embeddings")
        and hasattr(model, "get_output_embeddings")
    ):
        input_embeddings = model.get_input_embeddings()
        output_embeddings = model.get_output_embeddings()
        module_names = {
            id(module): name
            for name, module in model.named_modules(remove_duplicate=False)
        }
        if input_embeddings is not None and output_embeddings is not None:
            input_name = module_names.get(id(input_embeddings))
            output_name = module_names.get(id(output_embeddings))
            if input_name is not None and output_name is not None:
                input_key = f"{input_name}.weight" if input_name else "weight"
                output_key = f"{output_name}.weight" if output_name else "weight"
                _add_alias_pair(aliases, input_key, output_key)

    for name in aliases:
        aliases[name] = sorted(aliases[name])
    return aliases


def _merge_aliases(
    left: dict[str, list[str]], right: dict[str, list[str]]
) -> dict[str, list[str]]:
    merged = {}
    for alias_map in (left, right):
        for name, aliases in alias_map.items():
            for alias in aliases:
                _add_alias_pair(merged, name, alias)
    for name in merged:
        merged[name] = sorted(merged[name])
    return merged


def _aliases_for(names: Iterable[str], tied_aliases: dict[str, list[str]]) -> set[str]:
    aliases = set()
    for name in names:
        aliases.update(tied_aliases.get(name, []))
    return aliases


def _copy_state_tensor(name: str, dest: torch.Tensor, source: torch.Tensor):
    if tuple(dest.shape) != tuple(source.shape):
        raise RuntimeError(
            f"Shape mismatch for {name}: checkpoint {tuple(source.shape)} != model {tuple(dest.shape)}"
        )

    if isinstance(dest, DTensor):
        source = distribute_tensor(
            source, device_mesh=dest.device_mesh, placements=dest.placements
        )

    try:
        dest.copy_(source)
    except Exception as e:
        raise RuntimeError(
            f"Failed to copy tensor {name}: checkpoint dtype={source.dtype}, "
            f"model dtype={dest.dtype}, model device={dest.device}"
        ) from e


def _empty_device_cache(device: torch.device):
    if device.type == "cuda":
        torch.cuda.empty_cache()
    elif device.type == "mps" and hasattr(torch, "mps"):
        torch.mps.empty_cache()


def _device_context(device: torch.device):
    if device.type == "cuda":
        return torch.cuda.device(device.index)
    return nullcontext()


@lru_cache(maxsize=1)
def _mps_supports_bfloat16() -> bool:
    if not torch.backends.mps.is_available():
        return False
    try:
        device = torch.device("mps")
        lhs = torch.ones((4, 4), dtype=torch.bfloat16, device=device)
        rhs = torch.ones((4, 4), dtype=torch.bfloat16, device=device)
        out = lhs @ rhs
        torch.mps.synchronize()
        expected = torch.full((4, 4), 4.0, dtype=torch.float32)
        return out.dtype == torch.bfloat16 and torch.allclose(
            out.to("cpu", dtype=torch.float32), expected, rtol=1e-3, atol=1e-3
        )
    except Exception:
        return False


def _mps_safe_dtype(device: torch.device, dtype: torch.dtype) -> torch.dtype:
    if device.type != "mps" or dtype != torch.bfloat16:
        return dtype

    bf16_override = os.environ.get("PSYCHE_MPS_BF16", "").strip().lower()
    if bf16_override in {"1", "true", "yes", "force"}:
        print("PSYCHE_MPS_BF16=1 set; using bfloat16 on MPS without the safety probe")
        return dtype
    if bf16_override in {"0", "false", "no", "off"}:
        return torch.float16
    if _mps_supports_bfloat16():
        return dtype

    print(
        "MPS bfloat16 probe failed; using float16. Set PSYCHE_MPS_BF16=1 to force bfloat16."
    )
    return torch.float16


def _attention_implementation_for_device(
    device: torch.device, attn_implementation: str
) -> str:
    if device.type != "cuda" and attn_implementation == "flash_attention_2":
        return "sdpa"
    return attn_implementation


def _maybe_apply_liger_kernel(model: torch.nn.Module, config, no_tp: bool):
    try:
        from liger_kernel.transformers.monkey_patch import (
            _apply_liger_kernel_to_instance,
            MODEL_TYPE_TO_APPLY_LIGER_FN,
        )
    except ImportError:
        print("Skipping Liger kernels because liger_kernel is not installed")
        return

    if config.model_type not in MODEL_TYPE_TO_APPLY_LIGER_FN:
        return

    print(f"Applying liger kernels to model type `{config.model_type}`")
    _apply_liger_kernel_to_instance(
        model=model,
        fused_linear_cross_entropy=no_tp,  # liger fused ce can't deal with mixed tensor/dtensors which happens in non-pure-fsdp mode
    )


class HfTransformersAuto(CausalLM):
    def __init__(self, model, config, world_mesh: DeviceMesh, device: torch.device):
        self.model = model
        self.config = config
        self.world_mesh = world_mesh
        self.device = device

    @staticmethod
    def from_pretrained(
        source: Union[PretrainedSourceRepoFiles, PretrainedSourceStateDict],
        device: torch.device,
        attn_implementation: str,
        dp: int = 1,
        tp: int = 1,
        override_max_position_embeddings: Optional[int] = None,
        param_dtype: torch.dtype = torch.bfloat16,
        reduce_dtype: torch.dtype = torch.float32,
        fsdp_modules: Optional[Iterable[str]] = None,
    ):
        if isinstance(source, PretrainedSourceStateDict):
            state_dict = source.state_dict
            config_json = source.config_json
        else:
            state_dict = None
            config_json, tensor_files = _load_repo_source_index(source.files)

        if config_json is None:
            raise RuntimeError("No config.json present")
        config = auto_config_from_dict(json.loads(config_json))
        if override_max_position_embeddings:
            config.max_position_embeddings = override_max_position_embeddings

        param_dtype = _mps_safe_dtype(device, param_dtype)
        attn_implementation = _attention_implementation_for_device(
            device, attn_implementation
        )

        with torch.device("meta"):
            model: torch.nn.Module = AutoModelForCausalLM.from_config(
                config,
                attn_implementation=attn_implementation,
            )
        if hasattr(model.config, "use_cache"):
            model.config.use_cache = False
        if hasattr(model, "tie_weights"):
            model.tie_weights()
        tied_aliases = _tied_weight_aliases(model)
        if device.type == "cuda":
            torch.cuda.set_device(device)
        elif tp != 1 or dp != 1:
            raise RuntimeError(
                f"HfAuto only supports dp=1 and tp=1 on non-CUDA devices, got dp={dp}, tp={tp}"
            )

        world_mesh = None
        if tp != 1 or dp != 1:
            world_mesh = build_mesh("cuda", dp_shard=dp, tp=tp)

            tp_mesh = world_mesh["tp"] if tp > 1 else None
            dp_shard_mesh = world_mesh["dp_shard"] if dp > 1 else None

            if tp != 1:
                tp_mesh = world_mesh["tp"]

                if config.model_type != "llama" and config.model_type != "seed_oss":
                    raise ValueError(
                        f"Tensor parallelism not supported for model type `{config.model_type}` (yet)"
                    )
                if config.num_attention_heads % tp != 0:
                    raise ValueError(
                        f"TP degree {tp} must divide num_attention_heads {config.num_attention_heads}"
                    )
                if config.num_key_value_heads % tp != 0:
                    raise ValueError(
                        f"TP degree {tp} must divide num_key_value_heads {config.num_key_value_heads}"
                    )

                layer_plan = {
                    "self_attn.q_proj": ColwiseParallel(),
                    "self_attn.k_proj": ColwiseParallel(),
                    "self_attn.v_proj": ColwiseParallel(),
                    "self_attn.o_proj": RowwiseParallel(),
                    "mlp.gate_proj": ColwiseParallel(),
                    "mlp.up_proj": ColwiseParallel(),
                    "mlp.down_proj": RowwiseParallel(),
                }

                for layer in model.model.layers:
                    parallelize_module(layer, tp_mesh, parallelize_plan=layer_plan)

                parallelize_module(
                    model,
                    tp_mesh,
                    parallelize_plan={
                        "lm_head": ColwiseParallel(output_layouts=Replicate()),
                    },
                )

            if dp != 1:
                mp_policy = MixedPrecisionPolicy(
                    param_dtype=param_dtype, reduce_dtype=reduce_dtype
                )
                fsdp_config = {
                    "mesh": dp_shard_mesh,
                    "mp_policy": mp_policy,
                }

                if fsdp_modules is None:
                    if isinstance(model, PreTrainedModel):
                        fsdp_modules = model._no_split_modules
                    if hasattr(model, "model"):
                        if isinstance(model.model, PreTrainedModel):
                            fsdp_modules = model.model._no_split_modules
                if fsdp_modules is None:
                    raise RuntimeError("Could not determine models to apply FSDP to")

                for module in model.modules():
                    if module.__class__.__name__ in fsdp_modules:
                        fully_shard(module, **fsdp_config)
                model = fully_shard(model, **fsdp_config)
            else:
                # pure TP
                model = model.to(dtype=param_dtype)
        else:
            # if not sharding, apply param_dtype
            model = model.to(dtype=param_dtype)

        # move the (potentially sharded) meta model to the device
        model.to_empty(device=device)

        # HACK: apply RoPE parameters after meta device transition.
        # because transformers does this in __init__() (which is ignored on meta)
        # rather than post_init() or init_weights(), there (doesn't appear) to
        # be a general way to initialize static calculated buffers.
        # might be a problem for arbitrary models.
        # this is highly britle, someone plz fix

        def reinit_rope(module):
            if (
                hasattr(module, "inv_freq")
                and hasattr(module, "config")
                and hasattr(module, "attention_scaling")
                and hasattr(module, "rope_init_fn")
            ):
                inv_freq, attention_scaling = module.rope_init_fn(
                    module.config, device, **getattr(module, "rope_kwargs", {})
                )
                module.inv_freq.copy_(inv_freq)
                module.attention_scaling = attention_scaling

                # llama scaling needs this
                if hasattr(module, "original_inv_freq"):
                    module.original_inv_freq = module.inv_freq

        for module in model.modules():
            reinit_rope(module)
        reinit_rope(model)

        if model.supports_gradient_checkpointing:
            model.gradient_checkpointing_enable()

        if device.type == "cuda":
            no_tp = tp == 1
            _maybe_apply_liger_kernel(model, config, no_tp)

        if device.type == "cuda":
            # compile the loss, greatly reduces mem usage for large vocabularies
            model.loss_function = torch.compile(model.loss_function)

        # Stream safetensors shard-by-shard to avoid materializing the full
        # checkpoint in host RAM. DTensor paths still materialize each source
        # tensor before redistribution. Longer-term, prefer
        # torch.distributed.checkpoint when it supports the native target cleanly.
        with torch.no_grad():
            model_state = model.state_dict()
            tied_aliases = _merge_aliases(tied_aliases, _tied_weight_aliases(model))
            if state_dict is not None:
                remaining_names = set(state_dict.keys())
                loaded_names = set()
                for name, dest in model_state.items():
                    source_name = name
                    source_tensor: Optional[torch.Tensor] = state_dict.get(name)
                    if source_tensor is None:
                        for alias in tied_aliases.get(name, []):
                            if alias in state_dict:
                                source_name = alias
                                source_tensor = state_dict[alias]
                                break
                            if alias in loaded_names:
                                loaded_names.add(name)
                                break
                    if source_tensor is None:
                        if name not in loaded_names:
                            raise RuntimeError(f"Missing state_dict tensor {name}")
                    else:
                        _copy_state_tensor(name, dest, source_tensor)
                        del source_tensor
                        loaded_names.add(name)
                        remaining_names.discard(source_name)
                        remaining_names.discard(name)
                unexpected = remaining_names - _aliases_for(loaded_names, tied_aliases)
                if unexpected:
                    raise RuntimeError(
                        f"Unexpected checkpoint tensors: {_format_tensor_names(unexpected)}"
                    )
                _empty_device_cache(device)
            else:
                names_by_file: dict[str, dict[str, list[str]]] = {}
                used_checkpoint_names = set()

                for name in model_state.keys():
                    file = tensor_files.get(name)
                    source_name = name
                    if file is None:
                        for alias in tied_aliases.get(name, []):
                            file = tensor_files.get(alias)
                            if file is not None:
                                source_name = alias
                                break
                    if file is None:
                        raise RuntimeError(f"Missing state_dict tensor {name}")
                    used_checkpoint_names.add(source_name)
                    names_by_file.setdefault(file, {}).setdefault(
                        source_name, []
                    ).append(name)

                unexpected = (
                    set(tensor_files.keys())
                    - used_checkpoint_names
                    - _aliases_for(used_checkpoint_names, tied_aliases)
                )
                if unexpected:
                    raise RuntimeError(
                        f"Unexpected checkpoint tensors: {_format_tensor_names(unexpected)}"
                    )

                for file in sorted(names_by_file):
                    with safe_open(file, framework="pt", device="cpu") as f:
                        for source_name in sorted(names_by_file[file]):
                            source_tensor = f.get_tensor(source_name)
                            for name in names_by_file[file][source_name]:
                                _copy_state_tensor(
                                    name, model_state[name], source_tensor
                                )
                            del source_tensor
                    _empty_device_cache(device)

        if world_mesh is None and hasattr(model, "tie_weights"):
            model.tie_weights()

        return HfTransformersAuto(model, config, world_mesh, device)

    def forward(
        self,
        input_ids: torch.Tensor,
        labels: Optional[torch.Tensor],
        position_ids: Optional[torch.Tensor] = None,
        sequence_lengths: Optional[list[list[int]]] = None,
        num_logits_to_keep: Optional[int] = None,
        loss_scale: Optional[float] = None,
    ) -> Tuple[Optional[torch.Tensor], Optional[torch.Tensor]]:
        if self.world_mesh:
            if self.world_mesh.mesh_dim_names:
                if "dp_shard" in self.world_mesh.mesh_dim_names:
                    dp_shard = self.world_mesh[tuple(("dp_shard",))]
                    size = dp_shard.size()
                    rank = dp_shard.get_local_rank()

                    # do FSDP data sharding
                    shard_size = input_ids.shape[0] // size
                    start_row = rank * shard_size
                    input_ids = input_ids.narrow(0, start_row, shard_size)
                    if labels is not None:
                        labels = labels.narrow(0, start_row, shard_size)
                    if position_ids is not None:
                        position_ids = position_ids.narrow(0, start_row, shard_size)

        num_logits_to_keep = 0 if num_logits_to_keep is None else num_logits_to_keep

        # CUDA needs a device context for Liger/Triton kernels. Non-CUDA devices
        # such as Apple MPS do not have a torch.cuda context.
        with _device_context(input_ids.device), mps_compat_context(input_ids.device):
            try:
                ret = self.model(
                    input_ids.contiguous(),
                    labels=labels.contiguous() if labels is not None else None,
                    position_ids=(
                        position_ids.contiguous() if position_ids is not None else None
                    ),
                    logits_to_keep=num_logits_to_keep,  # name changed in 4.50
                    return_dict=True,
                    use_cache=False,
                )
            except Exception as e:
                import traceback

                print(f"[{self.device}]: {e}")
                traceback.print_exception(e)
                raise e
            if ret.loss and loss_scale:
                ret.loss /= loss_scale
            return (ret.logits, ret.loss)

    def named_parameters(self) -> dict[str, torch.Tensor]:
        params = dict(self.model.named_parameters())
        # undo activation checkpoint wrapping
        return {k.replace(_CHECKPOINT_PREFIX, ""): v for k, v in params.items()}

    def train(self):
        self.model.train()

    def get_config(self):
        return self.config.to_dict()

    def convert(
        self, state_dict: Optional[dict[str, torch.Tensor]]
    ) -> dict[str, torch.Tensor]:
        return state_dict if state_dict is not None else self.model.state_dict()
