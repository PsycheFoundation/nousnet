use psyche_event_sourcing::timeline::ClusterTimeline;
use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, Paragraph, Widget},
};

pub struct EventScrollWidget<'a> {
    pub timeline: &'a ClusterTimeline,
    pub cursor: usize,
    /// None = show all nodes; Some(id) = filter to that node only.
    pub node_filter: Option<&'a str>,
}

impl<'a> Widget for EventScrollWidget<'a> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        let block = Block::default().borders(Borders::TOP);
        let inner = block.inner(area);
        block.render(area, buf);

        let entries = self.timeline.entries();
        let total = entries.len();

        if total == 0 {
            Paragraph::new(Span::styled(
                "No events",
                Style::default().fg(Color::DarkGray),
            ))
            .render(inner, buf);
            return;
        }

        let short_node = |node_id: Option<&str>| -> String {
            match node_id {
                None => "coord".to_string(),
                Some(id) => {
                    if id.len() > 9 {
                        format!("{}…{}", &id[..4], &id[id.len() - 3..])
                    } else {
                        id.to_string()
                    }
                }
            }
        };

        // Build filtered index list.
        let filtered: Vec<usize> = match self.node_filter {
            None => (0..total).collect(),
            Some(nid) => entries
                .iter()
                .enumerate()
                .filter(|(_, e)| e.node_id() == Some(nid))
                .map(|(i, _)| i)
                .collect(),
        };

        if filtered.is_empty() {
            Paragraph::new(Span::styled(
                "No events for this node",
                Style::default().fg(Color::DarkGray),
            ))
            .render(inner, buf);
            return;
        }

        // Find the anchor: closest filtered entry at-or-before cursor.
        let anchor_pos = {
            let p = filtered.partition_point(|&i| i <= self.cursor);
            if p > 0 { p - 1 } else { 0 }
        };

        let start = anchor_pos.saturating_sub(2);
        let end = (anchor_pos + 3).min(filtered.len());
        let window = &filtered[start..end];
        let anchor_in_window = anchor_pos - start;

        let mut lines: Vec<Line> = Vec::new();

        // Pad top if fewer than 2 entries before anchor.
        for _ in 0..2_usize.saturating_sub(anchor_pos.min(2)) {
            lines.push(Line::from(""));
        }

        for (wi, &entry_idx) in window.iter().enumerate() {
            let entry = &entries[entry_idx];
            let node_label = short_node(entry.node_id());
            let event_label = entry.event_name();

            if wi == anchor_in_window {
                lines.push(Line::from(vec![
                    Span::styled(
                        "►",
                        Style::default()
                            .fg(Color::Cyan)
                            .add_modifier(Modifier::BOLD),
                    ),
                    Span::styled(
                        format!("{:<8} ", node_label),
                        Style::default()
                            .fg(Color::Cyan)
                            .add_modifier(Modifier::BOLD),
                    ),
                    Span::styled(
                        event_label,
                        Style::default()
                            .fg(Color::White)
                            .add_modifier(Modifier::BOLD),
                    ),
                ]));
            } else {
                let dist = wi.abs_diff(anchor_in_window);
                let color = if dist == 1 {
                    Color::Gray
                } else {
                    Color::DarkGray
                };
                lines.push(Line::from(vec![
                    Span::raw(" "),
                    Span::styled(format!("{:<8} ", node_label), Style::default().fg(color)),
                    Span::styled(event_label, Style::default().fg(color)),
                ]));
            }
        }

        // Pad bottom if fewer than 2 entries after anchor.
        let after = window.len().saturating_sub(anchor_in_window + 1);
        for _ in 0..2_usize.saturating_sub(after) {
            lines.push(Line::from(""));
        }

        Paragraph::new(lines).render(inner, buf);
    }
}
