use std::collections::BTreeMap;

use psyche_core::BatchId;
use psyche_event_sourcing::projection::{BatchDownload, ClusterBatchView, ClusterSnapshot};
use ratatui::{
    buffer::Buffer,
    layout::{Constraint, Rect},
    style::{Color, Modifier, Style, Stylize},
    text::{Line, Span},
    widgets::{Block, Borders, Cell, Paragraph, Row, Table, Widget},
};

pub struct BatchesWidget<'a> {
    pub snapshot: &'a ClusterSnapshot,
    /// Highlighted node — its rows are shown in a distinct colour.
    pub selected_node_id: Option<&'a str>,
    /// Vertical scroll offset (in data rows, not counting headers).
    pub scroll: usize,
}

// ── helpers ───────────────────────────────────────────────────────────────────

fn short_id(id: &str) -> String {
    if id.len() > 16 {
        format!("{}…{}", &id[..8], &id[id.len() - 6..])
    } else {
        id.to_string()
    }
}

fn fmt_range(batch_id: &BatchId) -> String {
    let s = batch_id.0.start;
    let e = batch_id.0.end;
    if s == e {
        format!("{s}")
    } else {
        format!("{s}..{e}")
    }
}

/// Returns (pct_complete, result) for the most-progressed download among all nodes.
fn best_download(
    batch: &ClusterBatchView,
    assigned: Option<&str>,
) -> Option<(f64, Option<Result<(), String>>)> {
    // Prefer the assigned node's download; fall back to any node.
    let dl: Option<&BatchDownload> = assigned
        .and_then(|id| batch.downloads.get(id))
        .or_else(|| batch.downloads.values().next());

    let dl = dl?;
    let pct = match dl.size_bytes {
        Some(sz) if sz > 0 => dl.bytes_downloaded as f64 / sz as f64,
        _ => {
            if dl.result.is_some() {
                1.0
            } else {
                0.0
            }
        }
    };
    Some((pct, dl.result.clone()))
}

fn download_cell(batch: &ClusterBatchView) -> (String, Color) {
    let assigned = batch.assigned_to.as_deref();
    match best_download(batch, assigned) {
        None => ("  —".to_string(), Color::DarkGray),
        Some((_, Some(Ok(())))) => ("  ✓".to_string(), Color::Green),
        Some((_, Some(Err(e)))) => {
            let msg = if e.is_empty() {
                "err".to_string()
            } else {
                e[..e.len().min(8)].to_string()
            };
            (format!("  ✗ {msg}"), Color::Red)
        }
        Some((pct, None)) => {
            let filled = (pct * 8.0).round() as usize;
            let bar: String = "█".repeat(filled) + &"░".repeat(8usize.saturating_sub(filled));
            (format!(" {bar} {:3.0}%", pct * 100.0), Color::Yellow)
        }
    }
}

fn trained_cell(batch: &ClusterBatchView) -> (String, Color) {
    if batch.trained_by.is_empty() {
        ("  —".to_string(), Color::DarkGray)
    } else {
        ("  ✓".to_string(), Color::Green)
    }
}

// ── section rendering ─────────────────────────────────────────────────────────

/// Build table rows from a batch map.  Returns (rows, total_data_row_count).
fn build_rows<'a>(
    batches: &'a BTreeMap<BatchId, ClusterBatchView>,
    selected_node_id: Option<&'a str>,
    scroll: usize,
    max_rows: usize,
) -> (Vec<Row<'a>>, usize) {
    let total = batches.len();
    let rows: Vec<Row> = batches
        .iter()
        .skip(scroll)
        .take(max_rows)
        .map(|(batch_id, batch)| {
            let assigned = batch.assigned_to.as_deref().unwrap_or("—");
            let is_mine =
                selected_node_id.is_some_and(|sel| batch.assigned_to.as_deref() == Some(sel));

            let range_cell = Cell::from(fmt_range(batch_id));
            let assigned_cell = Cell::from(short_id(assigned));

            let (dl_text, dl_color) = download_cell(batch);
            let download_cell_widget = Cell::from(dl_text).style(Style::default().fg(dl_color));

            let (tr_text, tr_color) = trained_cell(batch);
            let trained_cell_widget = Cell::from(tr_text).style(Style::default().fg(tr_color));

            let row = Row::new([
                range_cell,
                assigned_cell,
                download_cell_widget,
                trained_cell_widget,
            ]);
            if is_mine {
                row.style(Style::default().fg(Color::Yellow))
            } else {
                row
            }
        })
        .collect();
    (rows, total)
}

// ── widget ────────────────────────────────────────────────────────────────────

impl<'a> Widget for BatchesWidget<'a> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        let block = Block::default().borders(Borders::ALL);
        let inner = block.inner(area);
        block.render(area, buf);

        if inner.height < 3 {
            return;
        }

        let snap = self.snapshot;
        let has_prev = !snap.prev_step_batches.is_empty();
        let has_curr = !snap.step_batches.is_empty();

        if !has_prev && !has_curr {
            Paragraph::new(Span::styled(
                "No batch data for this step",
                Style::default()
                    .fg(Color::DarkGray)
                    .add_modifier(Modifier::ITALIC),
            ))
            .centered()
            .render(inner, buf);
            return;
        }

        let header_style = Style::default()
            .fg(Color::DarkGray)
            .add_modifier(Modifier::BOLD);
        let col_constraints = [
            Constraint::Length(18), // RANGE
            Constraint::Length(17), // ASSIGNED
            Constraint::Length(16), // DOWNLOAD
            Constraint::Length(6),  // TRAINED
        ];

        // We'll render prev section then current section stacked vertically.
        // Each section: 1-line section label + 1-line column header + N data rows.
        let mut y = inner.y;
        let bottom = inner.y + inner.height;

        let render_section = |label: &str,
                              batches: &BTreeMap<BatchId, ClusterBatchView>,
                              y: &mut u16,
                              buf: &mut Buffer| {
            if *y >= bottom {
                return;
            }
            // Section label line
            let label_area = Rect {
                x: inner.x,
                y: *y,
                width: inner.width,
                height: 1,
            };
            Paragraph::new(Line::from(vec![
                Span::styled(" ", Style::default()),
                Span::styled(label, Style::default().fg(Color::Cyan).bold()),
            ]))
            .render(label_area, buf);
            *y += 1;

            if *y >= bottom {
                return;
            }

            // Column header row
            let header_area = Rect {
                x: inner.x,
                y: *y,
                width: inner.width,
                height: 1,
            };
            let header = Row::new([
                Cell::from(" RANGE").style(header_style),
                Cell::from("ASSIGNED").style(header_style),
                Cell::from("DOWNLOAD").style(header_style),
                Cell::from("TRAIN").style(header_style),
            ]);
            let available_rows = (bottom - *y).saturating_sub(1) as usize;
            *y += 1;

            if *y > bottom {
                return;
            }

            let (data_rows, _total) =
                build_rows(batches, self.selected_node_id, self.scroll, available_rows);
            let actual_rows = data_rows.len() as u16;

            Table::new(data_rows, col_constraints)
                .header(header)
                .column_spacing(1)
                .render(
                    Rect {
                        x: inner.x,
                        y: header_area.y,
                        width: inner.width,
                        height: 1 + actual_rows,
                    },
                    buf,
                );

            *y += actual_rows;
        };

        // Render prev step first (if any), then current.
        let prev_step = snap
            .coordinator
            .as_ref()
            .map(|c| c.step.saturating_sub(1))
            .unwrap_or(0);
        let curr_step = snap.coordinator.as_ref().map(|c| c.step).unwrap_or(0);

        if has_prev {
            let label = format!("step {} (previous)", prev_step);
            render_section(&label, &snap.prev_step_batches, &mut y, buf);
            if y < bottom {
                y += 1; // blank separator line
            }
        }
        if has_curr {
            let label = format!("step {} (current)", curr_step);
            render_section(&label, &snap.step_batches, &mut y, buf);
        }
    }
}
