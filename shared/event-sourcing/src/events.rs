use std::collections::HashSet;

use chrono::{DateTime, Utc};
use first_class_variants::first_class_variants;
use iroh::EndpointId as IrohEndpointId;
use iroh_blobs::Hash as IrohHash;
use psyche_coordinator::model::Checkpoint;
use psyche_coordinator::{ClientState, RunState};
use psyche_core::BatchId;
use serde::{Deserialize, Serialize};

use crate::bytes_visitor::BytesVisitor;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum SubscriptionStatus {
    Up,
    Down,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ErrorKind {
    InvalidRunState,
    InvalidWitness,
    Timeout,
    Unknown,
}

// Wrapper types for iroh types that don't implement serde
// We serialize them as byte arrays for compactness

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct EndpointId(pub IrohEndpointId);

impl Serialize for EndpointId {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        // EndpointId derefs to [u8; 32]
        serializer.serialize_bytes(&*self.0)
    }
}

impl<'de> Deserialize<'de> for EndpointId {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let bytes = deserializer.deserialize_bytes(BytesVisitor::<32>)?;
        IrohEndpointId::from_bytes(&bytes)
            .map(EndpointId)
            .map_err(serde::de::Error::custom)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Hash(pub IrohHash);

impl Serialize for Hash {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        serializer.serialize_bytes(self.0.as_bytes())
    }
}

impl<'de> Deserialize<'de> for Hash {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let bytes = deserializer.deserialize_bytes(BytesVisitor::<32>)?;
        Ok(Hash(IrohHash::from_bytes(bytes)))
    }
}

/// Tags to link related events
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub enum Tag {
    BlobUpload(u64),
    BlobDownload(u64),
    BatchId(BatchId),
    Blob(Hash),
    CheckpointDownload(u64),
    EventSubmission(u64),
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Event {
    pub timestamp: DateTime<Utc>,
    pub tags: Vec<Tag>,
    pub data: EventData,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum EventData {
    RunStarted(RunStarted),
    EpochStarted(EpochStarted),
    Coordinator(Coordinator),
    Client(Client),
    P2P(P2P),
    Train(Train),
    Warmup(Warmup),
    Cooldown(Cooldown),
    ResourceSnapshot(ResourceSnapshot),
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RunStarted {
    pub run_id: String,
    pub node_id: String,
    pub config: String,
    pub psyche_version: String,
}

impl From<RunStarted> for EventData {
    fn from(value: RunStarted) -> Self {
        EventData::RunStarted(value)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EpochStarted {
    pub epoch_number: u64,
}

impl From<EpochStarted> for EventData {
    fn from(value: EpochStarted) -> Self {
        EventData::EpochStarted(value)
    }
}

#[first_class_variants(
    module = "coordinator",
    impl_into_parent = "EventData",
    derive(Debug, Clone, Serialize, Deserialize)
)]
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Coordinator {
    CoordinatorStateChanged {
        new_state_hash: String,
    },
    SolanaSubscriptionChanged {
        url: String,
        status: SubscriptionStatus,
    },
    // TODO: we submitted an RPC call
    // TODO: rpc call success/fail
}

#[first_class_variants(
    module = "client",
    impl_into_parent = "EventData",
    derive(Debug, Clone, Serialize, Deserialize)
)]
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Client {
    StateChanged {
        old_state: RunState,
        new_state: RunState,
        epoch: u64,
        step: u64,
    },
    HealthCheckFailed {
        index: u64,
        current_step: u64,
    },
    ErrorOccurred {
        kind: ErrorKind,
        message: String,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum ConnectionPath {
    Direct,
    Relayed,
    Disconnected,
}

#[first_class_variants(
    module = "p2p",
    impl_into_parent = "EventData",
    derive(Debug, Clone, Serialize, Deserialize)
)]
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum P2P {
    ConnectionChanged {
        endpoint_id: EndpointId,
        connection_path: ConnectionPath,
    },
    GossipNeighborsChanged {
        removed_neighbors: HashSet<EndpointId>,
        new_neighbors: HashSet<EndpointId>,
    },
    ConnectionLatencyChanged {
        endpoint_id: EndpointId,
        latency_ms: u64,
    },
    BlobAddedToStore {
        hash: Hash,
    },
    BlobUploadStarted {
        to_endpoint_id: EndpointId,
        size_bytes: u64,
        retry_number: u32,
    },
    BlobUploadProgress {
        bytes_transferred: u64,
    },
    BlobUploadCompleted(Result<(), String>),
    BlobDownloadStarted {
        from_endpoint_id: EndpointId,
        size_bytes: u64,
        retry_number: u32,
    },
    BlobDownloadProgress {
        bytes_transferred: u64,
    },
    BlobDownloadCompleted {
        success: bool,
        error_string: Option<String>,
    },
    GossipMessageSent {
        message_type: String,
        message_size_bytes: u64,
        rebroadcast_count: u32,
    },
    GossipMessageReceived {
        message_type: String,
        message_size_bytes: u64,
    },
}

#[first_class_variants(
    module = "train",
    impl_into_parent = "EventData",
    derive(Debug, Clone, Serialize, Deserialize)
)]
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Train {
    BatchesAssigned {
        num_batches: u32,
    },
    BatchDataDownloadStart {
        size_bytes: u64,
    },
    BatchDataDownloadProgress {
        bytes_downloaded: u64,
    },
    BatchDataDownloadComplete {
        success: bool,
        error_string: Option<String>,
    },

    TrainingStarted,
    TrainingFinished {
        epoch: u64,
        step: u64,
        loss: Option<f64>,
    },
    UntrainedBatchWarning {
        batch_id: BatchId,
        expected_trainer: Option<String>,
    },
    WitnessElected {
        step: u64,
        round: u64,
        epoch: u64,
        index: u64,
        committee_position: u64,
    },

    DistroResultDeserializeStarted,
    DistroResultDeserializeComplete(Result<(), String>),
    ApplyDistroResultsStart {
        batch_ids: HashSet<BatchId>,
    },
    ApplyDistroResultsComplete(Result<(), String>),
}

#[first_class_variants(
    module = "warmup",
    impl_into_parent = "EventData",
    derive(Debug, Clone, Serialize, Deserialize)
)]
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Warmup {
    P2PParamInfoRequest { from: IrohEndpointId },
    P2PParamInfoResponse,

    CheckpointDownloadStarted { size_bytes: u64 },
    CheckpointDownloadProgress { bytes_downloaded: u64 },
    CheckpointDownloadComplete(Result<(), String>),
    ModelLoadStarted,
    ModelLoadComplete,
}

#[first_class_variants(
    module = "cooldown",
    impl_into_parent = "EventData",
    derive(Debug, Clone, Serialize, Deserialize)
)]
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Cooldown {
    ModelSerializationStarted,
    ModelSerializationFinished {
        success: bool,
        error_string: Option<String>,
    },

    CheckpointWriteStarted,
    CheckpointWriteFinished {
        success: bool,
        error_string: Option<String>,
    },

    CheckpointUploadStarted,
    CheckpointUploadProgress {
        bytes_uploaded: u64,
    },
    CheckpointUploadFinished {
        success: bool,
        error_string: Option<String>,
    },
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ResourceSnapshot {
    pub gpu_mem_used_bytes: Option<u64>,
    pub gpu_utilization_percent: Option<f32>,
    pub cpu_mem_used_bytes: u64,
    pub cpu_utilization_percent: f32,
    pub network_bytes_sent_total: u64,
    pub network_bytes_recv_total: u64,
    pub disk_space_available_bytes: u64,
}

impl From<ResourceSnapshot> for EventData {
    fn from(value: ResourceSnapshot) -> Self {
        EventData::ResourceSnapshot(value)
    }
}

// ── Coordinator disk records ───────────────────────────────────────────────────
//
// Written by the coordinator server to `{events_dir}/coordinator/events.postcard`
// in COBS-framed postcard format — the same encoding used for node event files.
// Read by the observer via `ClusterTimeline::from_events_dir`.

/// A coordinator state snapshot written to disk on every state change.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CoordinatorRecord {
    pub timestamp: DateTime<Utc>,
    pub run_state: RunState,
    pub epoch: u16,
    pub step: u32,
    pub checkpoint: Checkpoint,
    pub min_clients: u16,
    /// Active clients at the time of this snapshot.
    pub clients: Vec<CoordinatorClientRecord>,
}

/// A single active client entry within a [`CoordinatorRecord`].
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CoordinatorClientRecord {
    /// Display string of the client's node identity.
    /// Matches the directory name used for that node's event files.
    pub id: String,
    pub state: ClientState,
}
