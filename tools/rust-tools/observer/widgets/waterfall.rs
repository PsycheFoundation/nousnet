use psyche_event_sourcing::{
    events::{Client, EventData},
    timeline::{ClusterTimeline, TimelineEntry},
};
use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Modifier, Style},
    text::Line,
    widgets::{Block, Borders, Paragraph, Widget},
};
use std::collections::BTreeMap;

/// Public event category used for filtering and the category picker.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EventCategory {
    Error,
    Train,
    Warmup,
    Cooldown,
    P2P,
    Client,
    Coordinator,
}

impl EventCategory {
    pub fn color(self) -> Color {
        match self {
            Self::Error => Color::Red,
            Self::Train => Color::Green,
            Self::Warmup => Color::Magenta,
            Self::Cooldown => Color::Cyan,
            Self::P2P => Color::Blue,
            Self::Client => Color::Yellow,
            Self::Coordinator => Color::White,
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            Self::Error => "error",
            Self::Train => "train",
            Self::Warmup => "warmup",
            Self::Cooldown => "cool",
            Self::P2P => "p2p",
            Self::Client => "client",
            Self::Coordinator => "coord",
        }
    }
    pub fn key(self) -> char {
        match self {
            Self::Error => 'e',
            Self::Train => 't',
            Self::Warmup => 'w',
            Self::Cooldown => 'c',
            Self::P2P => 'p',
            Self::Client => 'l',      // cLient
            Self::Coordinator => 'o', // cOord
        }
    }
}

pub const ALL_CATEGORIES: &[EventCategory] = &[
    EventCategory::Error,
    EventCategory::Train,
    EventCategory::Warmup,
    EventCategory::Cooldown,
    EventCategory::P2P,
    EventCategory::Client,
    EventCategory::Coordinator,
];

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Priority {
    Error = 0,
    Train,
    Warmup,
    Cooldown,
    Client,
    Coordinator,
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
        Priority::Coordinator => Color::White,
        Priority::Other => Color::DarkGray,
    }
}

fn priority_matches(p: Priority, filter: EventCategory) -> bool {
    matches!(
        (p, filter),
        (Priority::Error, EventCategory::Error)
            | (Priority::Train, EventCategory::Train)
            | (Priority::Warmup, EventCategory::Warmup)
            | (Priority::Cooldown, EventCategory::Cooldown)
            | (Priority::P2P, EventCategory::P2P)
            | (Priority::Client, EventCategory::Client)
            | (Priority::Coordinator, EventCategory::Coordinator)
    )
}

/// Horizontal event-track view with a fixed-size zoom window.
///
/// Shows `zoom` timeline entries at a time. `x_scroll` is the index of the
/// first visible entry. The scrubber `cursor` entry is always highlighted.
/// Rows align with `NodeListWidget` via shared `node_scroll`.
pub struct WaterfallWidget<'a> {
    pub timeline: &'a ClusterTimeline,
    pub cursor: usize,
    /// All entity IDs in the timeline, in order of first appearance.
    pub node_ids: &'a [String],
    pub selected_node_idx: Option<usize>,
    pub node_scroll: usize,
    /// How many timeline entries fit in the visible window.
    pub zoom: usize,
    /// Index of the first timeline entry in the visible window.
    pub x_scroll: usize,
    /// When set, only events of this category are shown on tracks.
    pub filter: Option<EventCategory>,
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

        // Which slot (if any) holds the cursor.
        let cursor_slot: Option<usize> = if self.cursor >= win_start && self.cursor < win_end {
            Some(self.cursor - win_start)
        } else {
            None
        };

        // Build per-entity slot map: entity_id → slot_index → (priority, display_name).
        // "coordinator" is treated as a special entity alongside node IDs.
        let mut node_events: BTreeMap<&str, BTreeMap<usize, (Priority, String)>> = BTreeMap::new();
        for (i, entry) in self.timeline.entries().iter().enumerate() {
            if i < win_start || i >= win_end {
                continue;
            }
            let slot = i - win_start;
            match entry {
                TimelineEntry::Node { node_id, event, .. } => {
                    let cat = categorize(&event.data);
                    if let Some(f) = self.filter
                        && !priority_matches(cat, f)
                    {
                        continue;
                    }
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
                TimelineEntry::Coordinator { state, .. } => {
                    if let Some(f) = self.filter
                        && f != EventCategory::Coordinator
                    {
                        continue;
                    }
                    let name = format!("{}", state.run_state);
                    node_events
                        .entry("coordinator")
                        .or_default()
                        .insert(slot, (Priority::Coordinator, name));
                }
            }
        }

        let entries = self.timeline.entries();

        // ── Row 0: timestamp ruler ────────────────────────────────────────────
        let ruler_y = area.y;
        let dashes: String = "─".repeat(track_w);
        buf.set_string(
            area.x,
            ruler_y,
            &dashes,
            Style::default().fg(Color::DarkGray),
        );

        // Pre-scan from the beginning up to win_end to find the training step
        // at each slot. We track the most recent coordinator step seen so far.
        let step_at_slot: Vec<Option<u64>> = {
            let mut cur: Option<u64> = None;
            let mut out = vec![None; win_size];
            for (i, entry) in entries.iter().enumerate().take(win_end) {
                if let TimelineEntry::Coordinator { state, .. } = entry {
                    cur = Some(state.step);
                }
                if i >= win_start {
                    out[i - win_start] = cur;
                }
            }
            out
        };

        // Write a step label at each slot boundary, skipping any that would
        // overlap the previous one. scan carries `last_label_end`.
        (0..win_size)
            .filter_map(|slot| {
                let c_start = slot_col_start(slot);
                entries.get(win_start + slot)?; // confirm entry exists
                let label = match step_at_slot[slot] {
                    Some(step) => format!("step {step}"),
                    None => format!("{}", win_start + slot),
                };
                Some((c_start, label))
            })
            // Only emit when the label changes (suppresses s1 s1 s1 …).
            .scan(None::<String>, |prev, (c_start, label)| {
                if prev.as_deref() == Some(label.as_str()) {
                    Some(None)
                } else {
                    *prev = Some(label.clone());
                    Some(Some((c_start, label)))
                }
            })
            .flatten()
            .take_while(|(c_start, label)| c_start + label.len() <= track_w)
            .scan(0usize, |last_end, (c_start, label)| {
                if c_start < *last_end {
                    Some(None)
                } else {
                    *last_end = c_start + label.len() + 4;
                    Some(Some((c_start, label)))
                }
            })
            .flatten()
            .for_each(|(c_start, label)| {
                buf.set_string(
                    area.x + c_start as u16,
                    ruler_y,
                    &label,
                    Style::default().fg(Color::White),
                );
            });

        let zoom_label = format!(" zoom:{zoom:2} ");
        if zoom_label.len() <= track_w {
            buf.set_string(
                area.x + (track_w - zoom_label.len()) as u16,
                ruler_y,
                &zoom_label,
                Style::default().fg(Color::White),
            );
        }

        for row in 1..area.height as usize {
            // row 1 = first visible node (node_scroll + 0)
            let node_row = self.node_scroll + (row - 1);
            let Some(node_id) = self.node_ids.get(node_row) else {
                break;
            };
            let y = area.y + row as u16;
            let is_selected = self.selected_node_idx == Some(node_row);

            let empty = BTreeMap::new();
            let slot_map = node_events.get(node_id.as_str()).unwrap_or(&empty);

            // Paint layer 1: fill entire track with dim dots.
            let dots: String = "-".repeat(track_w);
            buf.set_string(area.x, y, &dots, Style::default().fg(Color::DarkGray));

            // Paint layer 2: each event flows rightward until the next occupied slot.
            // The cursor slot is painted here too — layer 3 just stamps │ on top.
            for slot in 0..win_size {
                let Some((cat, name)) = slot_map.get(&slot) else {
                    continue;
                };
                let stop_col = (slot + 1..win_size)
                    .find(|&s| slot_map.contains_key(&s))
                    .map(&slot_col_start)
                    .unwrap_or(track_w);

                let available = stop_col - slot_col_start(slot);
                let text: String = "▶".chars().chain(name.chars()).take(available).collect();
                let style = if is_selected {
                    Style::default()
                        .fg(cat_color(*cat))
                        .add_modifier(Modifier::BOLD)
                } else {
                    Style::default().fg(cat_color(*cat))
                };
                buf.set_string(area.x + slot_col_start(slot) as u16, y, &text, style);
            }

            // Paint layer 3: cursor indicator — just │ at the slot's first column,
            // only on the selected row (or every row when nothing is selected).
            // We deliberately don't re-draw the event text here.
            let cursor_on_this_row = self
                .selected_node_idx
                .map(|sel| sel == node_row)
                .unwrap_or(true);
            if cursor_on_this_row && let Some(cs) = cursor_slot {
                let c_start = slot_col_start(cs);
                buf.set_string(
                    area.x + c_start as u16,
                    y,
                    "│",
                    Style::default()
                        .fg(Color::Yellow)
                        .add_modifier(Modifier::BOLD),
                );
            }
        }

        // ── Event detail box (bottom-left, floating) ──────────────────────────
        // When an entity is selected, find its most recent event at or before the
        // cursor. When nothing is selected, use whatever is at the cursor.
        let selected_entity = self.selected_node_idx.and_then(|i| self.node_ids.get(i));
        let passes_filter = |entry: &&TimelineEntry| -> bool {
            match self.filter {
                None => true,
                Some(f) => match entry {
                    TimelineEntry::Node { event, .. } => {
                        priority_matches(categorize(&event.data), f)
                    }
                    TimelineEntry::Coordinator { .. } => f == EventCategory::Coordinator,
                },
            }
        };
        // Only show the detail box when the cursor is *exactly* on a matching entry.
        let detail_entry = entries.get(self.cursor).filter(|e| {
            passes_filter(e)
                && match (e, selected_entity) {
                    (_, None) => true,
                    (TimelineEntry::Node { node_id, .. }, Some(eid)) => {
                        node_id.as_str() == eid.as_str()
                    }
                    (TimelineEntry::Coordinator { .. }, Some(eid)) => eid.as_str() == "coordinator",
                }
        });

        if let Some(entry) = detail_entry {
            let (title, debug_str, ts, entity_label) = match entry {
                TimelineEntry::Node { event, node_id, .. } => {
                    let short = if node_id.len() > 12 {
                        format!("{}…{}", &node_id[..4], &node_id[node_id.len() - 5..])
                    } else {
                        node_id.clone()
                    };
                    (
                        format!(" {} ", event.data),
                        format!("{:#?}", event.data),
                        event.timestamp,
                        short,
                    )
                }
                TimelineEntry::Coordinator { state, timestamp } => (
                    format!(" coordinator e{} s{} ", state.epoch, state.step),
                    format!("{:#?}", state),
                    *timestamp,
                    "coordinator".to_string(),
                ),
            };

            let mut lines: Vec<Line> = vec![
                Line::from(format!(
                    "  {} · {}",
                    entity_label,
                    ts.format("%H:%M:%S%.3f")
                ))
                .style(Style::default().fg(Color::DarkGray)),
            ];
            lines.extend(
                debug_str
                    .lines()
                    .map(|l| Line::from(format!("  {l}")).style(Style::default().fg(Color::Gray))),
            );

            let box_h = (lines.len() as u16 + 2)
                .min(area.height.saturating_sub(1))
                .max(3);
            let box_w = 60u16.min(area.width);
            let box_y = area.y + area.height.saturating_sub(box_h);

            Paragraph::new(lines)
                .block(
                    Block::default()
                        .borders(Borders::ALL)
                        .title(title)
                        .border_style(Style::default().fg(Color::Yellow)),
                )
                .render(
                    Rect {
                        x: area.x,
                        y: box_y,
                        width: box_w,
                        height: box_h,
                    },
                    buf,
                );
        }

        // ── Category filter picker (bottom-right) ─────────────────────────────
        // Each category: ■ be(f)ore(k)ey after  — key char is bold in parens.
        let picker_spans: Vec<ratatui::text::Span> = ALL_CATEGORIES
            .iter()
            .flat_map(|&cat| {
                let active = self.filter == Some(cat);
                let base = if active {
                    Style::default()
                        .fg(Color::Black)
                        .bg(cat.color())
                        .add_modifier(Modifier::BOLD)
                } else {
                    Style::default().fg(cat.color())
                };
                let key_style = base.add_modifier(Modifier::BOLD);

                let label = cat.label();
                let key = cat.key();
                let key_pos = label.find(key).unwrap_or(0);
                let before = &label[..key_pos];
                let after = &label[key_pos + key.len_utf8()..];

                let mut spans: Vec<ratatui::text::Span> = vec![
                    ratatui::text::Span::styled("■", base),
                    ratatui::text::Span::styled(" ", base),
                ];
                if !before.is_empty() {
                    spans.push(ratatui::text::Span::styled(before.to_string(), base));
                }
                spans.push(ratatui::text::Span::styled(format!("({key})"), key_style));
                if !after.is_empty() {
                    spans.push(ratatui::text::Span::styled(after.to_string(), base));
                }
                spans.push(ratatui::text::Span::styled(" ", base));
                spans
            })
            .collect();
        // Width: ■(1) + space(1) + label.len() + "(x)"(+2) + space(1) = label.len() + 5
        let picker_w = ALL_CATEGORIES
            .iter()
            .map(|c| c.label().len() + 5)
            .sum::<usize>() as u16;
        if picker_w <= area.width && area.height > 0 {
            let px = area.x + area.width.saturating_sub(picker_w);
            let py = area.y + area.height - 1;
            Paragraph::new(Line::from(picker_spans)).render(
                Rect {
                    x: px,
                    y: py,
                    width: picker_w,
                    height: 1,
                },
                buf,
            );
        }
    }
}
