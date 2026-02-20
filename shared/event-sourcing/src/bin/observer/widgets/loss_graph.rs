use indexmap::IndexMap;
use psyche_event_sourcing::projection::NodeSnapshot;
use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Style, Stylize},
    symbols,
    text::{Line, Span},
    widgets::{Axis, Block, Borders, Chart, Dataset, GraphType, LegendPosition, Paragraph, Widget},
};

static NODE_COLORS: &[Color] = &[
    Color::Cyan,
    Color::Red,
    Color::Magenta,
    Color::Green,
    Color::Yellow,
    Color::Blue,
    Color::LightCyan,
    Color::LightRed,
    Color::LightMagenta,
    Color::LightGreen,
];

pub struct LossGraphWidget<'a> {
    pub nodes: &'a IndexMap<String, NodeSnapshot>,
}

impl<'a> Widget for LossGraphWidget<'a> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        let block = Block::default().title(" LOSS ").borders(Borders::ALL);
        let inner = block.inner(area);
        block.render(area, buf);

        // Per-node: (label, step-loss points, epoch boundary step values)
        // x = step, y = loss
        // Epoch boundaries = step values where the epoch number first changes.
        let node_data: Vec<(String, usize, Vec<(f64, f64)>, Vec<f64>)> = self
            .nodes
            .iter()
            .filter(|(_, n)| !n.epoch_losses.is_empty())
            .enumerate()
            .map(|(color_idx, (id, n))| {
                let short_id = if id.len() > 14 {
                    format!("{}…{}", &id[..6], &id[id.len() - 4..])
                } else {
                    id.clone()
                };

                let pts: Vec<(f64, f64)> = n
                    .epoch_losses
                    .iter()
                    .map(|&(_, step, loss)| (step as f64, loss))
                    .collect();

                // Epoch boundary x-positions: the step where the epoch number
                // changes (midpoint between last step of old epoch and first of new).
                let boundaries: Vec<f64> = n
                    .epoch_losses
                    .windows(2)
                    .filter_map(|w| {
                        let (e0, s0, _) = w[0];
                        let (e1, s1, _) = w[1];
                        if e0 != e1 {
                            Some((s0 as f64 + s1 as f64) / 2.0)
                        } else {
                            None
                        }
                    })
                    .collect();

                (short_id, color_idx, pts, boundaries)
            })
            .collect();

        if node_data.is_empty() {
            Paragraph::new(Line::from(Span::styled(
                "No loss data yet",
                Style::default().fg(Color::DarkGray),
            )))
            .centered()
            .render(inner, buf);
            return;
        }

        // Axis bounds across all nodes.
        let all_x: Vec<f64> = node_data
            .iter()
            .flat_map(|(_, _, pts, _)| pts.iter().map(|(x, _)| *x))
            .collect();
        let all_y: Vec<f64> = node_data
            .iter()
            .flat_map(|(_, _, pts, _)| pts.iter().map(|(_, y)| *y))
            .collect();

        let x_min = all_x.iter().copied().fold(f64::INFINITY, f64::min);
        let x_max = all_x.iter().copied().fold(f64::NEG_INFINITY, f64::max);
        let y_lo = all_y.iter().copied().fold(f64::INFINITY, f64::min);
        let y_hi = all_y.iter().copied().fold(f64::NEG_INFINITY, f64::max);

        let y_margin = ((y_hi - y_lo) * 0.1).max(0.05);
        let y_min = (y_lo - y_margin).max(0.0);
        let y_max = if (y_hi + y_margin - y_min).abs() < f64::EPSILON {
            y_min + 1.0
        } else {
            y_hi + y_margin
        };
        let (x_min, x_max) = if (x_max - x_min).abs() < f64::EPSILON {
            (x_min - 0.5, x_max + 0.5)
        } else {
            (x_min, x_max)
        };

        // Build per-node loss line datasets.
        let mut datasets: Vec<Dataset> = node_data
            .iter()
            .map(|(name, color_idx, pts, _)| {
                Dataset::default()
                    .name(name.as_str())
                    .marker(symbols::Marker::Braille)
                    .graph_type(GraphType::Line)
                    .style(Style::default().fg(NODE_COLORS[color_idx % NODE_COLORS.len()]))
                    .data(pts)
            })
            .collect();

        // Epoch boundary vertical lines.
        // Deduplicate across nodes, then emit one dataset per boundary.
        // Empty name → not shown in legend.
        let mut seen: std::collections::BTreeSet<i64> = std::collections::BTreeSet::new();
        let boundary_point_vecs: Vec<Vec<(f64, f64)>> = node_data
            .iter()
            .flat_map(|(_, _, _, boundaries)| boundaries.iter().copied())
            .filter_map(|bx| {
                let key = (bx * 10.0).round() as i64;
                if seen.insert(key) {
                    // 80 sample points → solid braille vertical line
                    let pts: Vec<(f64, f64)> = (0..=80)
                        .map(|i| (bx, y_min + (y_max - y_min) * (i as f64) / 80.0))
                        .collect();
                    Some(pts)
                } else {
                    None
                }
            })
            .collect();

        for pts in &boundary_point_vecs {
            datasets.push(
                Dataset::default()
                    .name("")
                    .marker(symbols::Marker::Braille)
                    .graph_type(GraphType::Line)
                    .style(Style::default().fg(Color::DarkGray))
                    .data(pts),
            );
        }

        // X-axis labels: "s{min}" on the left, epoch transition points in the
        // middle, "s{max}" on the right — taken from the first node's data.
        let x_labels: Vec<String> = {
            let mut labels = vec![format!("step {}", x_min as i64)];
            if let Some((_, _, _, boundaries)) = node_data.first() {
                if let Some((_, node)) = self.nodes.iter().find(|(_, n)| !n.epoch_losses.is_empty())
                {
                    for &bx in boundaries {
                        if let Some(&(_, step, _)) =
                            node.epoch_losses.iter().find(|&&(_, s, _)| s as f64 > bx)
                        {
                            labels.push(format!("{step}"));
                        }
                    }
                }
            }
            labels.push(format!("s{}", x_max as i64));
            labels
        };

        let x_axis = Axis::default()
            .bounds([x_min, x_max])
            .labels(x_labels)
            .style(Style::default().white());

        let y_axis = Axis::default()
            .bounds([y_min, y_max])
            .labels([format!("{y_min:.2}"), format!("{y_max:.2}")])
            .style(Style::default().white());

        Chart::new(datasets)
            .x_axis(x_axis)
            .y_axis(y_axis)
            .legend_position(Some(LegendPosition::TopRight))
            .render(inner, buf);
    }
}
