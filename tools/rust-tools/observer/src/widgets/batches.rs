use std::collections::BTreeMap;

use psyche_core::BatchId;
use psyche_event_sourcing::projection::{ClusterBatchView, ClusterSnapshot, NodeBatchStatus};
use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Modifier, Style, Stylize},
    text::{Line, Span},
    widgets::{Block, Borders, Paragraph, Widget},
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
    if id.len() > 14 {
        format!("{}…{}", &id[..7], &id[id.len() - 5..])
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

/// Build a sorted list of all non-trainer node IDs across the snapshot.
/// The order is deterministic (alphabetical) so dots line up across rows.
fn all_node_ids(snapshot: &ClusterSnapshot) -> Vec<String> {
    let mut ids: Vec<String> = snapshot.nodes.keys().cloned().collect();
    ids.sort();
    ids
}

// ── status characters ─────────────────────────────────────────────────────────

fn status_char(status: &NodeBatchStatus) -> (char, Color) {
    // Check for failures first.
    if status.deserialized == Some(false) || status.download == Some(Some(false)) {
        return ('✗', Color::Red);
    }
    // Fully deserialized.
    if status.deserialized == Some(true) {
        return ('●', Color::Green);
    }
    // Downloaded but not yet deserialized.
    if status.download == Some(Some(true)) {
        return ('◉', Color::Cyan);
    }
    // Downloading in progress.
    if status.download == Some(None) {
        return ('↓', Color::Cyan);
    }
    // Gossip received but no download yet.
    if status.gossip_received {
        return ('○', Color::Yellow);
    }
    // Nothing yet.
    ('·', Color::DarkGray)
}

fn check_char(ok: Option<bool>) -> (char, Color) {
    match ok {
        None => ('—', Color::DarkGray),
        Some(true) => ('✓', Color::Green),
        Some(false) => ('✗', Color::Red),
    }
}

// ── per-row dot rendering ────────────────────────────────────────────────────

const MAX_DOT_WIDTH: usize = 24;

/// Render per-node dots for a batch row. Returns spans fitting within `max_w`.
fn node_dots<'a>(batch: &ClusterBatchView, all_nodes: &[String], max_w: usize) -> Vec<Span<'a>> {
    let trainer = batch.assigned_to.as_deref();
    let mut spans = Vec::new();
    let mut count = 0;
    let limit = max_w.min(MAX_DOT_WIDTH);

    for node_id in all_nodes {
        // Skip the trainer — they don't download their own result.
        if trainer == Some(node_id.as_str()) {
            continue;
        }
        if count >= limit {
            spans.push(Span::styled("…", Style::default().fg(Color::DarkGray)));
            break;
        }
        let status = batch.node_status.get(node_id.as_str());
        let (ch, color) = match status {
            Some(s) => status_char(s),
            None => ('·', Color::DarkGray),
        };
        spans.push(Span::styled(ch.to_string(), Style::default().fg(color)));
        count += 1;
    }
    spans
}

/// Render per-node applied dots.
fn applied_dots<'a>(
    batch: &ClusterBatchView,
    all_nodes: &[String],
    applied_by: &std::collections::HashSet<String>,
    max_w: usize,
) -> Vec<Span<'a>> {
    let trainer = batch.assigned_to.as_deref();
    let mut spans = Vec::new();
    let mut count = 0;
    let limit = max_w.min(MAX_DOT_WIDTH);

    for node_id in all_nodes {
        if trainer == Some(node_id.as_str()) {
            continue;
        }
        if count >= limit {
            spans.push(Span::styled("…", Style::default().fg(Color::DarkGray)));
            break;
        }
        let (ch, color) = if applied_by.contains(node_id.as_str()) {
            ('✓', Color::Green)
        } else {
            ('·', Color::DarkGray)
        };
        spans.push(Span::styled(ch.to_string(), Style::default().fg(color)));
        count += 1;
    }
    spans
}

// ── section rendering ─────────────────────────────────────────────────────────

struct SectionCtx<'a> {
    all_nodes: &'a [String],
    selected_node_id: Option<&'a str>,
    applied_by: &'a std::collections::HashSet<String>,
    inner: Rect,
    scroll: usize,
}

/// Render one batch-table section (prev or current step). Returns rows consumed.
fn render_section(
    label: &str,
    batches: &BTreeMap<BatchId, ClusterBatchView>,
    ctx: &SectionCtx<'_>,
    y: &mut u16,
    buf: &mut Buffer,
) {
    let bottom = ctx.inner.y + ctx.inner.height;
    if *y >= bottom || batches.is_empty() {
        return;
    }

    let x = ctx.inner.x;
    let w = ctx.inner.width as usize;

    // Section label.
    let label_area = Rect {
        x,
        y: *y,
        width: ctx.inner.width,
        height: 1,
    };
    Paragraph::new(Line::from(vec![
        Span::raw(" "),
        Span::styled(label, Style::default().fg(Color::Cyan).bold()),
    ]))
    .render(label_area, buf);
    *y += 1;
    if *y >= bottom {
        return;
    }

    // Column header.
    // Layout: RANGE(12) NODE(15) DATA(5) TRAIN(6) NODES(...) APPLIED(...)
    let header_style = Style::default()
        .fg(Color::DarkGray)
        .add_modifier(Modifier::BOLD);
    let hdr = " RANGE        NODE            DATA TRAIN NODES                    APPLIED";
    let hdr_truncated: String = hdr.chars().take(w).collect();
    buf.set_string(x, *y, &hdr_truncated, header_style);
    *y += 1;
    if *y >= bottom {
        return;
    }

    // Data rows.
    let available = (bottom - *y) as usize;
    for (batch_id, batch) in batches.iter().skip(ctx.scroll).take(available) {
        if *y >= bottom {
            break;
        }

        let is_mine = ctx
            .selected_node_id
            .is_some_and(|sel| batch.assigned_to.as_deref() == Some(sel));
        let row_style = if is_mine {
            Style::default().fg(Color::Yellow)
        } else {
            Style::default()
        };

        let range = fmt_range(batch_id);
        let assigned = batch
            .assigned_to
            .as_deref()
            .map(short_id)
            .unwrap_or_else(|| "—".to_string());

        let (data_ch, data_color) = check_char(batch.data_downloaded);
        let (train_ch, train_color) = if batch.trained {
            ('✓', Color::Green)
        } else {
            ('—', Color::DarkGray)
        };

        // Build the line as spans.
        let mut spans: Vec<Span> = Vec::new();
        spans.push(Span::styled(format!(" {:<12} ", range), row_style));
        spans.push(Span::styled(format!("{:<15} ", assigned), row_style));
        spans.push(Span::styled(
            format!(" {data_ch}  "),
            Style::default().fg(data_color),
        ));
        spans.push(Span::styled(
            format!("  {train_ch}   "),
            Style::default().fg(train_color),
        ));

        // Compute available width for dot columns.
        // Fixed prefix width: 1 + 12 + 1 + 15 + 1 + 4 + 6 = ~40 chars.
        let prefix_w = 40;
        let remaining = w.saturating_sub(prefix_w);
        let dot_w = remaining / 2;

        spans.extend(node_dots(batch, ctx.all_nodes, dot_w));

        // Pad to align APPLIED column.
        let current_w: usize = spans.iter().map(|s| s.content.chars().count()).sum();
        let applied_col = prefix_w + dot_w + 1;
        if current_w < applied_col {
            spans.push(Span::raw(" ".repeat(applied_col - current_w)));
        }

        spans.extend(applied_dots(batch, ctx.all_nodes, ctx.applied_by, dot_w));

        Paragraph::new(Line::from(spans)).render(
            Rect {
                x,
                y: *y,
                width: ctx.inner.width,
                height: 1,
            },
            buf,
        );
        *y += 1;
    }
}

fn render_witnesses(snapshot: &ClusterSnapshot, y: &mut u16, inner: Rect, buf: &mut Buffer) {
    let bottom = inner.y + inner.height;
    if snapshot.step_witnesses.is_empty() || *y >= bottom {
        return;
    }

    // Blank separator.
    *y += 1;
    if *y >= bottom {
        return;
    }

    let step = snapshot.coordinator.as_ref().map(|c| c.step).unwrap_or(0);
    let label = format!("Witnesses (step {})", step);
    buf.set_string(
        inner.x + 1,
        *y,
        &label,
        Style::default().fg(Color::Magenta).bold(),
    );
    *y += 1;

    for (node_id, ws) in &snapshot.step_witnesses {
        if *y >= bottom {
            break;
        }
        let id = short_id(node_id);
        let (sub_ch, sub_color) = if ws.submitted {
            ('✓', Color::Green)
        } else {
            ('⋯', Color::Yellow)
        };
        let (rpc_label, rpc_color) = match ws.rpc_result {
            None if ws.submitted => ("pending", Color::Yellow),
            None => ("—", Color::DarkGray),
            Some(true) => ("accepted", Color::Green),
            Some(false) => ("rejected", Color::Red),
        };

        let line = Line::from(vec![
            Span::raw(format!("  {:<15} ", id)),
            Span::styled(format!("{sub_ch}"), Style::default().fg(sub_color)),
            Span::raw(" submitted  "),
            Span::styled(rpc_label, Style::default().fg(rpc_color)),
        ]);
        Paragraph::new(line).render(
            Rect {
                x: inner.x,
                y: *y,
                width: inner.width,
                height: 1,
            },
            buf,
        );
        *y += 1;
    }
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

        let all_nodes = all_node_ids(snap);
        let mut y = inner.y;

        let prev_step = snap
            .coordinator
            .as_ref()
            .map(|c| c.step.saturating_sub(1))
            .unwrap_or(0);
        let curr_step = snap.coordinator.as_ref().map(|c| c.step).unwrap_or(0);

        if has_prev {
            let label = format!("step {} (previous)", prev_step);
            let ctx = SectionCtx {
                all_nodes: &all_nodes,
                selected_node_id: self.selected_node_id,
                applied_by: &snap.prev_applied_by,
                inner,
                scroll: self.scroll,
            };
            render_section(&label, &snap.prev_step_batches, &ctx, &mut y, buf);
            if y < inner.y + inner.height {
                y += 1; // blank separator
            }
        }
        if has_curr {
            let label = format!("step {} (current)", curr_step);
            let ctx = SectionCtx {
                all_nodes: &all_nodes,
                selected_node_id: self.selected_node_id,
                applied_by: &snap.applied_by,
                inner,
                scroll: if has_prev { 0 } else { self.scroll },
            };
            render_section(&label, &snap.step_batches, &ctx, &mut y, buf);
        }

        // Witness section at the bottom.
        render_witnesses(snap, &mut y, inner, buf);
    }
}
