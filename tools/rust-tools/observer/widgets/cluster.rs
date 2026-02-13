use psyche_event_sourcing::projection::ClusterSnapshot;
use ratatui::{
    buffer::Buffer,
    layout::{Alignment, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, List, ListItem, Paragraph, Widget},
};

pub struct ClusterWidget<'a> {
    pub snapshot: &'a ClusterSnapshot,
    /// None = "all nodes" selected; Some(i) = node at index i.
    pub selected_node_idx: Option<usize>,
}

impl<'a> Widget for ClusterWidget<'a> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        let mut lines: Vec<Line> = Vec::new();

        if let Some(coord) = &self.snapshot.coordinator {
            lines.push(Line::from(vec![
                Span::styled("State: ", Style::default().add_modifier(Modifier::BOLD)),
                Span::raw(format!("{:?}", coord.run_state)),
            ]));
            lines.push(Line::from(vec![
                Span::styled("Epoch: ", Style::default().add_modifier(Modifier::BOLD)),
                Span::raw(format!("{}", coord.epoch)),
                Span::raw("  "),
                Span::styled("Step: ", Style::default().add_modifier(Modifier::BOLD)),
                Span::raw(format!("{}", coord.step)),
            ]));
            lines.push(Line::from(vec![
                Span::styled(
                    "Checkpoint: ",
                    Style::default().add_modifier(Modifier::BOLD),
                ),
                Span::raw(format!("{}", coord.checkpoint)),
            ]));
            lines.push(Line::from(vec![
                Span::styled("Clients: ", Style::default().add_modifier(Modifier::BOLD)),
                Span::raw(format!("{}/{}", coord.client_ids.len(), coord.min_clients)),
            ]));
        } else {
            lines.push(Line::from(Span::styled(
                "No coordinator data",
                Style::default().fg(Color::DarkGray),
            )));
        }

        lines.push(Line::from(""));
        lines.push(Line::from(Span::styled(
            "NODES",
            Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED),
        )));

        // "ALL NODES" row at the top of the list.
        let all_style = if self.selected_node_idx.is_none() {
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD)
        } else {
            Style::default().fg(Color::White)
        };
        let mut node_items: Vec<ListItem> = vec![ListItem::new(Line::from(Span::styled(
            "  ● ALL NODES",
            all_style,
        )))];

        for (i, (node_id, node)) in self.snapshot.nodes.iter().enumerate() {
            let state_str = node
                .run_state
                .map(|s| format!("{:?}", s))
                .unwrap_or_else(|| "—".to_string());

            let warmup_icon = match &node.warmup.phase {
                psyche_event_sourcing::projection::WarmupPhase::Idle => "○",
                psyche_event_sourcing::projection::WarmupPhase::NegotiatingP2P => "◎",
                psyche_event_sourcing::projection::WarmupPhase::Downloading => "⬇",
                psyche_event_sourcing::projection::WarmupPhase::LoadingModel => "⟳",
                psyche_event_sourcing::projection::WarmupPhase::Complete => "●",
            };

            let error_icon = if node.last_error.is_some() { "!" } else { " " };

            let short_id = if node_id.len() > 12 {
                format!("{}…{}", &node_id[..6], &node_id[node_id.len() - 4..])
            } else {
                node_id.clone()
            };

            let style = if self.selected_node_idx == Some(i) {
                Style::default()
                    .fg(Color::Yellow)
                    .add_modifier(Modifier::BOLD)
            } else {
                Style::default()
            };

            node_items.push(ListItem::new(Line::from(vec![Span::styled(
                format!(
                    "{} {} {:<14} {}",
                    error_icon, warmup_icon, short_id, state_str
                ),
                style,
            )])));
        }

        let coord_height = lines.len() as u16;
        if coord_height < area.height {
            let coord_area = Rect {
                x: area.x,
                y: area.y,
                width: area.width,
                height: coord_height.min(area.height),
            };
            Paragraph::new(lines)
                .alignment(Alignment::Left)
                .render(coord_area, buf);

            let nodes_area = Rect {
                x: area.x,
                y: area.y + coord_height,
                width: area.width,
                height: area.height.saturating_sub(coord_height),
            };
            List::new(node_items).render(nodes_area, buf);
        } else {
            Paragraph::new(lines)
                .alignment(Alignment::Left)
                .render(area, buf);
        }
    }
}
