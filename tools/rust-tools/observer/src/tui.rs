use std::time::{Duration, Instant};
use std::{char, io};

use crossterm::{
    event::{self, Event, KeyCode, KeyModifiers},
    execute,
    terminal::{EnterAlternateScreen, LeaveAlternateScreen, disable_raw_mode, enable_raw_mode},
};
use ratatui::{
    Terminal,
    backend::CrosstermBackend,
    layout::{Constraint, Direction, Layout},
    style::{Color, Style, Stylize},
    symbols,
    widgets::{Block, Borders, Tabs, Widget},
};
use strum::IntoEnumIterator;

use crate::app::{App, DetailPanel, EventCategory};
use crate::widgets::{
    batches::BatchesWidget,
    coordinator_bar::CoordinatorBarWidget,
    event_detail::EventDetailWidget,
    loss_graph::LossGraphWidget,
    node::NodeWidget,
    scrubber::ScrubberWidget,
    waterfall::{ALL_CATEGORIES, WaterfallWidget},
};

pub fn run(mut app: App) -> io::Result<()> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let tick_rate = Duration::from_millis(200);
    let mut last_tick = Instant::now();
    let mut timeline_h: usize = 20;

    loop {
        app.ensure_node_visible(timeline_h);
        app.ensure_cursor_visible();

        {
            let snapshot = app.current_snapshot().clone();
            let cursor = app.cursor;
            let selected = app.selected_node_idx;
            let node_scroll = app.node_scroll;
            let file_stats = app.node_file_stats.clone();
            let detail_panel = app.detail_panel;
            let timeline = &app.timeline;
            let waterfall_zoom = app.waterfall_zoom;
            let waterfall_x_scroll = app.waterfall_x_scroll;
            let waterfall_filter = &app.waterfall_filter;
            let all_node_ids = timeline.all_entity_ids();
            let selected_node_id: Option<String> =
                selected.and_then(|i| all_node_ids.get(i).cloned());

            terminal.draw(|f| {
                let area = f.area();

                // Compute timeline height: at most 1/3 of total area, min 4 rows.
                let total_h = area.height;
                let max_timeline_h = (total_h / 3).max(4);
                // Actual rows needed: 1 ruler + 1 "all info" + node count + 2 scroll indicators.
                let node_count = all_node_ids.len() as u16;
                let desired_timeline_h = (1 + 1 + node_count + 2).min(max_timeline_h).max(4);

                let outer = Layout::default()
                    .direction(Direction::Vertical)
                    .constraints([
                        Constraint::Length(3),                  // scrubber
                        Constraint::Length(1),                  // separator ═══
                        Constraint::Length(2),                  // coordinator bar
                        Constraint::Length(desired_timeline_h), // timeline (always visible)
                        Constraint::Min(4),                     // detail panel
                    ])
                    .split(area);

                let scrubber_area = outer[0];
                let sep_area = outer[1];
                let coord_area = outer[2];
                let timeline_area = outer[3];
                let detail_area = outer[4];

                // Store the usable timeline row count (minus ruler and scroll indicator rows).
                timeline_h = timeline_area.height.saturating_sub(3) as usize;

                // ── Scrubber ──────────────────────────────────────────────────
                f.render_widget(ScrubberWidget { timeline, cursor }, scrubber_area);

                // ── Separator / category filter picker ════════════════════════
                {
                    // Fill with ═ as background.
                    for x in sep_area.x..sep_area.x + sep_area.width {
                        f.buffer_mut().set_string(
                            x,
                            sep_area.y,
                            "═",
                            Style::default().fg(Color::DarkGray),
                        );
                    }
                    // Overlay the category filter picker, right-aligned.
                    let picker_spans: Vec<ratatui::text::Span> = ALL_CATEGORIES
                        .iter()
                        .flat_map(|&cat| {
                            let active = waterfall_filter.contains(&cat);
                            let base = if active {
                                Style::default()
                                    .fg(Color::Black)
                                    .bg(cat.color())
                                    .add_modifier(ratatui::style::Modifier::BOLD)
                            } else {
                                Style::default().fg(cat.color())
                            };
                            let key_style = base.add_modifier(ratatui::style::Modifier::BOLD);

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
                    let picker_w: u16 = ALL_CATEGORIES
                        .iter()
                        .map(|c| c.label().len() + 5)
                        .sum::<usize>() as u16;
                    if picker_w <= sep_area.width {
                        let px = sep_area.x + sep_area.width.saturating_sub(picker_w);
                        ratatui::widgets::Paragraph::new(ratatui::text::Line::from(picker_spans))
                            .render(
                                ratatui::layout::Rect {
                                    x: px,
                                    y: sep_area.y,
                                    width: picker_w,
                                    height: 1,
                                },
                                f.buffer_mut(),
                            );
                    }
                }

                // ── Coordinator bar ───────────────────────────────────────────
                f.render_widget(
                    CoordinatorBarWidget {
                        snapshot: &snapshot,
                    },
                    coord_area,
                );

                // ── Timeline / Waterfall (always visible, not selectable) ────
                f.render_widget(
                    WaterfallWidget {
                        timeline,
                        cursor,
                        node_ids: all_node_ids,
                        selected_node_idx: selected,
                        node_scroll,
                        zoom: waterfall_zoom,
                        x_scroll: waterfall_x_scroll,
                        filter: waterfall_filter,
                    },
                    timeline_area,
                );

                // ── Detail panel (tabbed, selectable) ─────────────────────────
                let detail_border_color = Color::Cyan;
                let block = Block::default()
                    .borders(Borders::ALL)
                    .border_style(Style::default().fg(detail_border_color));
                let inner = block.inner(detail_area);
                f.render_widget(block, detail_area);

                let snapshot_node_idx = selected_node_id
                    .as_deref()
                    .and_then(|id| snapshot.nodes.get_index_of(id));

                match detail_panel {
                    DetailPanel::Node => f.render_widget(
                        NodeWidget {
                            snapshot: &snapshot,
                            selected_node_idx: snapshot_node_idx,
                            file_stats: &file_stats,
                        },
                        inner,
                    ),
                    DetailPanel::Loss => f.render_widget(
                        LossGraphWidget {
                            nodes: &snapshot.nodes,
                        },
                        inner,
                    ),
                    DetailPanel::Batches => f.render_widget(
                        BatchesWidget {
                            snapshot: &snapshot,
                            selected_node_id: selected_node_id.as_deref(),
                        },
                        inner,
                    ),
                    DetailPanel::Event => f.render_widget(
                        EventDetailWidget {
                            timeline,
                            cursor,
                            selected_node_id: selected_node_id.as_deref(),
                            filter: waterfall_filter,
                        },
                        inner,
                    ),
                }

                // Tabs overlay on detail panel top border
                let tab_idx = DetailPanel::iter().position(|p| p == detail_panel).unwrap();
                f.render_widget(
                    Tabs::new(
                        DetailPanel::iter()
                            .enumerate()
                            .map(|(i, p)| format!("[{i+1}] {}", p.to_string().to_uppercase())),
                    )
                    .select(tab_idx)
                    .style(Style::default().fg(Color::DarkGray))
                    .highlight_style(Style::default().yellow().bold())
                    .divider(symbols::DOT)
                    .padding(" ", " "),
                    ratatui::layout::Rect {
                        x: detail_area.x + 1,
                        y: detail_area.y,
                        width: detail_area.width.saturating_sub(2),
                        height: 1,
                    },
                );
            })?;
        }

        // ── Input ─────────────────────────────────────────────────────────────
        let timeout = tick_rate
            .checked_sub(last_tick.elapsed())
            .unwrap_or(Duration::ZERO);

        if event::poll(timeout)?
            && let Event::Key(key) = event::read()?
        {
            // Category filter keys are always active (timeline is always visible).
            let cat = match key.code {
                KeyCode::Char('e') => Some(EventCategory::Error),
                KeyCode::Char('t') => Some(EventCategory::Train),
                KeyCode::Char('w') => Some(EventCategory::Warmup),
                KeyCode::Char('c') => Some(EventCategory::Cooldown),
                KeyCode::Char('p') => Some(EventCategory::P2P),
                KeyCode::Char('l') => Some(EventCategory::Client),
                KeyCode::Char('o') => Some(EventCategory::Coordinator),
                _ => None,
            };
            if let Some(cat) = cat {
                app.toggle_category_filter(cat);
                continue;
            }

            match (key.code, key.modifiers) {
                (KeyCode::Char('q'), _) => break,

                // ── Playback speed (Shift+1/2/3) ─────────────────────────────
                (KeyCode::Char('!'), _) => app.set_speed(1),
                (KeyCode::Char('@'), _) => app.set_speed(5),
                (KeyCode::Char('#'), _) => app.set_speed(20),

                // ── Scrub ────────────────────────────────────────────────────
                (KeyCode::Char('g') | KeyCode::Home, KeyModifiers::NONE) => app.go_first(),
                (KeyCode::Char('G') | KeyCode::End, _)
                | (KeyCode::Char('g'), KeyModifiers::SHIFT) => app.go_last(),
                (KeyCode::Left, KeyModifiers::SHIFT) => app.step_backward(50),
                (KeyCode::Right, KeyModifiers::SHIFT) => app.step_forward(50),
                (KeyCode::Left, _) => app.step_backward(1),
                (KeyCode::Right, _) => app.step_forward(1),

                // ── ↑/↓ always navigate nodes ────────────────────────────────
                (KeyCode::Up, _) => app.prev_node(),
                (KeyCode::Down, _) => app.next_node(),

                // ── Tab still cycles panels ───────────────────────────────────
                (KeyCode::Tab, _) => app.cycle_detail_panel(),

                // ── Playback / zoom ───────────────────────────────────────────
                (KeyCode::Char(' '), _) => app.toggle_play(),
                (KeyCode::Char('['), _) => app.zoom_in(),
                (KeyCode::Char(']'), _) => app.zoom_out(),

                // ── Box focus (numbers) ──────────────────────────────────────
                (KeyCode::Char(number), KeyModifiers::NONE) => {
                    if let Some(number) = number.to_digit(10)
                        && let Some((_, panel)) = DetailPanel::iter()
                            .enumerate()
                            .find(|(i, p)| i == number.into())
                    {
                        app.switch_panel(panel)
                    }
                }

                _ => {}
            }
        }

        if last_tick.elapsed() >= tick_rate {
            app.tick();
            last_tick = Instant::now();
        }
    }

    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen)?;
    terminal.show_cursor()?;
    Ok(())
}
