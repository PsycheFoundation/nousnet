use chrono::{DateTime, Utc};
use indexmap::IndexMap;
use iroh::EndpointId;
use psyche_coordinator::{RunState, model::Checkpoint};
use psyche_core::BatchId;
use psyche_metrics::SelectedPath;
use std::collections::{BTreeMap, HashSet};

use crate::events::{Client, Cooldown, Event, EventData, P2P, ResourceSnapshot, Train, Warmup};

// ── Tag helpers ───────────────────────────────────────────────────────────────

fn operation_id(event: &Event) -> Option<u64> {
    event.tags.operation_id
}

fn batch_id_tags(event: &Event) -> impl Iterator<Item = &BatchId> {
    event.tags.batch_ids.iter().flatten()
}

// ── Coordinator ───────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct CoordinatorStateSnapshot {
    pub timestamp: DateTime<Utc>,
    pub run_state: RunState,
    pub epoch: u64,
    pub step: u64,
    pub checkpoint: Checkpoint,
    pub client_ids: Vec<String>,
    pub min_clients: usize,
    /// Batch → node_id assignments for the current step.
    /// Populated by the coordinator source using `assign_data_for_state`.
    pub batch_assignments: BTreeMap<BatchId, String>,
}

// ── Warmup ────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub enum WarmupPhase {
    #[default]
    Idle,
    NegotiatingP2P,
    Downloading,
    LoadingModel,
    Complete,
}

impl std::fmt::Display for WarmupPhase {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            WarmupPhase::Idle => write!(f, "Idle"),
            WarmupPhase::NegotiatingP2P => write!(f, "Negotiating P2P"),
            WarmupPhase::Downloading => write!(f, "Downloading"),
            WarmupPhase::LoadingModel => write!(f, "Loading Model"),
            WarmupPhase::Complete => write!(f, "Complete"),
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct WarmupSnapshot {
    pub phase: WarmupPhase,
    pub download_total_bytes: Option<u64>,
    pub download_bytes: u64,
    pub model_loaded: bool,
}

// ── P2P ───────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct PeerInfo {
    /// Whether we're talking to this peer directly or via a relay.
    pub connection_path: Option<SelectedPath>,
    pub latency_ms: Option<u64>,
}

#[derive(Debug, Clone)]
pub struct BlobTransfer {
    /// Endpoint we're uploading to / downloading from.
    pub peer_endpoint_id: EndpointId,
    pub size_bytes: u64,
    pub bytes_transferred: u64,
    /// None = in progress, Some(Ok(())) = success, Some(Err(s)) = failed.
    pub result: Option<Result<(), String>>,
}

#[derive(Debug, Clone, Default)]
pub struct P2PSnapshot {
    /// Currently known peers: iroh endpoint id → connection state.
    pub peers: IndexMap<EndpointId, PeerInfo>,
    /// Current gossip neighbourhood.
    pub gossip_neighbors: HashSet<EndpointId>,
    /// Blob uploads keyed by BlobUpload tag id.
    pub uploads: IndexMap<u64, BlobTransfer>,
    /// Blob downloads keyed by BlobDownload tag id.
    pub downloads: IndexMap<u64, BlobTransfer>,
    /// Total number of blobs ever added to local iroh store.
    pub blobs_in_store: usize,
    /// Total gossip messages sent this session.
    pub gossip_sent: u32,
    /// Total gossip messages received this session.
    pub gossip_recv: u32,
}

// ── Train ─────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct BatchDownload {
    pub result: Option<Result<(), ()>>,
}

#[derive(Debug, Clone)]
pub struct WitnessInfo {
    pub step: u64,
    pub round: u64,
    pub epoch: u64,
    pub index: u64,
    pub committee_position: u64,
}
#[derive(Debug, Clone, Default)]
pub struct TrainSnapshot {
    /// Number of batches assigned to this node for the current step.
    pub batches_assigned: u64,
    /// Per-batch download status for batches we're responsible for.
    /// Keyed by BatchId from Tag::BatchId on the event.
    pub batch_downloads: IndexMap<BatchId, BatchDownload>,
    /// True between TrainingStarted and TrainingFinished.
    pub training_in_progress: bool,
    /// Our witness election info for this step, if we were elected.
    pub witness: Option<WitnessInfo>,
    /// Batch IDs from the most recent ApplyDistroResultsStart (consensus).
    pub last_distro_batch_ids: HashSet<BatchId>,
    /// Whether the most recent DistroResult apply succeeded.
    pub last_distro_ok: Option<bool>,
    /// Batches we were warned about (batch_id, expected_trainer).
    pub untrained_warnings: Vec<(BatchId, Option<String>)>,
}

// ── Cooldown ──────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Default)]
pub struct CooldownSnapshot {
    /// True once we see ModelSerializationStarted — we are the checkpointer this round.
    pub is_checkpointer: bool,
    pub serialization_ok: Option<bool>,
    pub serialization_error: Option<String>,
    pub checkpoint_write_ok: Option<bool>,
    pub upload_bytes: u64,
    pub upload_ok: Option<bool>,
    pub upload_error: Option<String>,
}

// ── Cluster-level batch view ──────────────────────────────────────────────────

#[derive(Debug, Clone, Default)]
pub struct ClusterBatchView {
    /// Node assigned this batch, from coordinator state.
    pub assigned_to: Option<String>,
    /// Download progress per node (node_id → status).
    pub downloads: IndexMap<String, BatchDownload>,
    /// Nodes that have completed training for this batch (inferred from TrainingFinished).
    pub trained_by: HashSet<String>,
}

// ── NodeSnapshot ──────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct NodeSnapshot {
    pub node_id: String,
    pub run_state: Option<RunState>,
    pub epoch: u64,
    pub step: u64,
    pub warmup: WarmupSnapshot,
    /// (step, loss) — one entry per TrainingFinished with a loss value.
    pub losses: Vec<(u64, f64)>,
    pub health_check_steps: Vec<u64>,
    pub last_error: Option<String>,
    /// Instantaneous TX throughput in bytes/sec (derived from consecutive ResourceSnapshots).
    pub network_tx_bps: Option<u64>,
    /// Instantaneous RX throughput in bytes/sec.
    pub network_rx_bps: Option<u64>,
    pub p2p: P2PSnapshot,
    pub train: TrainSnapshot,
    pub cooldown: CooldownSnapshot,
    /// Last ResourceSnapshot seen, used to compute network bps deltas.
    pub last_resource: Option<(DateTime<Utc>, ResourceSnapshot)>,
}

impl NodeSnapshot {
    pub fn new(node_id: String) -> Self {
        Self {
            node_id,
            run_state: None,
            epoch: 0,
            step: 0,
            warmup: WarmupSnapshot::default(),
            losses: Vec::new(),
            health_check_steps: Vec::new(),
            last_error: None,
            network_tx_bps: None,
            network_rx_bps: None,
            p2p: P2PSnapshot::default(),
            train: TrainSnapshot::default(),
            cooldown: CooldownSnapshot::default(),
            last_resource: None,
        }
    }
}

// ── ClusterSnapshot ───────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct ClusterSnapshot {
    pub timestamp: DateTime<Utc>,
    pub coordinator: Option<CoordinatorStateSnapshot>,
    pub nodes: IndexMap<String, NodeSnapshot>,
    /// Cluster-level per-batch view for the current step, merged from coordinator
    /// assignments (assigned_to) and per-node events (downloads, trained_by).
    /// Cleared and re-seeded whenever the coordinator reports a new step.
    pub step_batches: BTreeMap<BatchId, ClusterBatchView>,
    /// Batch view from the previous step, retained when the coordinator advances.
    /// Nodes may still emit download/train events for these batches after the step
    /// has advanced (e.g. distro result application in progress).
    pub prev_step_batches: BTreeMap<BatchId, ClusterBatchView>,
}

impl ClusterSnapshot {
    pub fn new() -> Self {
        Self {
            timestamp: Utc::now(),
            coordinator: None,
            nodes: IndexMap::new(),
            step_batches: BTreeMap::new(),
            prev_step_batches: BTreeMap::new(),
        }
    }
}

impl Default for ClusterSnapshot {
    fn default() -> Self {
        Self::new()
    }
}

// ── ClusterProjection ─────────────────────────────────────────────────────────

pub struct ClusterProjection {
    snapshot: ClusterSnapshot,
}

impl ClusterProjection {
    pub fn new() -> Self {
        Self {
            snapshot: ClusterSnapshot::new(),
        }
    }

    pub fn from_snapshot(snapshot: ClusterSnapshot) -> Self {
        Self { snapshot }
    }

    pub fn into_snapshot(self) -> ClusterSnapshot {
        self.snapshot
    }

    pub fn apply_node_event(&mut self, node_id: &str, event: &Event) {
        self.snapshot.timestamp = event.timestamp;

        // ── Phase 1: per-node mutations ───────────────────────────────────────
        // Scoped so the &mut NodeSnapshot borrow on self.snapshot.nodes is released
        // before phase 2 touches self.snapshot.step_batches.
        {
            let node = self
                .snapshot
                .nodes
                .entry(node_id.to_string())
                .or_insert_with(|| NodeSnapshot::new(node_id.to_string()));

            match &event.data {
                // ── Client ───────────────────────────────────────────────────
                EventData::Client(client) => match client {
                    Client::StateChanged(sc) => {
                        node.run_state = Some(sc.new_state);
                        node.epoch = sc.epoch;
                        node.step = sc.step;
                        if sc.new_state == RunState::WaitingForMembers {
                            node.warmup = WarmupSnapshot::default();
                        }
                        if sc.new_state == RunState::Cooldown {
                            node.cooldown = CooldownSnapshot::default();
                        }
                    }
                    Client::HealthCheckFailed(hcf) => {
                        node.health_check_steps.push(hcf.current_step);
                    }
                    Client::Error(e) => {
                        node.last_error = Some(e.message.clone());
                    }
                    Client::Warning(_) => {}
                },

                // ── Train ────────────────────────────────────────────────────
                EventData::Train(train) => match train {
                    Train::BatchesAssigned(ba) => {
                        node.train.batches_assigned = ba.num_batches as u64;
                        node.train.batch_downloads.clear();
                        node.train.training_in_progress = false;
                        node.train.witness = None;
                    }
                    Train::BatchDataDownloadStart(_) => {
                        for batch_id in batch_id_tags(event) {
                            node.train
                                .batch_downloads
                                .insert(*batch_id, BatchDownload { result: None });
                        }
                    }
                    Train::BatchDataDownloadComplete(c) => {
                        for batch_id in batch_id_tags(event) {
                            if let Some(dl) = node.train.batch_downloads.get_mut(batch_id) {
                                dl.result = Some(c.result);
                            }
                        }
                    }
                    Train::TrainingStarted(_) => {
                        node.train.training_in_progress = true;
                    }
                    Train::TrainingFinished(crate::train::TrainingFinished { step, loss }) => {
                        node.train.training_in_progress = false;
                        if let Some(loss) = *loss {
                            node.losses.push((*step, loss));
                        }
                        node.step = *step;
                    }
                    Train::WitnessElected(we) => {
                        node.train.witness = Some(WitnessInfo {
                            step: we.step,
                            round: we.round,
                            epoch: we.epoch,
                            index: we.index,
                            committee_position: we.committee_position,
                        });
                    }
                    Train::UntrainedBatchWarning(ubw) => {
                        for batch_id in batch_id_tags(event) {
                            node.train
                                .untrained_warnings
                                .push((*batch_id, ubw.expected_trainer.clone()));
                        }
                    }
                    Train::DistroResultDeserializeStarted(_)
                    | Train::DistroResultDeserializeComplete(_) => {}
                    Train::ApplyDistroResultsStart(_) => {
                        node.train.last_distro_batch_ids = batch_id_tags(event).cloned().collect();
                        node.train.last_distro_ok = None;
                    }
                    Train::ApplyDistroResultsComplete(arc) => {
                        node.train.last_distro_ok = Some(arc.0.is_ok());
                    }
                    Train::DistroResultAddedToConsensus(_) => {}
                },

                // ── Warmup ───────────────────────────────────────────────────
                EventData::Warmup(warmup) => match warmup {
                    Warmup::P2PParamInfoRequest(_) | Warmup::P2PParamInfoResponse(_) => {
                        if node.warmup.phase == WarmupPhase::Idle {
                            node.warmup.phase = WarmupPhase::NegotiatingP2P;
                        }
                    }
                    Warmup::CheckpointDownloadStarted(cds) => {
                        node.warmup.phase = WarmupPhase::Downloading;
                        node.warmup.download_total_bytes = Some(cds.size_bytes);
                        node.warmup.download_bytes = 0;
                    }
                    Warmup::CheckpointDownloadProgress(cdp) => {
                        node.warmup.download_bytes = cdp.bytes_downloaded;
                    }
                    Warmup::CheckpointDownloadComplete(_) => {}
                    Warmup::ModelLoadStarted(_) => {
                        node.warmup.phase = WarmupPhase::LoadingModel;
                    }
                    Warmup::ModelLoadComplete(_) => {
                        node.warmup.phase = WarmupPhase::Complete;
                        node.warmup.model_loaded = true;
                    }
                },

                // ── Cooldown ─────────────────────────────────────────────────
                EventData::Cooldown(cooldown) => match cooldown {
                    Cooldown::ModelSerializationStarted(_) => {
                        node.cooldown.is_checkpointer = true;
                    }
                    Cooldown::ModelSerializationFinished(msf) => {
                        node.cooldown.serialization_ok = Some(msf.success);
                        node.cooldown.serialization_error = msf.error_string.clone();
                    }
                    Cooldown::CheckpointWriteStarted(_) => {}
                    Cooldown::CheckpointWriteFinished(cwf) => {
                        node.cooldown.checkpoint_write_ok = Some(cwf.success);
                    }
                    Cooldown::CheckpointUploadStarted(_) => {}
                    Cooldown::CheckpointUploadProgress(cup) => {
                        node.cooldown.upload_bytes = cup.bytes_uploaded;
                    }
                    Cooldown::CheckpointUploadFinished(cuf) => {
                        node.cooldown.upload_ok = Some(cuf.success);
                        node.cooldown.upload_error = cuf.error_string.clone();
                        if cuf.success {
                            node.warmup = WarmupSnapshot::default();
                        }
                    }
                },

                // ── P2P ──────────────────────────────────────────────────────
                EventData::P2P(p2p) => match p2p {
                    P2P::ConnectionChanged(cc) => {
                        let existing_latency = node
                            .p2p
                            .peers
                            .get(&cc.endpoint_id)
                            .and_then(|p| p.latency_ms);
                        node.p2p.peers.insert(
                            cc.endpoint_id,
                            PeerInfo {
                                connection_path: cc.connection_path.clone(),
                                latency_ms: existing_latency,
                            },
                        );
                    }
                    P2P::GossipNeighborsChanged(gnc) => {
                        for removed in &gnc.removed_neighbors {
                            node.p2p.gossip_neighbors.remove(removed);
                        }
                        for added in &gnc.new_neighbors {
                            node.p2p.gossip_neighbors.insert(*added);
                        }
                    }
                    P2P::GossipLagged(_) => {
                        // nothing
                    }
                    P2P::ConnectionLatencyChanged(clc) => {
                        if let Some(peer) = node.p2p.peers.get_mut(&clc.endpoint_id) {
                            peer.latency_ms = Some(clc.latency_ms);
                        }
                    }
                    P2P::BlobAddedToStore(_) => {
                        node.p2p.blobs_in_store += 1;
                    }
                    P2P::BlobUploadStarted(crate::p2p::BlobUploadStarted {
                        to_endpoint_id,
                        size_bytes,
                    }) => {
                        if let Some(tag_id) = operation_id(event) {
                            node.p2p.uploads.insert(
                                tag_id,
                                BlobTransfer {
                                    peer_endpoint_id: *to_endpoint_id,
                                    size_bytes: *size_bytes,
                                    bytes_transferred: 0,
                                    result: None,
                                },
                            );
                        }
                    }
                    P2P::BlobUploadProgress(bup) => {
                        if let Some(tag_id) = operation_id(event) {
                            if let Some(t) = node.p2p.uploads.get_mut(&tag_id) {
                                t.bytes_transferred = bup.bytes_transferred;
                            }
                        }
                    }
                    P2P::BlobUploadCompleted(buc) => {
                        if let Some(tag_id) = operation_id(event) {
                            if let Some(t) = node.p2p.uploads.get_mut(&tag_id) {
                                t.result = Some(buc.0.clone());
                            }
                        }
                    }
                    P2P::BlobDownloadStarted(bds) => {
                        if let Some(tag_id) = operation_id(event) {
                            node.p2p.downloads.insert(
                                tag_id,
                                BlobTransfer {
                                    peer_endpoint_id: EndpointId::from_bytes(&[0; 32]).unwrap(),
                                    size_bytes: bds.size_bytes,
                                    bytes_transferred: 0,
                                    result: None,
                                },
                            );
                        }
                    }
                    P2P::BlobDownloadProgress(bdp) => {
                        if let Some(tag_id) = operation_id(event) {
                            if let Some(t) = node.p2p.downloads.get_mut(&tag_id) {
                                t.bytes_transferred = bdp.bytes_transferred;
                            }
                        }
                    }
                    P2P::BlobDownloadCompleted(bdc) => {
                        if let Some(tag_id) = operation_id(event) {
                            if let Some(t) = node.p2p.downloads.get_mut(&tag_id) {
                                t.result = Some(if bdc.success {
                                    Ok(())
                                } else {
                                    Err(bdc.error_string.clone().unwrap_or_default())
                                });
                            }
                        }
                    }
                    P2P::GossipMessageSent(_) => {
                        node.p2p.gossip_sent += 1;
                    }
                    P2P::GossipMessageReceived(_) => {
                        node.p2p.gossip_recv += 1;
                    }
                    P2P::BlobDownloadRequested(_) => {}
                },

                // ── ResourceSnapshot ─────────────────────────────────────────
                EventData::ResourceSnapshot(rs) => {
                    if let Some((prev_ts, ref prev_rs)) = node.last_resource {
                        let dt_secs = (event.timestamp - prev_ts).num_milliseconds() as u64 / 1000;
                        if dt_secs > 0 {
                            node.network_tx_bps = Some(
                                rs.network_bytes_sent_total
                                    .saturating_sub(prev_rs.network_bytes_sent_total)
                                    / dt_secs,
                            );
                            node.network_rx_bps = Some(
                                rs.network_bytes_recv_total
                                    .saturating_sub(prev_rs.network_bytes_recv_total)
                                    / dt_secs,
                            );
                        }
                    }
                    node.last_resource = Some((event.timestamp, rs.clone()));
                }

                _ => {}
            }
        }

        // ── Phase 2: cluster-level step_batches updates ───────────────────────

        match &event.data {
            EventData::Train(Train::BatchDataDownloadStart(_)) => {
                for batch_id in batch_id_tags(event) {
                    let use_prev = self.in_prev(*batch_id);
                    let view = if use_prev {
                        self.snapshot
                            .prev_step_batches
                            .entry(*batch_id)
                            .or_default()
                    } else {
                        self.snapshot.step_batches.entry(*batch_id).or_default()
                    };
                    view.downloads
                        .insert(node_id.to_string(), BatchDownload { result: None });
                }
            }
            EventData::Train(Train::BatchDataDownloadComplete(c)) => {
                for batch_id in batch_id_tags(event) {
                    let use_prev = self.in_prev(*batch_id);
                    let map = if use_prev {
                        &mut self.snapshot.prev_step_batches
                    } else {
                        &mut self.snapshot.step_batches
                    };
                    if let Some(view) = map.get_mut(batch_id) {
                        if let Some(dl) = view.downloads.get_mut(node_id) {
                            dl.result = Some(c.result);
                        }
                    }
                }
            }
            EventData::Train(Train::TrainingFinished(_)) => {
                // Credit all batches assigned to this node in either step map.
                for map in [
                    &mut self.snapshot.step_batches,
                    &mut self.snapshot.prev_step_batches,
                ] {
                    let to_mark: Vec<BatchId> = map
                        .iter()
                        .filter(|(_, v)| v.assigned_to.as_deref() == Some(node_id))
                        .map(|(id, _)| *id)
                        .collect();
                    for batch_id in to_mark {
                        if let Some(view) = map.get_mut(&batch_id) {
                            view.trained_by.insert(node_id.to_string());
                        }
                    }
                }
            }
            _ => {}
        }
    }

    pub fn apply_coordinator(&mut self, update: CoordinatorStateSnapshot) {
        self.snapshot.timestamp = update.timestamp;

        // If the step changed, clear stale batch data and re-seed from new assignments.
        let step_changed = self.snapshot.coordinator.as_ref().map(|c| c.step) != Some(update.step);
        if step_changed {
            // Preserve the outgoing step's batch data — nodes may still emit events
            // (distro result downloads, late TrainingFinished) after the coordinator
            // has advanced to the new step.
            self.snapshot.prev_step_batches = std::mem::take(&mut self.snapshot.step_batches);
            for (batch_id, node_id) in &update.batch_assignments {
                self.snapshot.step_batches.insert(
                    *batch_id,
                    ClusterBatchView {
                        assigned_to: Some(node_id.clone()),
                        downloads: IndexMap::new(),
                        trained_by: HashSet::new(),
                    },
                );
            }
        } else {
            // Same step — just refresh assigned_to in case assignments arrived late.
            for (batch_id, node_id) in &update.batch_assignments {
                self.snapshot
                    .step_batches
                    .entry(*batch_id)
                    .or_default()
                    .assigned_to = Some(node_id.clone());
            }
        }

        self.snapshot.coordinator = Some(update);
    }

    pub fn snapshot(&self) -> &ClusterSnapshot {
        &self.snapshot
    }

    // Returns true if the batch_id belongs to the previous step's map
    // (i.e. it's not in the current step but IS in the previous step).
    fn in_prev(&self, batch_id: BatchId) -> bool {
        !self.snapshot.step_batches.contains_key(&batch_id)
            && self.snapshot.prev_step_batches.contains_key(&batch_id)
    }
}

impl Default for ClusterProjection {
    fn default() -> Self {
        Self::new()
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::events::EventData;
    use crate::{Tags, client, cooldown, train, warmup};
    use chrono::Utc;

    fn make_event(data: EventData) -> Event {
        Event {
            timestamp: Utc::now(),
            tags: Tags::default(),
            data,
        }
    }

    fn make_event_with_tags(data: EventData, tags: Tags) -> Event {
        Event {
            timestamp: Utc::now(),
            tags,
            data,
        }
    }

    #[test]
    fn test_warmup_phase_transitions() {
        let mut proj = ClusterProjection::new();
        let node_id = "node-1";

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Warmup(
                crate::events::Warmup::P2PParamInfoResponse(warmup::P2PParamInfoResponse),
            )),
        );
        assert_eq!(
            proj.snapshot().nodes[node_id].warmup.phase,
            WarmupPhase::NegotiatingP2P
        );

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Warmup(
                crate::events::Warmup::CheckpointDownloadStarted(
                    warmup::CheckpointDownloadStarted { size_bytes: 1024 },
                ),
            )),
        );
        assert_eq!(
            proj.snapshot().nodes[node_id].warmup.phase,
            WarmupPhase::Downloading
        );
        assert_eq!(
            proj.snapshot().nodes[node_id].warmup.download_total_bytes,
            Some(1024)
        );

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Warmup(
                crate::events::Warmup::CheckpointDownloadProgress(
                    warmup::CheckpointDownloadProgress {
                        bytes_downloaded: 512,
                    },
                ),
            )),
        );
        assert_eq!(proj.snapshot().nodes[node_id].warmup.download_bytes, 512);

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Warmup(crate::events::Warmup::ModelLoadStarted(
                warmup::ModelLoadStarted,
            ))),
        );
        assert_eq!(
            proj.snapshot().nodes[node_id].warmup.phase,
            WarmupPhase::LoadingModel
        );

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Warmup(crate::events::Warmup::ModelLoadComplete(
                warmup::ModelLoadComplete,
            ))),
        );
        assert_eq!(
            proj.snapshot().nodes[node_id].warmup.phase,
            WarmupPhase::Complete
        );
        assert!(proj.snapshot().nodes[node_id].warmup.model_loaded);
    }

    #[test]
    fn test_state_changed_updates_node() {
        let mut proj = ClusterProjection::new();
        let node_id = "node-2";

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Client(crate::events::Client::StateChanged(
                client::StateChanged {
                    old_state: RunState::Uninitialized,
                    new_state: RunState::RoundTrain,
                    epoch: 2,
                    step: 7,
                },
            ))),
        );

        let node = &proj.snapshot().nodes[node_id];
        assert_eq!(node.run_state, Some(RunState::RoundTrain));
        assert_eq!(node.epoch, 2);
        assert_eq!(node.step, 7);
    }

    #[test]
    fn test_training_finished_records_loss() {
        let mut proj = ClusterProjection::new();
        let node_id = "node-3";

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Train(crate::events::Train::TrainingFinished(
                train::TrainingFinished {
                    step: 5,
                    loss: Some(2.5),
                },
            ))),
        );

        let node = &proj.snapshot().nodes[node_id];
        assert_eq!(node.losses, vec![(5, 2.5)]);
        assert_eq!(node.step, 5);
    }

    #[test]
    fn test_warmup_resets_on_waiting_for_members() {
        let mut proj = ClusterProjection::new();
        let node_id = "node-4";

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Warmup(crate::events::Warmup::ModelLoadComplete(
                warmup::ModelLoadComplete,
            ))),
        );
        assert_eq!(
            proj.snapshot().nodes[node_id].warmup.phase,
            WarmupPhase::Complete
        );

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Client(crate::events::Client::StateChanged(
                client::StateChanged {
                    old_state: RunState::Cooldown,
                    new_state: RunState::WaitingForMembers,
                    epoch: 2,
                    step: 0,
                },
            ))),
        );
        assert_eq!(
            proj.snapshot().nodes[node_id].warmup.phase,
            WarmupPhase::Idle
        );
    }

    #[test]
    fn test_batch_download_tracked_by_tag() {
        let mut proj = ClusterProjection::new();
        let node_id = "node-5";
        let batch_id = BatchId(psyche_core::ClosedInterval { start: 0, end: 9 });

        proj.apply_node_event(
            node_id,
            &make_event_with_tags(
                EventData::Train(crate::events::Train::BatchDataDownloadStart(
                    train::BatchDataDownloadStart,
                )),
                Tags {
                    batch_ids: Some(vec![batch_id]),
                    ..Default::default()
                },
            ),
        );

        let node = &proj.snapshot().nodes[node_id];
        assert!(node.train.batch_downloads.contains_key(&batch_id));

        // Also mirrored into cluster step_batches
        assert!(proj.snapshot().step_batches.contains_key(&batch_id));
    }

    #[test]
    fn test_cooldown_checkpointer_flag() {
        let mut proj = ClusterProjection::new();
        let node_id = "node-6";

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Client(crate::events::Client::StateChanged(
                client::StateChanged {
                    old_state: RunState::RoundWitness,
                    new_state: RunState::Cooldown,
                    epoch: 1,
                    step: 10,
                },
            ))),
        );
        assert!(!proj.snapshot().nodes[node_id].cooldown.is_checkpointer);

        proj.apply_node_event(
            node_id,
            &make_event(EventData::Cooldown(
                crate::events::Cooldown::ModelSerializationStarted(
                    cooldown::ModelSerializationStarted,
                ),
            )),
        );
        assert!(proj.snapshot().nodes[node_id].cooldown.is_checkpointer);
    }

    #[test]
    fn test_p2p_gossip_neighbors() {
        use crate::events::p2p;

        let mut proj = ClusterProjection::new();
        let node_id = "node-7";

        let ep1 = iroh::SecretKey::generate(&mut rand::rng()).public();
        let ep2 = iroh::SecretKey::generate(&mut rand::rng()).public();

        proj.apply_node_event(
            node_id,
            &make_event(EventData::P2P(crate::events::P2P::GossipNeighborsChanged(
                p2p::GossipNeighborsChanged {
                    removed_neighbors: vec![],
                    new_neighbors: vec![ep1, ep2],
                },
            ))),
        );

        let neighbors = &proj.snapshot().nodes[node_id].p2p.gossip_neighbors;
        assert!(neighbors.contains(&ep1));
        assert!(neighbors.contains(&ep2));

        proj.apply_node_event(
            node_id,
            &make_event(EventData::P2P(crate::events::P2P::GossipNeighborsChanged(
                p2p::GossipNeighborsChanged {
                    removed_neighbors: vec![ep1],
                    new_neighbors: vec![],
                },
            ))),
        );

        let neighbors = &proj.snapshot().nodes[node_id].p2p.gossip_neighbors;
        assert!(!neighbors.contains(&ep1));
        assert!(neighbors.contains(&ep2));
    }

    #[test]
    fn test_coordinator_step_batches() {
        let mut proj = ClusterProjection::new();

        let b1 = BatchId(psyche_core::ClosedInterval { start: 0, end: 4 });
        let b2 = BatchId(psyche_core::ClosedInterval { start: 5, end: 9 });

        let mut assignments = BTreeMap::new();
        assignments.insert(b1, "node-A".to_string());
        assignments.insert(b2, "node-B".to_string());

        proj.apply_coordinator(CoordinatorStateSnapshot {
            timestamp: Utc::now(),
            run_state: RunState::RoundTrain,
            epoch: 0,
            step: 1,
            checkpoint: psyche_coordinator::model::Checkpoint::Ephemeral,
            client_ids: vec![],
            min_clients: 1,
            batch_assignments: assignments,
        });

        assert_eq!(
            proj.snapshot().step_batches[&b1].assigned_to.as_deref(),
            Some("node-A")
        );
        assert_eq!(
            proj.snapshot().step_batches[&b2].assigned_to.as_deref(),
            Some("node-B")
        );

        // New step clears old batch data
        proj.apply_coordinator(CoordinatorStateSnapshot {
            timestamp: Utc::now(),
            run_state: RunState::RoundTrain,
            epoch: 0,
            step: 2,
            checkpoint: psyche_coordinator::model::Checkpoint::Ephemeral,
            client_ids: vec![],
            min_clients: 1,
            batch_assignments: BTreeMap::new(),
        });

        assert!(proj.snapshot().step_batches.is_empty());
    }
}
