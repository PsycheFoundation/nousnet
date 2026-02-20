# Cluster Observer

The `observer` is a program with a terminal UI that can replay events from the `psyche-event-sourcing` crate. It's designed to let you replay events from nodes and see when & how things go wrong.

It reads `.postcard` event files written by running clients, turns them into state snapshots, and lets you scrub backward and forward through time to understand exactly what happened during a training run.

```
┌─────────────────────┬──────────────────────────────────────┐
│ COORDINATOR         │ NODE: abc...def                      │
│ State: RoundTrain   │ Run State: RoundTrain                │
│ Epoch: 2  Step: 7   │ Warmup: Complete                     │
│ Checkpoint: P2P     │ Losses: e1:3.21 → e2:2.94 → e2:2.67  │
│ Clients: 2/2        │ Health failures: 0                   │
├─────────────────────┴──────────────────────────────────────┤
│ ◄──────────────────●──────────────────────────────────────►│
│ 14:30:00       14:32:44                        14:35:00    │
│ [←/→] step  [Shift+←/→] ×50  [Space] play  [q] quit        │
│ [Tab] next node  [1/2/3] speed  [g/G] first/last           │
└────────────────────────────────────────────────────────────┘
```

## Running

```bash
nix run .#observer
```

or

```bash
cargo run --bin observer
```

## Testing on local runs

### Centralized local testnet (`just local-testnet`)

`just local-testnet` passes `--events-dir ./events/local-testnet` to every client it spawns, so events are waiting for you as soon as training starts.

```bash
# terminal 1 — start a 2-client run (opens tmux)
just local-testnet --num-clients 2 --config-path path/to/config

# terminal 2 — watch it live (events accumulate in target/)
nix run .#observer -- ./events/local-testnet
```

Events persist in `target/local-testnet-events/` after the testnet exits.

### Decentralized Docker testnet (`just run_test_infra`)

Events are written to `docker/test/data/events/`

```bash
just build_docker_images
just run_test_infra 2  # spin up 2 clients

# in another terminal:
nix run .#observer -- --events-dir docker/test/data/events/

just stop_test_infra # events persist after this
```

> **Note:** Running integration tests via `cargo test` cleans up the events directory automatically on teardown. Use `just run_test_infra` directly if you want to keep the data.

## Directory structure

Events are organized by node ID, one subdirectory per node:

```
<events-dir>/
  <node_id_1>/
    events-epoch-0-2024-01-01T12-00-00Z.postcard
    events-epoch-1-2024-01-01T12-05-00Z.postcard
  <node_id_2>/
    events-epoch-0-2024-01-01T12-00-01Z.postcard
    ...
```

The observer discovers all node subdirectories automatically.

## Keyboard controls

| Key                   | Action                           |
| --------------------- | -------------------------------- |
| `←` / `→`             | Step backward/forward one event  |
| `Shift+←` / `Shift+→` | Jump 50 events                   |
| `Space`               | Toggle playback                  |
| `1` / `2` / `3`       | Set playback speed (×1, ×5, ×20) |
| `g` / `G`             | Jump to first/last event         |
| `Tab`                 | Cycle to next node panel         |
| `q`                   | Quit                             |

## Custom events directory

Any Psyche client supports `--events-dir` (or the `EVENTS_DIR` env var):

```bash
psyche-solana-client train \
    --events-dir /tmp/my-run-events \
    --run-id my-run \
    # ... other args

# In another terminal:
nix run .#observer -- --events-dir /tmp/my-run-events
```
