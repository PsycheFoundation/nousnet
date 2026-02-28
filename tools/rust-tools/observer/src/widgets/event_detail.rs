use std::collections::HashSet;

use psyche_event_sourcing::{
    events::{Client, EventData},
    timeline::{ClusterTimeline, TimelineEntry},
};
use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Paragraph, Widget, Wrap},
};

use super::waterfall::EventCategory;

fn matches_filter(entry: &TimelineEntry, filter: &HashSet<EventCategory>) -> bool {
    if filter.is_empty() {
        return true;
    }
    match entry {
        TimelineEntry::Node { event, .. } => {
            let cat = event_category(&event.data);
            cat.map_or(false, |c| filter.contains(&c))
        }
        TimelineEntry::Coordinator { .. } => filter.contains(&EventCategory::Coordinator),
    }
}

fn event_category(data: &EventData) -> Option<EventCategory> {
    match data {
        EventData::Client(Client::Error(_) | Client::Warning(_)) => Some(EventCategory::Error),
        EventData::Train(_) => Some(EventCategory::Train),
        EventData::Warmup(_) => Some(EventCategory::Warmup),
        EventData::Cooldown(_) => Some(EventCategory::Cooldown),
        EventData::P2P(_) => Some(EventCategory::P2P),
        EventData::Client(_) => Some(EventCategory::Client),
        _ => None,
    }
}

fn short_node_id(id: &str, max_w: usize) -> String {
    if id.len() <= max_w {
        return id.to_string();
    }
    if max_w < 6 {
        return id[..max_w].to_string();
    }
    let tail = max_w.saturating_sub(5);
    format!("{}…{}", &id[..4], &id[id.len() - tail..])
}

pub struct EventDetailWidget<'a> {
    pub timeline: &'a ClusterTimeline,
    pub cursor: usize,
    pub selected_node_id: Option<&'a str>,
    pub filter: &'a HashSet<EventCategory>,
}

impl<'a> Widget for EventDetailWidget<'a> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        if area.height == 0 || area.width < 5 {
            return;
        }

        let entries = self.timeline.entries();
        let entry = entries.get(self.cursor).filter(|e| {
            matches_filter(e, self.filter)
                && match (e, self.selected_node_id) {
                    (_, None) => true,
                    (TimelineEntry::Node { node_id, .. }, Some(sel)) => node_id.as_str() == sel,
                    (TimelineEntry::Coordinator { .. }, Some(sel)) => sel == "coordinator",
                }
        });

        let Some(entry) = entry else {
            let msg = if self.selected_node_id.is_some() {
                "No matching event at cursor for selected node"
            } else {
                "No matching event at cursor"
            };
            Paragraph::new(Span::styled(
                msg,
                Style::default()
                    .fg(Color::DarkGray)
                    .add_modifier(Modifier::ITALIC),
            ))
            .centered()
            .render(area, buf);
            return;
        };

        let (title, debug_str, ts, entity_label) = match entry {
            TimelineEntry::Node { event, node_id, .. } => {
                let short = short_node_id(node_id, 12);
                (
                    format!("{}", event.data),
                    format!("{:#?}", event.data),
                    event.timestamp,
                    short,
                )
            }
            TimelineEntry::Coordinator { state, timestamp } => (
                format!("coordinator e{} s{}", state.epoch, state.step),
                format!("{:#?}", state),
                *timestamp,
                "coordinator".to_string(),
            ),
        };

        let mut lines: Vec<Line> = Vec::new();

        // Title line.
        lines.push(Line::from(vec![Span::styled(
            format!(" {} ", title),
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD),
        )]));

        // Entity + timestamp.
        lines.push(
            Line::from(format!(" {} · {}", entity_label, ts.format("%H:%M:%S%.3f")))
                .style(Style::default().fg(Color::DarkGray)),
        );

        lines.push(Line::from(""));

        // Debug dump.
        for l in debug_str.lines() {
            lines.push(Line::from(format!(" {l}")).style(Style::default().fg(Color::Gray)));
        }

        Paragraph::new(lines)
            .wrap(Wrap { trim: false })
            .render(area, buf);
    }
}
