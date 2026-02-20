use std::collections::HashMap;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use chrono::{DateTime, Utc};

use std::collections::BTreeMap;

use crate::events::{CoordinatorRecord, Event};
use crate::projection::{ClusterProjection, ClusterSnapshot, CoordinatorStateSnapshot};
use crate::store::try_decode_cobs_frame;

const CHECKPOINT_INTERVAL: usize = 100;
/// Subdirectory name under the events dir that holds coordinator records.
const COORDINATOR_SUBDIR: &str = "coordinator";

fn coordinator_record_to_snapshot(rec: CoordinatorRecord) -> CoordinatorStateSnapshot {
    CoordinatorStateSnapshot {
        timestamp: rec.timestamp,
        run_state: rec.run_state,
        epoch: rec.epoch as u64,
        step: rec.step as u64,
        checkpoint: rec.checkpoint,
        client_ids: rec.clients.iter().map(|c| c.id.clone()).collect(),
        min_clients: rec.min_clients as usize,
        batch_assignments: BTreeMap::new(),
    }
}

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

    pub fn node_id(&self) -> Option<&str> {
        match self {
            TimelineEntry::Node { node_id, .. } => Some(node_id),
            TimelineEntry::Coordinator { .. } => None,
        }
    }

    pub fn event_name(&self) -> String {
        match self {
            TimelineEntry::Coordinator { .. } => "coordinator update".to_string(),
            TimelineEntry::Node { event, .. } => event.data.to_string(),
        }
    }
}

/// Tracks the position (bytes consumed) in each .postcard file so `refresh()`
/// can incrementally read only new events.
struct LiveSource {
    dir: PathBuf,
    /// Maps each .postcard file → bytes successfully decoded so far.
    file_positions: HashMap<PathBuf, u64>,
}

pub struct ClusterTimeline {
    entries: Vec<TimelineEntry>,
    /// Snapshot materialized BEFORE applying the entry at `idx`.
    /// Stored every CHECKPOINT_INTERVAL entries for O(sqrt N) scrub.
    checkpoints: Vec<(usize, ClusterSnapshot)>,
    /// Present when the timeline was created from a directory; enables `refresh()`.
    live_source: Option<LiveSource>,
}

impl ClusterTimeline {
    pub fn new() -> Self {
        Self {
            entries: Vec::new(),
            checkpoints: Vec::new(),
            live_source: None,
        }
    }

    /// Scan all .postcard files under `dir` (recursing into node_id subdirs),
    /// decode events, sort by timestamp, and track file positions for live refresh.
    pub fn from_events_dir(dir: &Path) -> io::Result<Self> {
        let mut file_positions: HashMap<PathBuf, u64> = HashMap::new();
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

            // The coordinator subdir uses a different record type — handled below.
            if node_id == COORDINATOR_SUBDIR {
                continue;
            }

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
                file_positions.insert(file_path, cursor as u64);
            }
        }

        // Scan coordinator subdirectory for CoordinatorRecord frames.
        let coordinator_dir = dir.join(COORDINATOR_SUBDIR);
        if coordinator_dir.is_dir() {
            let mut postcard_files: Vec<PathBuf> = std::fs::read_dir(&coordinator_dir)?
                .filter_map(|e| e.ok())
                .map(|e| e.path())
                .filter(|p| p.extension().is_some_and(|ext| ext == "postcard"))
                .collect();
            postcard_files.sort();

            for file_path in postcard_files {
                let data = std::fs::read(&file_path)?;
                let mut cursor = 0;
                while cursor < data.len() {
                    match try_decode_cobs_frame::<CoordinatorRecord>(&data, &mut cursor) {
                        Some(rec) => {
                            let snapshot = coordinator_record_to_snapshot(rec);
                            raw_entries.push(TimelineEntry::Coordinator {
                                timestamp: snapshot.timestamp,
                                state: snapshot,
                            });
                        }
                        None => break,
                    }
                }
                file_positions.insert(file_path, cursor as u64);
            }
        }

        raw_entries.sort_by_key(|e| e.timestamp());

        let mut timeline = Self {
            entries: raw_entries,
            checkpoints: Vec::new(),
            live_source: Some(LiveSource {
                dir: dir.to_path_buf(),
                file_positions,
            }),
        };
        timeline.rebuild_checkpoints();
        Ok(timeline)
    }

    /// Incrementally scan the source directory for new events appended since the
    /// last load or refresh. Returns `true` if any new events were found.
    ///
    /// Only callable when the timeline was created via `from_events_dir`.
    pub fn refresh(&mut self) -> io::Result<bool> {
        // Clone what we need to avoid holding a borrow while mutating self.
        let (dir, current_positions) = match &self.live_source {
            None => return Ok(false),
            Some(live) => (live.dir.clone(), live.file_positions.clone()),
        };

        let mut new_entries: Vec<TimelineEntry> = Vec::new();
        let mut updated_positions: HashMap<PathBuf, u64> = HashMap::new();

        // Walk node subdirectories — new nodes may have appeared since last scan.
        let Ok(read_dir) = std::fs::read_dir(&dir) else {
            return Ok(false);
        };

        for dir_entry in read_dir.filter_map(|e| e.ok()) {
            let node_dir = dir_entry.path();
            if !node_dir.is_dir() {
                continue;
            }
            let node_id = node_dir
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or("")
                .to_string();

            // Coordinator subdir handled separately below.
            if node_id == COORDINATOR_SUBDIR {
                continue;
            }

            let mut postcard_files: Vec<PathBuf> = std::fs::read_dir(&node_dir)
                .ok()
                .into_iter()
                .flatten()
                .filter_map(|e| e.ok())
                .map(|e| e.path())
                .filter(|p| p.extension().is_some_and(|ext| ext == "postcard"))
                .collect();
            postcard_files.sort();

            for file_path in postcard_files {
                let start_pos = *current_positions.get(&file_path).unwrap_or(&0);

                // Cheap size check before opening the file.
                let Ok(meta) = std::fs::metadata(&file_path) else {
                    continue;
                };
                if meta.len() <= start_pos {
                    continue;
                }

                // Read only the new bytes.
                let Ok(mut file) = std::fs::File::open(&file_path) else {
                    continue;
                };
                if file.seek(SeekFrom::Start(start_pos)).is_err() {
                    continue;
                }
                let mut new_data = Vec::new();
                if file.read_to_end(&mut new_data).is_err() {
                    continue;
                }

                let mut cursor = 0;
                while cursor < new_data.len() {
                    match try_decode_cobs_frame::<Event>(&new_data, &mut cursor) {
                        Some(event) => {
                            new_entries.push(TimelineEntry::Node {
                                timestamp: event.timestamp,
                                node_id: node_id.clone(),
                                event,
                            });
                        }
                        None => break,
                    }
                }

                updated_positions.insert(file_path, start_pos + cursor as u64);
            }
        }

        // Incrementally scan the coordinator subdirectory.
        let coordinator_dir = dir.join(COORDINATOR_SUBDIR);
        if coordinator_dir.is_dir() {
            let postcard_files: Vec<PathBuf> = std::fs::read_dir(&coordinator_dir)
                .ok()
                .into_iter()
                .flatten()
                .filter_map(|e| e.ok())
                .map(|e| e.path())
                .filter(|p| p.extension().is_some_and(|ext| ext == "postcard"))
                .collect();

            for file_path in postcard_files {
                let start_pos = *current_positions.get(&file_path).unwrap_or(&0);

                let Ok(meta) = std::fs::metadata(&file_path) else {
                    continue;
                };
                if meta.len() <= start_pos {
                    continue;
                }

                let Ok(mut file) = std::fs::File::open(&file_path) else {
                    continue;
                };
                if file.seek(SeekFrom::Start(start_pos)).is_err() {
                    continue;
                }
                let mut new_data = Vec::new();
                if file.read_to_end(&mut new_data).is_err() {
                    continue;
                }

                let mut cursor = 0;
                while cursor < new_data.len() {
                    match try_decode_cobs_frame::<CoordinatorRecord>(&new_data, &mut cursor) {
                        Some(rec) => {
                            let snapshot = coordinator_record_to_snapshot(rec);
                            new_entries.push(TimelineEntry::Coordinator {
                                timestamp: snapshot.timestamp,
                                state: snapshot,
                            });
                        }
                        None => break,
                    }
                }

                updated_positions.insert(file_path, start_pos + cursor as u64);
            }
        }

        // Apply position updates regardless of whether we got new entries
        // (advances past any partial frames we already tried).
        if let Some(live) = &mut self.live_source {
            live.file_positions.extend(updated_positions);
        }

        if new_entries.is_empty() {
            return Ok(false);
        }

        self.entries.extend(new_entries);
        // Stable sort preserves existing order for entries with equal timestamps.
        self.entries.sort_by_key(|e| e.timestamp());
        self.rebuild_checkpoints();
        Ok(true)
    }

    pub fn push_coordinator(&mut self, state: CoordinatorStateSnapshot) {
        let timestamp = state.timestamp;
        self.entries
            .push(TimelineEntry::Coordinator { timestamp, state });
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

        let (start_idx, base_snapshot) = self
            .checkpoints
            .iter()
            .rev()
            .find(|(ci, _)| *ci <= idx)
            .map(|(ci, snap)| (*ci, snap.clone()))
            .unwrap_or_else(|| (0, ClusterSnapshot::new()));

        let mut proj = ClusterProjection::from_snapshot(base_snapshot);

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
