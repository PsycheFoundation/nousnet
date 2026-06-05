# Native Silicon Compute

This page covers experimental native compute-provider builds for systems where the standard Docker CUDA flow cannot expose the local accelerator. The primary supported target for this path is Apple Silicon on macOS using Metal Performance Shaders (MPS). Windows ARM64 is treated as an experimental CPU-only development target.

The production compute-provider path is still Linux + NVIDIA CUDA + Docker. Native silicon mode is useful for development and for runs whose administrator has confirmed that a native client is acceptable.

## Support Matrix

| Platform | Status | Notes |
| --- | --- | --- |
| macOS Apple Silicon | Experimental | Single local rank. Uses `--device auto` or `--device mps`. BF16 is used when the local MPS stack passes a basic runtime BF16 probe; otherwise native paths fall back to float16. Some PyTorch operations may fall back to CPU through `PYTORCH_ENABLE_MPS_FALLBACK=1`, which can be much slower than failing fast. |
| Linux NVIDIA CUDA | Supported by standard Docker flow | Native mode can be useful for development, but Docker is the recommended compute-provider path. |
| Windows ARM64 | Experimental CPU-only | Uses `--device cpu`. Requires native Windows ARM64 Python with PyTorch installed. DirectML, NPUs, and GPU acceleration are not wired into this client. |
| Other accelerators | Not supported | ROCm, Vulkan, DirectML, NPUs, and Apple MPS on non-macOS platforms are not covered by these native helpers. |

## Model Compatibility

Native silicon mode can only join runs that the local client binary can execute:

- `HfLlama` and `HfDeepseek` use the native Rust/tch model path and are the safest targets for Apple Silicon development.
- `HfAuto` requires building the client with the `python` feature. On non-CUDA devices it is single-rank only.
- `Torchtitan` currently requires CUDA and is rejected on non-CUDA native devices.
- Ephemeral checkpoint runs cannot be joined by native mode.

Apple Silicon native mode is intended for small development runs. Large-vocabulary or long-context runs can still run out of unified memory or become very slow on MPS, especially when PyTorch falls back to CPU for unsupported operations.

For Apple Silicon, keep `DATA_PARALLELISM=1` and `TENSOR_PARALLELISM=1`, or pass:

```bash
-- --device mps --data-parallelism 1 --tensor-parallelism 1
```

For Windows ARM64, use CPU explicitly:

```powershell
-- --device cpu --data-parallelism 1 --tensor-parallelism 1
```

## Build Native Binaries

### macOS Apple Silicon

From the repository root:

```bash
scripts/build-native-silicon.sh
```

For `HfAuto` runs, build with Python support:

```bash
scripts/build-native-silicon.sh --python
```

The helper uses the Python selected by `PYTHON_SYS_EXECUTABLE`, or `python3` if that variable is unset. That Python must be able to import `torch` for every native build because the Rust client links against libtorch through PyTorch. The `--python` flag additionally enables Python-backed `HfAuto` model support.

On macOS, the helper also patches the built binaries so they can find the PyTorch dynamic libraries at runtime.

Apple Silicon builds keep BF16 enabled when MPS passes a basic numerical BF16 probe. To override detection:

```bash
PSYCHE_MPS_BF16=1  # force BF16 and skip the safety probe
PSYCHE_MPS_BF16=0  # use float16 instead
```

Only force BF16 if you know your local macOS, PyTorch, and MPS stack handles BF16 reliably.

### Windows ARM64 CPU

Install native Windows ARM64 Python and a PyTorch build that supports Windows ARM64. The helper expects that Python to import `torch` successfully:

```powershell
python -c "import platform, torch; print(platform.machine(), torch.__version__)"
```

Install the Rust Windows ARM64 target:

```powershell
rustup target add aarch64-pc-windows-msvc
```

You also need the Microsoft C++ build tools for the ARM64 MSVC target. Run this helper from native Windows ARM64, not from an x64 Windows host.

Then build from the repository root:

```powershell
scripts\build-native-windows-arm64.ps1
```

For `HfAuto` runs:

```powershell
scripts\build-native-windows-arm64.ps1 -Python
```

The helper writes binaries and a runtime environment script under `target\aarch64-pc-windows-msvc\debug` or `target\aarch64-pc-windows-msvc\release`. Source the environment script before running the binaries in a fresh PowerShell session so Windows can find the PyTorch DLLs:

```powershell
. .\target\aarch64-pc-windows-msvc\debug\native-windows-arm64-env.ps1
```

Windows ARM64 native mode is CPU-only and should be verified on the target Windows ARM64 machine before joining any real run. Use it for smoke tests and small development runs, not production training.

## Solana Wallet Requirements

You need a Solana keypair even if you are not claiming rewards. The client submits transactions to join and update the coordinator, and those transactions cost SOL.

Create a dedicated node wallet:

```bash
solana-keygen new --outfile ~/.config/solana/psyche-node.json
solana-keygen pubkey ~/.config/solana/psyche-node.json
```

Fund that wallet with enough SOL for coordinator transactions on the network you are using. For devnet:

```bash
solana airdrop 1 "$(solana-keygen pubkey ~/.config/solana/psyche-node.json)" --url devnet
```

Devnet airdrops can be rate-limited. If the command fails, retry later or use the Solana web faucet.

For mainnet, send native SOL on the Solana network to the wallet public key from a wallet or exchange account you control. Keep only the amount needed for operating the node and topping up transaction fees if the run lasts longer than expected.

Do not commit a private key. Prefer a keypair file with restricted permissions:

```bash
chmod 600 ~/.config/solana/psyche-node.json
```

Use the keypair path in your run env file:

```env
WALLET_PRIVATE_KEY_PATH=/Users/you/.config/solana/psyche-node.json
RPC=https://your-mainnet-rpc-provider
WS_RPC=wss://your-mainnet-rpc-provider
RUN_ID=your-run-id
```

Set the keypair path in the env file. `run-manager` loads that file and forwards the raw key material to the native client process; you do not need to set `RAW_WALLET_PRIVATE_KEY` yourself.

Public mainnet RPC endpoints are acceptable for smoke tests, but sustained training participation should use a reliable RPC provider with a matching websocket endpoint.

If the run is permissioned, send the wallet public key to the run administrator and wait for authorization. Native mode forwards the resolved `AUTHORIZER` to the client the same way Docker mode does.

## Run Native Mode

After building:

```bash
target/debug/run-manager \
  --env-file ~/.config/psyche/run.env \
  --native-client target/debug/psyche-solana-client
```

For Apple Silicon, the default `--device auto` selects MPS when available. You can make it explicit:

```bash
target/debug/run-manager \
  --env-file ~/.config/psyche/run.env \
  --native-client target/debug/psyche-solana-client \
  -- --device mps --data-parallelism 1 --tensor-parallelism 1
```

On Windows ARM64, source the generated runtime environment script and run CPU mode:

```powershell
. .\target\aarch64-pc-windows-msvc\debug\native-windows-arm64-env.ps1

target\aarch64-pc-windows-msvc\debug\run-manager.exe `
  --env-file "$env:USERPROFILE\.config\psyche\run.env" `
  --native-client target\aarch64-pc-windows-msvc\debug\psyche-solana-client.exe `
  -- --device cpu --data-parallelism 1 --tensor-parallelism 1
```

If the coordinator requires a client version that cannot be inferred from the local workspace version, pass it explicitly only after the run administrator confirms this native fork and commit are accepted for the run:

```bash
target/debug/run-manager \
  --env-file ~/.config/psyche/run.env \
  --native-client target/debug/psyche-solana-client \
  --native-client-version v0.2.0
```

Do not use `--native-client-version` only to silence the compatibility error. That value is part of the coordinator's client-version gate.

Native mode does not pull the coordinator-selected Docker image. If the run administrator expects only the official Docker client, do not use native mode for that run.
