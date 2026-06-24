Thsi binary is a manager for Psyche client containers. It should allow users to connect to a Psyche without having to worry about client versions, as this performs the necessary checks beforehand.

One can run the run manager like this:

```bash
cargo run --release -p run-manager -- --env-file .env.local
```

In case you already have a prebuilt binary:

```bash
./run-manager --env-file .env.local
```

Where:

- `--env-file` should point to a `.env` file where several relevant environment variables should be defined, for example:

  ```
  RPC=http://some-host:8899
  WS_RPC=ws://some-host:8900
  RUN_ID=the_run_id
  WALLET_PRIVATE_KEY_PATH=keys/keypair.json  # Optional
  ```

  - If `WALLET_PRIVATE_KEY_PATH` is defined it will use the specified keypair instead of the default `$HOME/.config/solana/id.json`

The run manager will also try to restart the client a few times in case it encounters an error. If you notice it somehow is stuck you may close the process manually via `ctrl+c` and run it again.

## Native client mode

By default, run-manager launches a Dockerized CUDA client. For platforms where Docker cannot expose the local accelerator, such as Apple Silicon, run-manager can instead launch a locally built `psyche-solana-client` binary:

```bash
./run-manager \
  --env-file .env.local \
  --native-client /path/to/psyche-solana-client
```

The native client inherits the environment loaded from `--env-file`, plus the resolved `RUN_ID`, `AUTHORIZER`, and wallet key. Pass client CLI options after `--`:

```bash
./run-manager \
  --env-file .env.local \
  --native-client /path/to/psyche-solana-client \
  -- --device mps --data-parallelism 1 --tensor-parallelism 1
```

If you want the native client to enforce the coordinator client-version check, pass the local binary's compatible version explicitly:

```bash
./run-manager \
  --env-file .env.local \
  --native-client /path/to/psyche-solana-client \
  --native-client-version v0.2.0
```
