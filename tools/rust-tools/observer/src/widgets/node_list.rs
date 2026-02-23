use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Modifier, Style},
    widgets::{Block, Borders, Widget},
};

pub struct NodeListWidget<'a> {
    pub node_ids: &'a [String],
    pub selected_node_idx: Option<usize>,
    pub node_scroll: usize,
    pub focused: bool,
}

impl<'a> Widget for NodeListWidget<'a> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        let block = Block::default().borders(Borders::ALL).title("[2] nodes");

        let inner = block.inner(area);
        block.render(area, buf);

        {
            let is_selected = self.selected_node_idx.is_none();
            let prefix = if is_selected { "► " } else { "  " };
            let style = if is_selected {
                Style::default()
                    .fg(Color::Cyan)
                    .bg(Color::DarkGray)
                    .add_modifier(Modifier::BOLD)
            } else {
                Style::new()
            };
            let label = format!(
                "{:<width$}",
                prefix.to_owned() + "all nodes",
                width = inner.width as usize
            );
            buf.set_string(inner.x, inner.y, &label, style);
        }
        let max_name = inner.width.saturating_sub(2) as usize;

        for row in 0..(inner.height - 1) as usize {
            let node_row = self.node_scroll + row;
            let Some(node_id) = self.node_ids.get(node_row) else {
                break;
            };
            let y = inner.y + row as u16 + 1;
            let is_selected = self.selected_node_idx == Some(node_row);

            let short_id = if node_id.len() > max_name {
                let tail = max_name.saturating_sub(5);
                format!("{}…{}", &node_id[..4], &node_id[node_id.len() - tail..])
            } else {
                node_id.clone()
            };

            let prefix = if is_selected { "► " } else { "  " };
            let label = format!("{}{:<width$}", prefix, short_id, width = max_name);

            let style = if is_selected {
                Style::default()
                    .fg(Color::Cyan)
                    .bg(Color::DarkGray)
                    .add_modifier(Modifier::BOLD)
            } else {
                Style::default().fg(Color::Gray)
            };

            // Pad to full width so the background covers the entire row.
            let label = format!("{:<width$}", label, width = inner.width as usize);
            let label: String = label.chars().take(inner.width as usize).collect();
            buf.set_string(inner.x, y, &label, style);
        }
    }
}
