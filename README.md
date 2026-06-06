# psyche - Apple Silicon native fork

<p align="center" width="100%">
    <img src="./psyche-book/src/psyche.jpg">
</p>

Psyche is a set of systems that enable distributed training of transformer-based AI models over the internet. It seeks to enable collaboration between untrusted parties to train state-of-the-art ML models.

This fork adds an experimental native compute-provider path for Apple Silicon so macOS machines can contribute without the standard Linux/NVIDIA CUDA Docker path. It also includes a Windows ARM64 helper for CPU-only smoke/development runs.

The upstream project and protocol are still Psyche. For the general project documentation, visit [the Psyche docs](https://docs.psyche.network). For this fork's native hardware path, start with [`psyche-book/src/enduser/native-silicon.md`](./psyche-book/src/enduser/native-silicon.md).

## Native Hardware Status

| Platform            | Status in this fork                       | Notes                                                                                                                                                                                  |
| ------------------- | ----------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| macOS Apple Silicon | Experimental native compute-provider path | Uses native `run-manager`/client binaries with PyTorch MPS for local acceleration. Intended for small development runs and run administrators who explicitly accept this fork/version. |
| Windows ARM64       | Experimental CPU-only path                | Builds native Windows ARM64 binaries for `--device cpu`. DirectML, NPUs, and Windows GPU acceleration are not wired in.                                                                |
| Linux NVIDIA CUDA   | Upstream supported path                   | The production path remains Docker + CUDA. This fork does not replace the standard NVIDIA provider flow.                                                                               |
| Other accelerators  | Not supported                             | ROCm, Vulkan, DirectML, NPUs, and non-macOS MPS are not implemented here.                                                                                                              |

## What This Fork Changes

- Adds `scripts/build-native-silicon.sh` to build `run-manager` and `psyche-solana-client` for macOS Apple Silicon.
- Adds `scripts/build-native-windows-arm64.ps1` to build Windows ARM64 CPU-only binaries.
- Adds native run-manager support for launching a local client binary instead of requiring a CUDA Docker container.
- Adds Apple MPS device handling, including BF16 probing with `PSYCHE_MPS_BF16=1` / `PSYCHE_MPS_BF16=0` overrides.
- Hardens Hugging Face/PyTorch model setup so non-CUDA devices do not assume CUDA-only features such as Liger, flash attention 2, or CUDA `torch.compile`.
- Gates `tikv-jemallocator` off on Windows so the Solana client can build on Windows/MSVC targets.
- Documents Solana wallet/funding requirements for joining coordinator runs.

## Apple Silicon Quick Start

From the repository root on macOS Apple Silicon:

```bash
scripts/build-native-silicon.sh
```

For `HfAuto`/Python-backed runs:

```bash
scripts/build-native-silicon.sh --python
```

Run with the native client path:

```bash
target/debug/run-manager \
  --env-file "$HOME/.config/psyche/run.env" \
  --native-client target/debug/psyche-solana-client \
  -- --device mps --data-parallelism 1 --tensor-parallelism 1
```

Apple Silicon runs should use a single local rank:

```bash
-- --device mps --data-parallelism 1 --tensor-parallelism 1
```

If your local PyTorch/MPS stack has BF16 issues, force float16:

```bash
PSYCHE_MPS_BF16=0
```

Only force BF16 when you know your stack handles it:

```bash
PSYCHE_MPS_BF16=1
```

## Windows ARM64 CPU Quick Start

This path is for native Windows ARM64 CPU smoke/development runs, not accelerator training.

```powershell
rustup target add aarch64-pc-windows-msvc
scripts\build-native-windows-arm64.ps1
```

For `HfAuto`/Python-backed runs:

```powershell
scripts\build-native-windows-arm64.ps1 -Python
```

Then source the generated runtime environment and run CPU mode:

```powershell
. .\target\aarch64-pc-windows-msvc\debug\native-windows-arm64-env.ps1

target\aarch64-pc-windows-msvc\debug\run-manager.exe `
  --env-file "$env:USERPROFILE\.config\psyche\run.env" `
  --native-client target\aarch64-pc-windows-msvc\debug\psyche-solana-client.exe `
  -- --device cpu --data-parallelism 1 --tensor-parallelism 1
```

## Solana Requirement

Psyche's decentralized coordinator runs on Solana, so the client needs a Solana keypair with enough SOL to pay transaction fees. Use a dedicated keypair for this client and confirm the target cluster with the run administrator. Devnet SOL is only useful for devnet runs; mainnet runs need real mainnet SOL.

See the wallet section in [`psyche-book/src/enduser/native-silicon.md`](./psyche-book/src/enduser/native-silicon.md#solana-wallet-requirements).

## Important Caveats

- This fork does not make Apple Silicon equivalent to a CUDA training provider. MPS support is narrower and may fall back to CPU for unsupported PyTorch operations.
- Large models, long contexts, and large vocabularies can exceed unified memory or become too slow to be useful.
- Native mode should be accepted by the run administrator before joining a real coordinated run. Use `--client-version-override` only when the administrator explicitly confirms the accepted version.
- Windows ARM64 support has been statically reviewed and mechanically checked where possible, but it still needs target-host validation on a real Windows ARM64 machine before anyone should call it proven.

<p align="center" width="100%">
    <a href="https://www.youtube.com/watch?v=XMWI3nDk48c">
        <img src="./psyche-book/src/psyche_youtube.png">
    </a>
</p>
