use indexmap::IndexMap;
use psyche_event_sourcing::projection::NodeSnapshot;
use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Modifier, Style},
    widgets::Widget,
};

pub struct NodeListWidget<'a> {
    pub nodes: &'a IndexMap<String, NodeSnapshot>,
    pub selected_node_idx: Option<usize>,
    pub node_scroll: usize,
}

impl<'a> Widget for NodeListWidget<'a> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        let node_ids: Vec<&str> = self.nodes.keys().map(|s| s.as_str()).collect();
        let max_name = area.width.saturating_sub(2) as usize; // 2 = "► "

        for row in 0..area.height as usize {
            let node_row = self.node_scroll + row;
            let Some(&node_id) = node_ids.get(node_row) else {
                break;
            };
            let y = area.y + row as u16;
            let is_selected = self.selected_node_idx == Some(node_row);

            let short_id = if node_id.len() > max_name {
                let tail = max_name.saturating_sub(5);
                format!("{}…{}", &node_id[..4], &node_id[node_id.len() - tail..])
            } else {
                node_id.to_string()
            };

            let prefix = if is_selected { "► " } else { "  " };
            let label = format!("{}{:<width$}", prefix, short_id, width = max_name);

            let style = if is_selected {
                Style::default()
                    .fg(Color::Cyan)
                    .add_modifier(Modifier::BOLD)
            } else {
                Style::default().fg(Color::Gray)
            };

            buf.set_string(area.x, y, &label[..area.width as usize], style);
        }
    }
}
