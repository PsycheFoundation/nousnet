# Migration Plan: IntegrationTestLogMarker → Event Sourcing

## Overview

Replace JSON log parsing (IntegrationTestLogMarker) with event sourcing for integration tests. This gives us:

- Type-safe events instead of string parsing
- Efficient binary format (postcard) instead of JSON
- Crash-safe COBS framing
- Direct file access instead of streaming Docker logs via Bollard API

## Current Architecture

Integration tests use `DockerWatcher` to:

1. Stream Docker container logs via Bollard API
2. Parse JSON logs looking for `integration_test_log_marker` field
3. Extract fields into `Response` enum
4. Send to test via channel

Example current log statement:

```rust
info!(
    integration_test_log_marker = %IntegrationTestLogMarker::Loss,
    client_id = %self.identity,
    epoch = state.progress.epoch,
    step = state.progress.step,
    loss = loss.unwrap_or(f32::NAN),
    "client_loss",
);
```

## Proposed Architecture

1. **Production code emits events** using `event!()` macro
2. **FileBackend** writes to `/data/events/` in containers
3. **Docker compose** mounts `/data/events` as volume
4. **EventReader utility** tails event files from tests
5. **Tests** query events instead of parsing logs

## Event Mapping

### Existing Events (Already Defined)

| Marker      | Maps To                     | Notes                       |
| ----------- | --------------------------- | --------------------------- |
| LoadedModel | `Warmup::ModelLoadComplete` | checkpoint field matches    |
| -           | `Train::TrainingStarted`    | already exists, just use it |
| -           | `Train::TrainingFinished`   | already exists, just use it |

### New Events Needed

**1. StateChange → New `Client::StateChanged`**

```rust
#[first_class_variants(module = "client", derive(Debug, Clone, Serialize, Deserialize))]
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Client {
    StateChanged {
        old_state: RunState,
        new_state: RunState,
        epoch: u64,
        step: u64,
    },
    // ...
}
```

**2. Loss → `Train::TrainingFinished`**

```rust
// Add to TrainingFinished variant:
TrainingFinished {
    epoch: u64,
    step: u64,
    loss: Option<f64>,
},
```

**3. HealthCheck → New `Client::HealthCheckFailed`**

```rust
HealthCheckFailed {
    index: u64,
    current_step: u64,
},
```

**4. UntrainedBatches → New `Train::UntrainedBatchWarning`**

```rust
UntrainedBatchWarning {
    batch_id: BatchId,
    expected_trainer: Option<String>,
},
```

**5. WitnessElected → New `Train::WitnessElected`**

```rust
WitnessElected {
    client_id: String,
    step: u64,
    round: u64,
    epoch: u64,
    index: u64,
    committee_position: u64,
},
```

**6. SolanaSubscription → New `Coordinator::SolanaSubscription`**

```rust
// Add to CoordinatorEvent enum:
SolanaSubscriptionChanged {
    url: String,
    status: SubscriptionStatus,  // enum { Up, Down }
},
```

**7. Error → Generic error event**

```rust
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Client {
    // ...
    ErrorOccurred {
        kind: ErrorKind,  // InvalidRunState, InvalidWitness, Timeout, Unknown
        message: String,
    },
}
```

## Implementation Steps

### Phase 1: Extend Event Types (1-2 hours)

1. Add new event variants to `shared/event-sourcing/src/events.rs`
2. Add new `Client` enum with variants above
3. Add `LossComputed`, `UntrainedBatchWarning`, `WitnessElected` to `Train` enum
4. Add `SolanaSubscriptionChanged` to `CoordinatorEvent` enum
5. Update tests in event-sourcing crate

### Phase 2: Replace Log Statements (2-3 hours)

Replace each `info!(integration_test_log_marker = ...)` with `event!(...)`:

**Files to update:**

- `shared/client/src/client.rs` (StateChange)
- `shared/client/src/state/train.rs` (WitnessElected, UntrainedBatches, HealthCheck)
- `shared/client/src/state/steps.rs` (Loss)
- `shared/client/src/state/init.rs` (LoadedModel)
- `architectures/decentralized/solana-common/src/backend.rs` (SolanaSubscription)

**Example migration:**

```rust
// Before:
info!(
    integration_test_log_marker = %IntegrationTestLogMarker::Loss,
    client_id = %self.identity,
    epoch = state.progress.epoch,
    step = state.progress.step,
    loss = loss.unwrap_or(f32::NAN),
    "client_loss",
);

// After:
event!(train::LossComputed {
    client_id: self.identity.to_string(),
    epoch: state.progress.epoch,
    step: state.progress.step,
    loss: Some(loss),
});
```

### Phase 3: Docker Integration (1 hour)

1. **Update Docker entrypoint** to init FileBackend:

   ```rust
   EventStore::init(vec![
       Box::new(FileBackend::new(
           Path::new("/data/events"),
           0,  // initial epoch
           RunStarted {
               run_id: env::var("RUN_ID").unwrap(),
               node_id: env::var("NODE_ID").unwrap(),
               config: env::var("CONFIG").unwrap(),
               psyche_version: env!("CARGO_PKG_VERSION").into(),
           }
       )?),
   ]);
   ```

2. **Update docker-compose.yml**:
   ```yaml
   services:
     client-1:
       volumes:
         - ./data/client-1-events:/data/events
     client-2:
       volumes:
         - ./data/client-2-events:/data/events
   ```

### Phase 4: EventReader Utility (2-3 hours)

Create `architectures/decentralized/testing/src/event_reader.rs`:

```rust
pub struct EventReader {
    base_paths: HashMap<String, PathBuf>,  // container_id -> /data/client-N-events
    rx: mpsc::Receiver<(String, Event)>,    // (container_id, event)
}

impl EventReader {
    pub fn new() -> Self;

    /// Watch a container's event directory
    pub fn monitor_container(&self, container_id: &str, event_dir: PathBuf) -> JoinHandle<()>;

    /// Receive next event from any monitored container
    pub async fn recv(&mut self) -> Option<(String, Event)>;

    /// Filter by event type
    pub fn filter<F>(&mut self, f: F) -> FilteredReader<F>
    where F: Fn(&EventData) -> bool;
}
```

**Implementation details:**

- Use `notify` crate to watch event directories for file changes
- When new file appears, read incrementally using `EventStore::import_streamed_file()` logic
- Send events through channel to test
- Similar API to DockerWatcher but returns typed Events instead of Response enum

### Phase 5: Port Integration Tests (3-4 hours)

For each test in `integration_tests.rs` and `chaos_tests.rs`:

**Before:**

```rust
let _monitor_client_1 = watcher
    .monitor_container(
        &format!("{CLIENT_CONTAINER_PREFIX}-1"),
        vec![
            IntegrationTestLogMarker::StateChange,
            IntegrationTestLogMarker::Loss,
        ],
    )
    .unwrap();

while let Some(response) = watcher.log_rx.recv().await {
    match response {
        Response::StateChange(ts, client_id, old, new, epoch, step) => {
            // assertions
        }
        Response::Loss(client_id, epoch, step, loss) => {
            // assertions
        }
        _ => {}
    }
}
```

**After:**

```rust
let event_reader = EventReader::new();
event_reader.monitor_container(
    "client-1",
    PathBuf::from("./data/client-1-events")
);

while let Some((container_id, event)) = event_reader.recv().await {
    match event.data {
        EventData::Client(Client::StateChanged { old_state, new_state, epoch, step, .. }) => {
            // assertions
        }
        EventData::Train(Train::LossComputed { epoch, step, loss, .. }) => {
            // assertions
        }
        _ => {}
    }
}
```

**Benefits:**

- Type-safe pattern matching instead of Response enum
- Can filter/query by tags (e.g., `Tag::BatchId`)
- Access to full event history (can rewind/replay)
- No Docker API dependency in tests

### Phase 6: Cleanup (1 hour)

1. Remove `IntegrationTestLogMarker` from `shared/core/src/testing.rs`
2. Remove `DockerWatcher` from `architectures/decentralized/testing/src/docker_watcher.rs`
3. Remove `Response` enum
4. Update test dependencies (remove bollard if only used for log streaming)
5. Keep JSON logging for human debugging, but remove `integration_test_log_marker` fields

## Testing Strategy

1. **Dual-mode initially**: Keep both systems running in parallel
   - Emit both logs and events
   - Run subset of tests with EventReader
   - Verify events match log parsing results

2. **Gradual migration**: Port tests one by one, not all at once

3. **Validation**: Event file size, read performance, test flakiness

## Open Questions

1. **Event volume**: How large do event files get during integration tests?
   - simply measure!

2. **File watching latency**: notify crate fast enough for tests?
   - Alternative: Poll with inotify, or just read full file periodically

3. **Event timestamps**: Do tests need sub-millisecond precision?
   - Current: Tests use timestamps from logs
   - Event sourcing: Every event has DateTime&lt;Utc&gt; timestamp

4. **Multiple containers writing same event**: we can bind mount each event sourcing dir to a different host dir and read them there.

## Estimated Timeline

- Phase 1: 2 hours (extend event types)
- Phase 2: 3 hours (replace log statements)
- Phase 3: 1 hour (Docker integration)
- Phase 4: 3 hours (EventReader utility)
- Phase 5: 4 hours (port tests)
- Phase 6: 1 hour (cleanup)

**Total: ~14 hours** (could be done over 2-3 days)

## Rollback Plan

If migration fails:

1. Keep `integration_test_log_marker` logs in place during migration
2. EventReader is additive, doesn't break existing tests
3. Can revert any test to use DockerWatcher
4. Event sourcing crate is independent, doesn't affect production if not initialized
