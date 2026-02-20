use psyche_event_sourcing::projection::ClusterSnapshot;
use psyche_event_sourcing::timeline::ClusterTimeline;

pub struct App {
    pub timeline: ClusterTimeline,
    pub cursor: usize,
    pub selected_node_idx: usize,
    pub playing: bool,
    pub speed: u64,
    cached_snapshot: Option<(usize, ClusterSnapshot)>,
}

impl App {
    pub fn new(timeline: ClusterTimeline) -> Self {
        let len = timeline.len();
        Self {
            timeline,
            cursor: len.saturating_sub(1),
            selected_node_idx: 0,
            playing: false,
            speed: 1,
            cached_snapshot: None,
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

    pub fn next_node(&mut self) {
        let snap = self.current_snapshot();
        let count = snap.nodes.len();
        if count == 0 {
            return;
        }
        self.selected_node_idx = (self.selected_node_idx + 1) % count;
    }

    pub fn prev_node(&mut self) {
        let snap = self.current_snapshot();
        let count = snap.nodes.len();
        if count == 0 {
            return;
        }
        self.selected_node_idx = self.selected_node_idx.saturating_sub(1);
        if self.selected_node_idx == 0 && count > 0 {
            // wrap not needed since we use saturating_sub
        }
    }

    pub fn toggle_play(&mut self) {
        self.playing = !self.playing;
    }

    pub fn set_speed(&mut self, speed: u64) {
        self.speed = speed;
    }

    /// Advance the cursor if in play mode. Returns true if the cursor moved.
    pub fn tick(&mut self) -> bool {
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
}
