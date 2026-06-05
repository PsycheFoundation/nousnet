# Native Silicon Compute

This page covers experimental native compute-provider builds for systems where the standard Docker CUDA flow cannot expose the local accelerator. The primary supported target for this path is Apple Silicon on macOS using Metal Performance Shaders (MPS).

The production compute-provider path is still Linux + NVIDIA CUDA + Docker. Native silicon mode is useful for development and for runs whose administrator has confirmed that a native client is acceptable.

## Support Matrix

| Platform | Status | Notes |
| --- | --- | --- |
| macOS Apple Silicon | Experimental | Single local rank. Uses `--device auto` or `--device mps`. BF16 is used when the local MPS stack passes a runtime BF16 probe. Some PyTorch operations may fall back to CPU through `PYTORCH_ENABLE_MPS_FALLBACK=1`. |
| Linux NVIDIA CUDA | Supported by standard Docker flow | Native mode can be useful for development, but Docker is the recommended compute-provider path. |
| Windows ARM | Not supported for production training | CPU-only builds may be possible with manual dependency work, but this repository does not provide a tested Windows ARM native accelerator path. |
| Other accelerators | Not supported | ROCm, Vulkan, DirectML, NPUs, and Apple MPS on non-macOS platforms are not covered by this native helper. |

## Model Compatibility

Native silicon mode can only join runs that the local client binary can execute:

- `HfLlama` and `HfDeepseek` use the native Rust/tch model path and are the safest targets for Apple Silicon development.
- `HfAuto` requires building the client with the `python` feature. On non-CUDA devices it is single-rank only.
- `Torchtitan` currently requires CUDA and is rejected on non-CUDA native devices.
- Ephemeral checkpoint runs cannot be joined by native mode.

For Apple Silicon, keep `DATA_PARALLELISM=1` and `TENSOR_PARALLELISM=1`, or pass:

```bash
-- --device mps --data-parallelism 1 --tensor-parallelism 1
```

## Build Native Binaries

From the repository root:

```bash
scripts/build-native-silicon.sh
```

For `HfAuto` runs, build with Python support:

```bash
scripts/build-native-silicon.sh --python
```

The helper uses the Python selected by `PYTHON_SYS_EXECUTABLE`, or `python3` if that variable is unset. That Python must be able to import `torch`.

On macOS, the helper also patches the built binaries so they can find the PyTorch dynamic libraries at runtime.

Apple Silicon builds keep BF16 enabled when MPS supports it. To override detection:

```bash
PSYCHE_MPS_BF16=1  # force BF16
PSYCHE_MPS_BF16=0  # use float16 instead
```

## Solana Wallet Requirements

You need a Solana keypair even if you are not claiming rewards. The client submits transactions to join and update the coordinator, and those transactions cost SOL.

Create a dedicated node wallet:

```bash
solana-keygen new --outfile ~/.config/solana/psyche-node.json
solana-keygen pubkey ~/.config/solana/psyche-node.json
```

Fund that wallet with enough SOL for coordinator transactions on the network you are using. For devnet:

```bash
solana airdrop 1 ~/.config/solana/psyche-node.json --url devnet
```

For mainnet, send SOL to the wallet public key from a wallet or exchange account you control. Keep only the amount needed for operating the node.

Do not commit a private key. Prefer a keypair file with restricted permissions:

```bash
chmod 600 ~/.config/solana/psyche-node.json
```

Use the keypair path in your run env file:

```env
WALLET_PRIVATE_KEY_PATH=/Users/you/.config/solana/psyche-node.json
RPC=https://api.mainnet-beta.solana.com
WS_RPC=wss://api.mainnet-beta.solana.com
RUN_ID=your-run-id
```

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

If the coordinator requires a client version that cannot be inferred from the local workspace version, pass it explicitly:

```bash
target/debug/run-manager \
  --env-file ~/.config/psyche/run.env \
  --native-client target/debug/psyche-solana-client \
  --native-client-version v0.2.0
```

Native mode does not pull the coordinator-selected Docker image. If the run administrator expects only the official Docker client, do not use native mode for that run.
