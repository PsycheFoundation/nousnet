use chrono::{DateTime, Utc};
use indexmap::IndexMap;
use psyche_coordinator::model::Checkpoint;

use crate::events::{
    Client, Cooldown, ErrorKind, Event, EventData, ResourceSnapshot, RunState, Train, Warmup,
};

#[derive(Debug, Clone)]
pub struct CoordinatorStateSnapshot {
    pub timestamp: DateTime<Utc>,
    pub run_state: RunState,
    pub epoch: u64,
    pub step: u64,
    pub checkpoint: Checkpoint,
    pub client_ids: Vec<String>,
    pub min_clients: usize,
}

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

#[derive(Debug, Clone)]
pub struct NodeSnapshot {
    pub node_id: String,
    pub run_state: Option<RunState>,
    pub epoch: u64,
    pub step: u64,
    pub warmup: WarmupSnapshot,
    /// (epoch, step, loss) — one entry per TrainingFinished event with a loss value.
    pub epoch_losses: Vec<(u64, u64, f64)>,
    pub health_check_steps: Vec<u64>,
    pub last_error: Option<(ErrorKind, String)>,
    /// Instantaneous TX throughput in bytes/sec (derived from consecutive ResourceSnapshots)
    pub network_tx_bps: Option<f64>,
    /// Instantaneous RX throughput in bytes/sec
    pub network_rx_bps: Option<f64>,
    /// Last ResourceSnapshot seen, used to compute deltas
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
            epoch_losses: Vec::new(),
            health_check_steps: Vec::new(),
            last_error: None,
            network_tx_bps: None,
            network_rx_bps: None,
            last_resource: None,
        }
    }
}

#[derive(Debug, Clone)]
pub struct ClusterSnapshot {
    pub timestamp: DateTime<Utc>,
    pub coordinator: Option<CoordinatorStateSnapshot>,
    pub nodes: IndexMap<String, NodeSnapshot>,
}

impl ClusterSnapshot {
    pub fn new() -> Self {
        Self {
            timestamp: Utc::now(),
            coordinator: None,
            nodes: IndexMap::new(),
        }
    }
}

impl Default for ClusterSnapshot {
    fn default() -> Self {
        Self::new()
    }
}

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
        let node = self
            .snapshot
            .nodes
            .entry(node_id.to_string())
            .or_insert_with(|| NodeSnapshot::new(node_id.to_string()));

        match &event.data {
            EventData::Client(client) => match client {
                Client::StateChanged(sc) => {
                    node.run_state = Some(sc.new_state);
                    node.epoch = sc.epoch;
                    node.step = sc.step;
                    if sc.new_state == RunState::WaitingForMembers {
                        node.warmup = WarmupSnapshot::default();
                    }
                }
                Client::HealthCheckFailed(hcf) => {
                    node.health_check_steps.push(hcf.current_step);
                }
                Client::ErrorOccurred(e) => {
                    node.last_error = Some((e.kind, e.message.clone()));
                }
            },
            EventData::Train(train) => match train {
                Train::TrainingFinished(tf) => {
                    if let Some(loss) = tf.loss {
                        node.epoch_losses.push((tf.epoch, tf.step, loss));
                    }
                    node.epoch = tf.epoch;
                    node.step = tf.step;
                }
                _ => {}
            },
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
                Warmup::CheckpointDownloadComplete(_) => {
                    // stay in Downloading phase until model load begins
                }
                Warmup::ModelLoadStarted(_) => {
                    node.warmup.phase = WarmupPhase::LoadingModel;
                }
                Warmup::ModelLoadComplete(_) => {
                    node.warmup.phase = WarmupPhase::Complete;
                    node.warmup.model_loaded = true;
                }
            },
            EventData::Cooldown(cooldown) => match cooldown {
                Cooldown::CheckpointUploadFinished(f) if f.success => {
                    node.warmup = WarmupSnapshot::default();
                }
                _ => {}
            },
            EventData::ResourceSnapshot(rs) => {
                if let Some((prev_ts, ref prev_rs)) = node.last_resource {
                    let dt_secs = (event.timestamp - prev_ts).num_milliseconds() as f64 / 1000.0;
                    if dt_secs > 0.0 {
                        node.network_tx_bps = Some(
                            rs.network_bytes_sent_total
                                .saturating_sub(prev_rs.network_bytes_sent_total)
                                as f64
                                / dt_secs,
                        );
                        node.network_rx_bps = Some(
                            rs.network_bytes_recv_total
                                .saturating_sub(prev_rs.network_bytes_recv_total)
                                as f64
                                / dt_secs,
                        );
                    }
                }
                node.last_resource = Some((event.timestamp, rs.clone()));
            }
            _ => {}
        }
    }

    pub fn apply_coordinator(&mut self, update: CoordinatorStateSnapshot) {
        self.snapshot.timestamp = update.timestamp;
        self.snapshot.coordinator = Some(update);
    }

    pub fn snapshot(&self) -> &ClusterSnapshot {
        &self.snapshot
    }
}

impl Default for ClusterProjection {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::events::EventData;
    use crate::{client, train, warmup};
    use chrono::Utc;

    fn make_event(data: EventData) -> Event {
        Event {
            timestamp: Utc::now(),
            tags: vec![],
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
                    epoch: 1,
                    step: 5,
                    loss: Some(2.5),
                },
            ))),
        );

        let node = &proj.snapshot().nodes[node_id];
        assert_eq!(node.epoch_losses, vec![(1, 5, 2.5)]);
        assert_eq!(node.epoch, 1);
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
}
