use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::Instant;

use indexmap::IndexMap;
use psyche_event_sourcing::projection::ClusterSnapshot;
use psyche_event_sourcing::timeline::ClusterTimeline;

#[derive(Debug, Clone, Default)]
pub struct NodeFileStats {
    /// Total bytes of all .postcard files on disk for this node.
    pub total_bytes: u64,
    /// Lifetime average write rate: total_bytes / elapsed_since_first_seen.
    pub bytes_per_sec: f64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DetailPanel {
    Timeline,
    Node,
    Loss,
    Batches,
}

impl DetailPanel {
    pub fn next(self) -> DetailPanel {
        match self {
            DetailPanel::Timeline => DetailPanel::Node,
            DetailPanel::Node => DetailPanel::Loss,
            DetailPanel::Loss => DetailPanel::Batches,
            DetailPanel::Batches => DetailPanel::Timeline,
        }
    }
}

pub struct App {
    pub timeline: ClusterTimeline,
    pub cursor: usize,
    /// None = "all nodes" view; Some(i) = node at index i in snapshot.nodes.
    pub selected_node_idx: Option<usize>,
    /// Vertical scroll offset for the node/waterfall rows.
    pub node_scroll: usize,
    pub playing: bool,
    pub speed: u64,
    pub events_dir: PathBuf,
    /// Live file-size stats per node_id, refreshed every tick.
    pub node_file_stats: IndexMap<String, NodeFileStats>,
    pub detail_panel: DetailPanel,
    pub batch_scroll: usize,
    /// Number of timeline entries visible at once in the waterfall x-axis.
    pub waterfall_zoom: usize,
    /// Index of the first timeline entry visible in the waterfall window.
    pub waterfall_x_scroll: usize,
    cached_snapshot: Option<(usize, ClusterSnapshot)>,
    /// Tracks when each node was first observed for lifetime bps calculation.
    node_first_seen: HashMap<String, Instant>,
}

fn scan_node_sizes(events_dir: &Path) -> HashMap<String, u64> {
    let mut sizes = HashMap::new();
    let Ok(entries) = std::fs::read_dir(events_dir) else {
        return sizes;
    };
    for entry in entries.filter_map(|e| e.ok()) {
        let node_dir = entry.path();
        if !node_dir.is_dir() {
            continue;
        }
        let node_id = node_dir
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string();
        let total: u64 = std::fs::read_dir(&node_dir)
            .ok()
            .into_iter()
            .flatten()
            .filter_map(|e| e.ok())
            .filter(|e| e.path().extension().is_some_and(|ext| ext == "postcard"))
            .filter_map(|e| e.metadata().ok())
            .map(|m| m.len())
            .sum();
        sizes.insert(node_id, total);
    }
    sizes
}

impl App {
    pub fn new(timeline: ClusterTimeline, events_dir: PathBuf) -> Self {
        let len = timeline.len();
        Self {
            timeline,
            cursor: len.saturating_sub(1),
            selected_node_idx: None,
            node_scroll: 0,
            playing: false,
            speed: 1,
            events_dir,
            node_file_stats: IndexMap::new(),
            detail_panel: DetailPanel::Timeline,
            batch_scroll: 0,
            waterfall_zoom: 20,
            waterfall_x_scroll: 0,
            cached_snapshot: None,
            node_first_seen: HashMap::new(),
        }
    }

    pub fn current_snapshot(&mut self) -> &ClusterSnapshot {
        let cursor = self.cursor;
        if self.cached_snapshot.as_ref().map(|(c, _)| *c) != Some(cursor) {
            let snap = self.timeline.snapshot_at(cursor);
            self.cached_snapshot = Some((cursor, snap));
        }
        &self.cached_snapshot.as_ref().unwrap().1
    }

    pub fn step_forward(&mut self, n: usize) {
        let max = self.timeline.len().saturating_sub(1);
        self.cursor = (self.cursor + n).min(max);
    }

    pub fn step_backward(&mut self, n: usize) {
        self.cursor = self.cursor.saturating_sub(n);
    }

    pub fn go_first(&mut self) {
        self.cursor = 0;
    }

    pub fn go_last(&mut self) {
        self.cursor = self.timeline.len().saturating_sub(1);
    }

    /// Select next node (↓). Cycles: None → 0 → 1 → … → last → None.
    pub fn next_node(&mut self) {
        let count = self.current_snapshot().nodes.len();
        if count == 0 {
            self.selected_node_idx = None;
            return;
        }
        self.selected_node_idx = match self.selected_node_idx {
            None => Some(0),
            Some(i) if i + 1 < count => Some(i + 1),
            Some(_) => None,
        };
    }

    /// Select previous node (↑). Cycles: None → last → … → 0 → None.
    pub fn prev_node(&mut self) {
        let count = self.current_snapshot().nodes.len();
        if count == 0 {
            self.selected_node_idx = None;
            return;
        }
        self.selected_node_idx = match self.selected_node_idx {
            None => Some(count - 1),
            Some(0) => None,
            Some(i) => Some(i - 1),
        };
    }

    /// Adjust `node_scroll` so the selected node is within the visible viewport.
    pub fn ensure_node_visible(&mut self, viewport_h: usize) {
        if viewport_h == 0 {
            return;
        }
        if let Some(idx) = self.selected_node_idx {
            if idx < self.node_scroll {
                self.node_scroll = idx;
            } else if idx >= self.node_scroll + viewport_h {
                self.node_scroll = idx + 1 - viewport_h;
            }
        }
    }

    pub fn toggle_play(&mut self) {
        self.playing = !self.playing;
    }

    pub fn set_speed(&mut self, speed: u64) {
        self.speed = speed;
    }

    /// Zoom in: halve the number of visible events (min 5).
    pub fn zoom_in(&mut self) {
        self.waterfall_zoom = (self.waterfall_zoom / 2).max(5);
        self.ensure_cursor_visible();
    }

    /// Zoom out: double the number of visible events.
    pub fn zoom_out(&mut self) {
        self.waterfall_zoom = (self.waterfall_zoom * 2).min(self.timeline.len().max(20));
        self.ensure_cursor_visible();
    }

    /// Adjust `waterfall_x_scroll` so the cursor stays within the visible window.
    pub fn ensure_cursor_visible(&mut self) {
        if self.cursor < self.waterfall_x_scroll {
            self.waterfall_x_scroll = self.cursor;
        } else if self.waterfall_zoom > 0
            && self.cursor >= self.waterfall_x_scroll + self.waterfall_zoom
        {
            self.waterfall_x_scroll = self.cursor + 1 - self.waterfall_zoom;
        }
    }

    pub fn cycle_detail_panel(&mut self) {
        self.detail_panel = self.detail_panel.next();
    }

    pub fn scroll_batches_up(&mut self) {
        self.batch_scroll = self.batch_scroll.saturating_sub(1);
    }

    pub fn scroll_batches_down(&mut self) {
        self.batch_scroll += 1;
    }

    pub fn tick(&mut self) -> bool {
        self.refresh_file_stats();

        // Pull in any new events written to disk since last tick.
        let was_at_tail =
            self.timeline.is_empty() || self.cursor >= self.timeline.len().saturating_sub(1);

        if self.timeline.refresh().unwrap_or(false) {
            // New events arrived — the cached snapshot may be stale.
            self.cached_snapshot = None;
            if was_at_tail {
                // Auto-follow: stay pinned to the latest event.
                self.cursor = self.timeline.len().saturating_sub(1);
            }
        }

        if self.playing {
            let max = self.timeline.len().saturating_sub(1);
            if self.cursor < max {
                self.cursor += self.speed as usize;
                if self.cursor > max {
                    self.cursor = max;
                }
                return true;
            } else {
                self.playing = false;
            }
        }
        false
    }

    fn refresh_file_stats(&mut self) {
        let now = Instant::now();
        let current_sizes = scan_node_sizes(&self.events_dir);

        self.node_file_stats.clear();
        for (node_id, &total_bytes) in &current_sizes {
            let first_seen = *self.node_first_seen.entry(node_id.clone()).or_insert(now);
            let elapsed = now.duration_since(first_seen).as_secs_f64();
            let bytes_per_sec = if elapsed > 1.0 {
                total_bytes as f64 / elapsed
            } else {
                0.0
            };
            self.node_file_stats.insert(
                node_id.clone(),
                NodeFileStats {
                    total_bytes,
                    bytes_per_sec,
                },
            );
        }
    }
}
