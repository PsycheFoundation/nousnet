use std::collections::BTreeMap;

use psyche_core::BatchId;
use psyche_event_sourcing::projection::{ClusterBatchView, ClusterSnapshot, NodeBatchStatus};
use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Modifier, Style, Stylize},
    text::{Line, Span},
    widgets::{Paragraph, Widget},
};

pub struct BatchesWidget<'a> {
    pub snapshot: &'a ClusterSnapshot,
    /// Highlighted node — its rows are shown in a distinct colour.
    pub selected_node_id: Option<&'a str>,
    /// Vertical scroll offset (in rendered lines).
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

fn fmt_batch_id(batch_id: &BatchId) -> String {
    let s = batch_id.0.start;
    let e = batch_id.0.end;
    if s == e {
        format!("{s}")
    } else {
        format!("{s}..{e}")
    }
}

/// Build a sorted list of all node IDs across the snapshot.
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

/// Render per-node dots for a batch row into a string. Wraps at `wrap_w`.
/// Returns lines of (char, Color) tuples.
fn node_dot_lines(
    batch: &ClusterBatchView,
    all_nodes: &[String],
    wrap_w: usize,
) -> Vec<Vec<(char, Color)>> {
    let trainer = batch.assigned_to.as_deref();
    let mut chars: Vec<(char, Color)> = Vec::new();

    for node_id in all_nodes {
        if trainer == Some(node_id.as_str()) {
            continue;
        }
        let status = batch.node_status.get(node_id.as_str());
        let (ch, color) = match status {
            Some(s) => status_char(s),
            None => ('·', Color::DarkGray),
        };
        chars.push((ch, color));
    }

    if chars.is_empty() || wrap_w == 0 {
        return vec![chars];
    }
    chars.chunks(wrap_w).map(|c| c.to_vec()).collect()
}

/// Render per-node applied dots. Wraps at `wrap_w`.
fn applied_dot_lines(
    batch: &ClusterBatchView,
    all_nodes: &[String],
    applied_by: &std::collections::HashSet<String>,
    wrap_w: usize,
) -> Vec<Vec<(char, Color)>> {
    let trainer = batch.assigned_to.as_deref();
    let mut chars: Vec<(char, Color)> = Vec::new();

    for node_id in all_nodes {
        if trainer == Some(node_id.as_str()) {
            continue;
        }
        let (ch, color) = if applied_by.contains(node_id.as_str()) {
            ('✓', Color::Green)
        } else {
            ('·', Color::DarkGray)
        };
        chars.push((ch, color));
    }

    if chars.is_empty() || wrap_w == 0 {
        return vec![chars];
    }
    chars.chunks(wrap_w).map(|c| c.to_vec()).collect()
}

// ── column layout ────────────────────────────────────────────────────────────

/// Fixed column widths: " BATCH_ID  NODE            DATA TRAIN "
/// BATCH_ID column is 16 wide to fit ranges like "161064..161067".
const COL_BATCH_ID: usize = 16;
const COL_NODE: usize = 15;
const COL_DATA: usize = 5;
const COL_TRAIN: usize = 6;
/// Total fixed prefix width including leading space and inter-column spaces.
const PREFIX_W: usize = 1 + COL_BATCH_ID + 1 + COL_NODE + 1 + COL_DATA + COL_TRAIN;

// ── section rendering ─────────────────────────────────────────────────────────

struct SectionCtx<'a> {
    all_nodes: &'a [String],
    selected_node_id: Option<&'a str>,
    applied_by: &'a std::collections::HashSet<String>,
}

/// Collect all rendered lines for one batch-table section (prev or current step).
/// Returns a Vec of Lines.
fn build_section_lines<'a>(
    label: &str,
    batches: &BTreeMap<BatchId, ClusterBatchView>,
    ctx: &SectionCtx<'a>,
    total_w: usize,
) -> Vec<Line<'a>> {
    if batches.is_empty() {
        return Vec::new();
    }

    let mut lines: Vec<Line> = Vec::new();

    // Section label.
    lines.push(Line::from(vec![
        Span::raw(" "),
        Span::styled(label.to_string(), Style::default().fg(Color::Cyan).bold()),
    ]));

    // Compute dot column widths.
    let remaining = total_w.saturating_sub(PREFIX_W);
    // Split remaining between NODES and APPLIED with a 1-char separator.
    let dot_w = if remaining > 1 {
        (remaining - 1) / 2
    } else {
        0
    };
    // Column header — align labels to center of their dot columns.
    let header_style = Style::default()
        .fg(Color::DarkGray)
        .add_modifier(Modifier::BOLD);

    let mut hdr = format!(
        " {:<w_bid$} {:<w_node$} {:<w_data$}{:<w_train$}",
        "BATCH ID",
        "NODE",
        "DATA",
        "TRAIN",
        w_bid = COL_BATCH_ID,
        w_node = COL_NODE,
        w_data = COL_DATA,
        w_train = COL_TRAIN,
    );
    // Center "NODES" within dot_w
    if dot_w > 0 {
        let label_nodes = "NODES";
        let pad_l = dot_w.saturating_sub(label_nodes.len()) / 2;
        let pad_r = dot_w
            .saturating_sub(label_nodes.len())
            .saturating_sub(pad_l);
        hdr.push_str(&" ".repeat(pad_l));
        hdr.push_str(label_nodes);
        hdr.push_str(&" ".repeat(pad_r));
    }
    hdr.push(' ');
    // Center "APPLIED" within dot_w
    if dot_w > 0 {
        let label_applied = "APPLIED";
        let pad_l = dot_w.saturating_sub(label_applied.len()) / 2;
        hdr.push_str(&" ".repeat(pad_l));
        hdr.push_str(label_applied);
    }

    let hdr_truncated: String = hdr.chars().take(total_w).collect();
    lines.push(Line::from(Span::styled(hdr_truncated, header_style)));

    // Data rows.
    for (batch_id, batch) in batches.iter() {
        let is_mine = ctx
            .selected_node_id
            .is_some_and(|sel| batch.assigned_to.as_deref() == Some(sel));
        let row_style = if is_mine {
            Style::default().fg(Color::Yellow)
        } else {
            Style::default()
        };

        let bid = fmt_batch_id(batch_id);
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

        let wrap_w = if dot_w > 0 { dot_w } else { 1 };
        let node_lines = node_dot_lines(batch, ctx.all_nodes, wrap_w);
        let app_lines = applied_dot_lines(batch, ctx.all_nodes, ctx.applied_by, wrap_w);
        let num_rows = node_lines.len().max(app_lines.len()).max(1);

        for row_i in 0..num_rows {
            let mut spans: Vec<Span> = Vec::new();

            if row_i == 0 {
                // First line: show the fixed columns.
                spans.push(Span::styled(
                    format!(" {:<w$} ", bid, w = COL_BATCH_ID),
                    row_style,
                ));
                spans.push(Span::styled(
                    format!("{:<w$} ", assigned, w = COL_NODE),
                    row_style,
                ));
                spans.push(Span::styled(
                    format!(" {data_ch}   "),
                    Style::default().fg(data_color),
                ));
                spans.push(Span::styled(
                    format!(" {train_ch}   "),
                    Style::default().fg(train_color),
                ));
            } else {
                // Continuation line: pad the fixed columns.
                spans.push(Span::raw(" ".repeat(PREFIX_W)));
            }

            // Node dots for this wrap line.
            if let Some(dots) = node_lines.get(row_i) {
                for &(ch, color) in dots {
                    spans.push(Span::styled(ch.to_string(), Style::default().fg(color)));
                }
                // Pad to dot_w.
                let used = dots.len();
                if used < dot_w {
                    spans.push(Span::raw(" ".repeat(dot_w - used)));
                }
            } else {
                spans.push(Span::raw(" ".repeat(dot_w)));
            }

            spans.push(Span::raw(" "));

            // Applied dots for this wrap line.
            if let Some(dots) = app_lines.get(row_i) {
                for &(ch, color) in dots {
                    spans.push(Span::styled(ch.to_string(), Style::default().fg(color)));
                }
            }

            lines.push(Line::from(spans));
        }
    }

    lines
}

fn build_witness_lines<'a>(snapshot: &'a ClusterSnapshot) -> Vec<Line<'a>> {
    if snapshot.step_witnesses.is_empty() {
        return Vec::new();
    }

    let mut lines: Vec<Line> = Vec::new();
    let step = snapshot.coordinator.as_ref().map(|c| c.step).unwrap_or(0);
    let label = format!("Witnesses (step {})", step);

    lines.push(Line::from("")); // blank separator
    lines.push(Line::from(vec![
        Span::raw(" "),
        Span::styled(label, Style::default().fg(Color::Magenta).bold()),
    ]));

    for (node_id, ws) in &snapshot.step_witnesses {
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

        lines.push(Line::from(vec![
            Span::raw(format!("  {:<15} ", id)),
            Span::styled(format!("{sub_ch}"), Style::default().fg(sub_color)),
            Span::raw(" submitted  "),
            Span::styled(rpc_label.to_string(), Style::default().fg(rpc_color)),
        ]));
    }

    lines
}

fn build_legend_lines<'a>() -> Vec<Line<'a>> {
    let dim = Style::default().fg(Color::DarkGray);
    vec![
        Line::from(""),
        Line::from(vec![
            Span::styled(" NODES: ", dim.add_modifier(Modifier::BOLD)),
            Span::styled("·", Style::default().fg(Color::DarkGray)),
            Span::styled(" waiting ", dim),
            Span::styled("○", Style::default().fg(Color::Yellow)),
            Span::styled(" gossip ", dim),
            Span::styled("↓", Style::default().fg(Color::Cyan)),
            Span::styled(" downloading ", dim),
            Span::styled("◉", Style::default().fg(Color::Cyan)),
            Span::styled(" downloaded ", dim),
            Span::styled("●", Style::default().fg(Color::Green)),
            Span::styled(" ready ", dim),
            Span::styled("✗", Style::default().fg(Color::Red)),
            Span::styled(" failed", dim),
        ]),
        Line::from(vec![
            Span::styled(" APPLIED: ", dim.add_modifier(Modifier::BOLD)),
            Span::styled("·", Style::default().fg(Color::DarkGray)),
            Span::styled(" pending ", dim),
            Span::styled("✓", Style::default().fg(Color::Green)),
            Span::styled(" applied ", dim),
        ]),
    ]
}

// ── widget ────────────────────────────────────────────────────────────────────

impl<'a> Widget for BatchesWidget<'a> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        if area.height < 3 || area.width < 10 {
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
            .render(area, buf);
            return;
        }

        let w = area.width as usize;
        let all_nodes = all_node_ids(snap);

        let prev_step = snap
            .coordinator
            .as_ref()
            .map(|c| c.step.saturating_sub(1))
            .unwrap_or(0);
        let curr_step = snap.coordinator.as_ref().map(|c| c.step).unwrap_or(0);

        // Build all lines, then scroll and render.
        let mut all_lines: Vec<Line> = Vec::new();

        let ctx = SectionCtx {
            all_nodes: &all_nodes,
            selected_node_id: self.selected_node_id,
            applied_by: &snap.prev_applied_by,
        };

        if has_prev {
            all_lines.extend(build_section_lines(
                &format!("step {} (previous)", prev_step),
                &snap.prev_step_batches,
                &ctx,
                w,
            ));
            all_lines.push(Line::from("")); // blank separator
        }
        if has_curr {
            let ctx_curr = SectionCtx {
                all_nodes: &all_nodes,
                selected_node_id: self.selected_node_id,
                applied_by: &snap.applied_by,
            };
            all_lines.extend(build_section_lines(
                &format!("step {} (current)", curr_step),
                &snap.step_batches,
                &ctx_curr,
                w,
            ));
        }

        all_lines.extend(build_witness_lines(snap));
        all_lines.extend(build_legend_lines());

        // Apply scroll and render visible lines.
        let visible_h = area.height as usize;
        let visible: Vec<Line> = all_lines
            .into_iter()
            .skip(self.scroll)
            .take(visible_h)
            .collect();

        for (i, line) in visible.iter().enumerate() {
            let line_y = area.y + i as u16;
            if line_y >= area.y + area.height {
                break;
            }
            let r = Rect {
                x: area.x,
                y: line_y,
                width: area.width,
                height: 1,
            };
            Paragraph::new(line.clone()).render(r, buf);
        }
    }
}
