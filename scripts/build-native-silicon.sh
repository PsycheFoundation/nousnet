#!/usr/bin/env bash
set -euo pipefail

profile=debug
features=()

has_feature() {
    local wanted="$1"
    local feature
    for feature in ${features[@]+"${features[@]}"}; do
        [[ "$feature" == "$wanted" ]] && return 0
    done
    return 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)
            profile=release
            shift
            ;;
        --python)
            features+=("python")
            shift
            ;;
        --parallelism)
            features+=("parallelism")
            shift
            ;;
        -h|--help)
            cat <<'EOF'
Usage: scripts/build-native-silicon.sh [--release] [--python] [--parallelism]

Build run-manager and a native psyche-solana-client for macOS Apple Silicon
development. Non-CUDA native training is experimental and single-rank only.
All native builds require a Python environment that can import torch because
the Rust client links against libtorch through PyTorch.

Options:
  --release      Build release binaries.
  --python       Build the client with the python feature for HfAuto runs.
  --parallelism  Build with NCCL/CUDA parallelism support. Not supported on macOS.
EOF
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" == "Darwin" ]] && has_feature "parallelism"; then
    echo "--parallelism uses NCCL/CUDA and is not supported by this Apple Silicon build helper" >&2
    exit 2
fi

python_bin="${PYTHON_SYS_EXECUTABLE:-}"
if [[ -z "$python_bin" ]]; then
    python_bin="$(command -v python3 || true)"
fi
if [[ -z "$python_bin" ]]; then
    echo "python3 not found; set PYTHON_SYS_EXECUTABLE" >&2
    exit 1
fi

set +e
torch_lib="$("$python_bin" - <<'PY'
import pathlib
import torch
print(pathlib.Path(torch.__file__).parent / "lib")
PY
)"
torch_status=$?
set -e
if [[ $torch_status -ne 0 ]]; then
    echo "Failed to import torch with $python_bin." >&2
    echo "Install PyTorch for that Python or set PYTHON_SYS_EXECUTABLE to the Python that has torch installed." >&2
    exit 1
fi

python_version="$("$python_bin" - <<'PY'
import sys
print(f"{sys.version_info.major}.{sys.version_info.minor}")
PY
)"

export PYTHON_SYS_EXECUTABLE="$python_bin"
export LIBTORCH_USE_PYTORCH="${LIBTORCH_USE_PYTORCH:-1}"
export LIBTORCH_BYPASS_VERSION_CHECK="${LIBTORCH_BYPASS_VERSION_CHECK:-1}"

rustflags_extra=()
rpath_dirs=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    rpath_dirs+=("$torch_lib")
    rustflags_extra+=(
        "-C"
        "link-arg=-Wl,-headerpad_max_install_names"
        "-C"
        "link-arg=-Wl,-rpath,$torch_lib"
    )
fi

python_ld_dirs=()
if has_feature "python"; then
    python_major="${python_version%%.*}"
    python_minor="${python_version#*.}"
    if [[ "$python_major" -gt 3 || ( "$python_major" -eq 3 && "$python_minor" -ge 14 ) ]]; then
        export PYO3_USE_ABI3_FORWARD_COMPATIBILITY="${PYO3_USE_ABI3_FORWARD_COMPATIBILITY:-1}"
    fi

    python_config=""
    python_config_candidates=(
        "${python_bin}-config"
        "$(dirname "$python_bin")/python${python_version}-config"
        "$(dirname "$python_bin")/python3-config"
        "python${python_version}-config"
        "python3-config"
    )
    for candidate in "${python_config_candidates[@]}"; do
        if command -v "$candidate" >/dev/null 2>&1; then
            python_config="$candidate"
            break
        fi
    done

    if [[ -n "$python_config" ]]; then
        python_ldflag_lines="$("$python_config" --ldflags --embed 2>/dev/null | tr ' ' '\n' || true)"
        while IFS= read -r token; do
            case "$token" in
                -L*) python_ld_dirs+=("${token#-L}") ;;
            esac
        done <<<"$python_ldflag_lines"
    fi

    python_sysconfig_dirs="$("$python_bin" - <<'PY'
import sysconfig
seen = set()
for key in ("LIBPL", "LIBDIR"):
    value = sysconfig.get_config_var(key)
    if value and value not in seen:
        print(value)
        seen.add(value)
PY
)"
    while IFS= read -r dir; do
        [[ -n "$dir" ]] && python_ld_dirs+=("$dir")
    done <<<"$python_sysconfig_dirs"

    seen_python_ld_dirs=":"
    for dir in "${python_ld_dirs[@]}"; do
        [[ -d "$dir" ]] || continue
        case "$seen_python_ld_dirs" in
            *":$dir:"*) continue ;;
        esac
        seen_python_ld_dirs="${seen_python_ld_dirs}${dir}:"
        rustflags_extra+=("-L" "native=$dir")
        if [[ "$(uname -s)" == "Darwin" ]]; then
            rpath_dirs+=("$dir")
            rustflags_extra+=("-C" "link-arg=-Wl,-rpath,$dir")
        fi
    done
fi

if [[ ${#rustflags_extra[@]} -gt 0 ]]; then
    export RUSTFLAGS="${RUSTFLAGS:-} ${rustflags_extra[*]}"
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    seen_rpath_dirs=":"
    unique_rpath_dirs=()
    for dir in "${rpath_dirs[@]}"; do
        [[ -d "$dir" ]] || continue
        case "$seen_rpath_dirs" in
            *":$dir:"*) continue ;;
        esac
        seen_rpath_dirs="${seen_rpath_dirs}${dir}:"
        unique_rpath_dirs+=("$dir")
    done
    rpath_dirs=("${unique_rpath_dirs[@]}")
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    existing_rustflags=" ${RUSTFLAGS:-} "
    if [[ "$existing_rustflags" != *"-headerpad_max_install_names"* ]]; then
        echo "warning: Darwin builds should include -headerpad_max_install_names in RUSTFLAGS for post-link rpath fixes" >&2
    fi
fi

build_mode=()
if [[ "$profile" == "release" ]]; then
    build_mode=(--release)
fi

client_feature_args=()
if [[ ${#features[@]} -gt 0 ]]; then
    IFS=,
    client_feature_args=(--features "${features[*]}")
    unset IFS
fi

cargo build -p run-manager ${build_mode[@]+"${build_mode[@]}"}
cargo build -p psyche-solana-client --no-default-features ${client_feature_args[@]+"${client_feature_args[@]}"} ${build_mode[@]+"${build_mode[@]}"}

if [[ "$(uname -s)" == "Darwin" ]]; then
    for binary in "target/$profile/psyche-solana-client" "target/$profile/run-manager"; do
        [[ -x "$binary" ]] || continue
        for rpath_dir in "${rpath_dirs[@]}"; do
            if ! otool -l "$binary" | grep -Fq "$rpath_dir"; then
                install_name_tool -add_rpath "$rpath_dir" "$binary" || {
                    echo "Failed to add rpath $rpath_dir to $binary." >&2
                    echo "Try a clean rebuild so the script can embed rpaths at link time." >&2
                    exit 1
                }
            fi
        done
    done
fi

cat <<EOF
Built native Psyche binaries:
  target/$profile/run-manager
  target/$profile/psyche-solana-client

Python: $python_bin
Python version: $python_version
Torch libraries: $torch_lib
EOF
