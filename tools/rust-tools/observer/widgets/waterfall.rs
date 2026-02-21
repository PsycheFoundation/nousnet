use indexmap::IndexMap;
use psyche_event_sourcing::{
    events::{Client, EventData},
    projection::NodeSnapshot,
    timeline::{ClusterTimeline, TimelineEntry},
};
use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Modifier, Style},
    widgets::Widget,
};
use std::collections::BTreeMap;

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Priority {
    Error = 0,
    Train,
    Warmup,
    Cooldown,
    Client,
    Other,
    P2P,
}

fn categorize(data: &EventData) -> Priority {
    match data {
        EventData::Client(Client::ErrorOccurred(_)) => Priority::Error,
        EventData::Train(_) => Priority::Train,
        EventData::Warmup(_) => Priority::Warmup,
        EventData::Cooldown(_) => Priority::Cooldown,
        EventData::P2P(_) => Priority::P2P,
        EventData::Client(_) => Priority::Client,
        _ => Priority::Other,
    }
}

fn cat_color(cat: Priority) -> Color {
    match cat {
        Priority::Error => Color::Red,
        Priority::Train => Color::Green,
        Priority::Warmup => Color::Magenta,
        Priority::Cooldown => Color::Cyan,
        Priority::P2P => Color::Blue,
        Priority::Client => Color::Yellow,
        Priority::Other => Color::DarkGray,
    }
}

/// Horizontal event-track view with a fixed-size zoom window.
///
/// Shows `zoom` timeline entries at a time. `x_scroll` is the index of the
/// first visible entry. The scrubber `cursor` entry is always highlighted.
/// Rows align with `NodeListWidget` via shared `node_scroll`.
pub struct WaterfallWidget<'a> {
    pub timeline: &'a ClusterTimeline,
    pub cursor: usize,
    pub nodes: &'a IndexMap<String, NodeSnapshot>,
    pub selected_node_idx: Option<usize>,
    pub node_scroll: usize,
    /// How many timeline entries fit in the visible window.
    pub zoom: usize,
    /// Index of the first timeline entry in the visible window.
    pub x_scroll: usize,
}

impl<'a> Widget for WaterfallWidget<'a> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        if area.width == 0 || area.height == 0 {
            return;
        }

        let total = self.timeline.len();
        if total == 0 {
            return;
        }

        let zoom = self.zoom.max(1);
        let win_start = self.x_scroll.min(total.saturating_sub(1));
        let win_end = (win_start + zoom).min(total);
        let win_size = (win_end - win_start).max(1);
        let track_w = area.width as usize;

        // Column range for slot `s` (0-based within window).
        let slot_col_start = |s: usize| s * track_w / win_size;
        let slot_col_end = |s: usize| (s + 1) * track_w / win_size;

        // Which slot (if any) holds the cursor.
        let cursor_slot: Option<usize> = if self.cursor >= win_start && self.cursor < win_end {
            Some(self.cursor - win_start)
        } else {
            None
        };

        // Build per-node slot map: node_id → slot_index → (priority, display_name).
        let mut node_events: BTreeMap<&str, BTreeMap<usize, (Priority, String)>> = BTreeMap::new();
        for (i, entry) in self.timeline.entries().iter().enumerate() {
            if i < win_start || i >= win_end {
                continue;
            }
            if let TimelineEntry::Node { node_id, event, .. } = entry {
                let slot = i - win_start;
                let cat = categorize(&event.data);
                let name = event.data.to_string();
                let slot_entry = node_events
                    .entry(node_id.as_str())
                    .or_default()
                    .entry(slot)
                    .or_insert((cat, name.clone()));
                if cat < slot_entry.0 {
                    *slot_entry = (cat, name);
                }
            }
        }

        let node_ids: Vec<&str> = self.nodes.keys().map(|s| s.as_str()).collect();

        for row in 0..area.height as usize {
            let node_row = self.node_scroll + row;
            let Some(&node_id) = node_ids.get(node_row) else {
                break;
            };
            let y = area.y + row as u16;
            let is_selected = self.selected_node_idx == Some(node_row);

            let empty = BTreeMap::new();
            let slot_map = node_events.get(node_id).unwrap_or(&empty);

            // Paint layer 1: fill entire track with dim dots.
            let dots: String = "·".repeat(track_w);
            buf.set_string(area.x, y, &dots, Style::default().fg(Color::DarkGray));

            // Paint layer 2: each event flows rightward until the next occupied slot
            // or cursor slot blocks it.
            for slot in 0..win_size {
                if cursor_slot == Some(slot) {
                    continue; // painted last
                }
                let Some((cat, name)) = slot_map.get(&slot) else {
                    continue;
                };
                // How far can this text flow? Up to the start of the next occupied
                // slot (or cursor slot), or the track edge.
                let stop_col = (slot + 1..win_size)
                    .find(|&s| slot_map.contains_key(&s) || cursor_slot == Some(s))
                    .map(|s| slot_col_start(s))
                    .unwrap_or(track_w);

                let available = stop_col - slot_col_start(slot);
                let text: String = name.chars().take(available).collect();
                let style = if is_selected {
                    Style::default()
                        .fg(cat_color(*cat))
                        .add_modifier(Modifier::BOLD)
                } else {
                    Style::default().fg(cat_color(*cat))
                };
                buf.set_string(area.x + slot_col_start(slot) as u16, y, &text, style);
            }

            // Paint layer 3: cursor slot on top.
            if let Some(cs) = cursor_slot {
                let c_start = slot_col_start(cs);
                let slot_w = slot_col_end(cs).max(c_start + 1) - c_start;
                let (text, fg) = if let Some((cat, name)) = slot_map.get(&cs) {
                    let t: String = name.chars().take(slot_w).collect();
                    (format!("{:<width$}", t, width = slot_w), cat_color(*cat))
                } else {
                    (format!("{:<width$}", "│", width = slot_w), Color::Yellow)
                };
                buf.set_string(
                    area.x + c_start as u16,
                    y,
                    &text[..slot_w.min(text.len())],
                    Style::default()
                        .fg(fg)
                        .bg(Color::DarkGray)
                        .add_modifier(Modifier::BOLD),
                );
            }
        }
    }
}
