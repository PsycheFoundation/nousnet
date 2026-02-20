use std::io;
use std::path::{Path, PathBuf};

use chrono::{DateTime, Utc};

use crate::events::Event;
use crate::projection::{ClusterProjection, ClusterSnapshot, CoordinatorStateSnapshot};
use crate::store::try_decode_cobs_frame;

const CHECKPOINT_INTERVAL: usize = 100;

pub enum TimelineEntry {
    Node {
        timestamp: DateTime<Utc>,
        node_id: String,
        event: Event,
    },
    Coordinator {
        timestamp: DateTime<Utc>,
        state: CoordinatorStateSnapshot,
    },
}

impl TimelineEntry {
    pub fn timestamp(&self) -> DateTime<Utc> {
        match self {
            TimelineEntry::Node { timestamp, .. } => *timestamp,
            TimelineEntry::Coordinator { timestamp, .. } => *timestamp,
        }
    }
}

pub struct ClusterTimeline {
    entries: Vec<TimelineEntry>,
    /// Snapshot materialized BEFORE applying the entry at `idx`.
    /// Stored every CHECKPOINT_INTERVAL entries for O(sqrt N) scrub.
    checkpoints: Vec<(usize, ClusterSnapshot)>,
}

impl ClusterTimeline {
    pub fn new() -> Self {
        Self {
            entries: Vec::new(),
            checkpoints: Vec::new(),
        }
    }

    /// Scan all .postcard files under `dir` (recursing into node_id subdirs),
    /// decode events, and sort by timestamp.
    pub fn from_events_dir(dir: &Path) -> io::Result<Self> {
        let mut raw_entries: Vec<TimelineEntry> = Vec::new();

        for dir_entry in std::fs::read_dir(dir)? {
            let dir_entry = dir_entry?;
            let node_dir = dir_entry.path();
            if !node_dir.is_dir() {
                continue;
            }

            let node_id = node_dir
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or("")
                .to_string();

            let mut postcard_files: Vec<PathBuf> = std::fs::read_dir(&node_dir)?
                .filter_map(|e| e.ok())
                .map(|e| e.path())
                .filter(|p| p.extension().is_some_and(|ext| ext == "postcard"))
                .collect();
            postcard_files.sort();

            for file_path in postcard_files {
                let data = std::fs::read(&file_path)?;
                let mut cursor = 0;
                while cursor < data.len() {
                    match try_decode_cobs_frame::<Event>(&data, &mut cursor) {
                        Some(event) => {
                            raw_entries.push(TimelineEntry::Node {
                                timestamp: event.timestamp,
                                node_id: node_id.clone(),
                                event,
                            });
                        }
                        None => break,
                    }
                }
            }
        }

        raw_entries.sort_by_key(|e| e.timestamp());

        let mut timeline = Self {
            entries: raw_entries,
            checkpoints: Vec::new(),
        };
        timeline.rebuild_checkpoints();
        Ok(timeline)
    }

    pub fn push_coordinator(&mut self, state: CoordinatorStateSnapshot) {
        let timestamp = state.timestamp;
        self.entries
            .push(TimelineEntry::Coordinator { timestamp, state });
        // Rebuild checkpoints to include the new entry
        self.rebuild_checkpoints();
    }

    /// Rebuild the materialized checkpoint index.
    /// Checkpoint at position i stores the projection state BEFORE applying entries[i].
    fn rebuild_checkpoints(&mut self) {
        self.checkpoints.clear();
        let mut proj = ClusterProjection::new();

        for (idx, entry) in self.entries.iter().enumerate() {
            if idx % CHECKPOINT_INTERVAL == 0 {
                self.checkpoints.push((idx, proj.snapshot().clone()));
            }
            match entry {
                TimelineEntry::Node { node_id, event, .. } => {
                    proj.apply_node_event(node_id, event);
                }
                TimelineEntry::Coordinator { state, .. } => {
                    proj.apply_coordinator(state.clone());
                }
            }
        }
    }

    /// Replay entries from the nearest materialized checkpoint to produce a
    /// ClusterSnapshot at position `idx`. O(sqrt N) amortized.
    pub fn snapshot_at(&self, idx: usize) -> ClusterSnapshot {
        if self.entries.is_empty() {
            return ClusterSnapshot::new();
        }
        let idx = idx.min(self.entries.len() - 1);

        // Find nearest checkpoint at or before idx
        let (start_idx, base_snapshot) = self
            .checkpoints
            .iter()
            .rev()
            .find(|(ci, _)| *ci <= idx)
            .map(|(ci, snap)| (*ci, snap.clone()))
            .unwrap_or_else(|| (0, ClusterSnapshot::new()));

        let mut proj = ClusterProjection::from_snapshot(base_snapshot);

        // Replay from start_idx to idx (checkpoint is BEFORE start_idx was applied)
        for i in start_idx..=idx {
            match &self.entries[i] {
                TimelineEntry::Node { node_id, event, .. } => {
                    proj.apply_node_event(node_id, event);
                }
                TimelineEntry::Coordinator { state, .. } => {
                    proj.apply_coordinator(state.clone());
                }
            }
        }

        proj.into_snapshot()
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn entries(&self) -> &[TimelineEntry] {
        &self.entries
    }

    pub fn timestamp_range(&self) -> Option<(DateTime<Utc>, DateTime<Utc>)> {
        let first = self.entries.first()?.timestamp();
        let last = self.entries.last()?.timestamp();
        Some((first, last))
    }
}

impl Default for ClusterTimeline {
    fn default() -> Self {
        Self::new()
    }
}
