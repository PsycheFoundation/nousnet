use std::io;
use std::time::{Duration, Instant};

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
    widgets::{Block, Borders, Tabs},
};

use crate::app::{App, DetailPanel, EventCategory, FocusedBox};
use crate::widgets::{
    batches::BatchesWidget, coordinator_bar::CoordinatorBarWidget, loss_graph::LossGraphWidget,
    node::NodeWidget, node_list::NodeListWidget, scrubber::ScrubberWidget,
    waterfall::WaterfallWidget,
};

/// Fixed width of the always-visible node-selector column.
const NODE_LIST_WIDTH: u16 = 18;

pub fn run(mut app: App) -> io::Result<()> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let tick_rate = Duration::from_millis(200);
    let mut last_tick = Instant::now();
    let mut node_list_h: usize = 20;

    loop {
        app.ensure_node_visible(node_list_h);
        app.ensure_cursor_visible();

        {
            let snapshot = app.current_snapshot().clone();
            let cursor = app.cursor;
            let selected = app.selected_node_idx;
            let node_scroll = app.node_scroll;
            let file_stats = app.node_file_stats.clone();
            let detail_panel = app.detail_panel;
            let focused_box = app.focused_box;
            let batch_scroll = app.batch_scroll;
            let timeline = &app.timeline;
            let waterfall_zoom = app.waterfall_zoom;
            let waterfall_x_scroll = app.waterfall_x_scroll;
            let waterfall_filter = &app.waterfall_filter;
            let all_node_ids = timeline.all_entity_ids();
            let selected_node_id: Option<String> =
                selected.and_then(|i| all_node_ids.get(i).cloned());

            terminal.draw(|f| {
                let area = f.area();
                let outer = outer_layout(area);

                // ── Scrubber ──────────────────────────────────────────────────
                f.render_widget(ScrubberWidget { timeline, cursor }, outer[0]);

                // ── Separator ════════════════════════════════════════════════
                let sep = outer[1];
                for x in sep.x..sep.x + sep.width {
                    f.buffer_mut()
                        .set_string(x, sep.y, "═", Style::default().fg(Color::DarkGray));
                }

                // ── Coordinator bar ───────────────────────────────────────────
                f.render_widget(
                    CoordinatorBarWidget {
                        snapshot: &snapshot,
                    },
                    outer[2],
                );

                // ── Main: node list (left) + tabbed panel (right) ─────────────
                let main = outer[3];
                node_list_h = main.height as usize;

                let cols = Layout::default()
                    .direction(Direction::Horizontal)
                    .constraints([Constraint::Length(NODE_LIST_WIDTH), Constraint::Min(10)])
                    .split(main);

                let list_area = cols[0];
                let right_area = cols[1];

                // Node list — highlight border when focused
                let list_border_color = if focused_box == FocusedBox::NodeList {
                    Color::Cyan
                } else {
                    Color::DarkGray
                };
                f.render_widget(
                    NodeListWidget {
                        node_ids: &all_node_ids,
                        selected_node_idx: selected,
                        node_scroll,
                        focused: focused_box == FocusedBox::NodeList,
                    },
                    list_area,
                );
                // Override the node list border color (drawn by NodeListWidget).
                // Draw a 1-char right-side border between list and right panel.
                for y in list_area.y..list_area.y + list_area.height {
                    f.buffer_mut().set_string(
                        list_area.x + list_area.width.saturating_sub(1),
                        y,
                        "│",
                        Style::default().fg(list_border_color),
                    );
                }

                // Right panel border — highlight when detail panel is focused
                let right_focused = matches!(
                    focused_box,
                    FocusedBox::Timeline
                        | FocusedBox::Node
                        | FocusedBox::Loss
                        | FocusedBox::Batches
                );
                let right_border_color = if right_focused {
                    Color::Cyan
                } else {
                    Color::DarkGray
                };
                let block = Block::default()
                    .borders(Borders::ALL)
                    .border_style(Style::default().fg(right_border_color));
                let inner = block.inner(right_area);
                f.render_widget(block, right_area);

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
                            scroll: batch_scroll,
                        },
                        inner,
                    ),
                    DetailPanel::Timeline => f.render_widget(
                        WaterfallWidget {
                            timeline,
                            cursor,
                            node_ids: &all_node_ids,
                            selected_node_idx: selected,
                            node_scroll,
                            zoom: waterfall_zoom,
                            x_scroll: waterfall_x_scroll,
                            filter: &waterfall_filter,
                        },
                        inner,
                    ),
                }

                // Tabs overlay on right panel top border
                let tab_idx = match detail_panel {
                    DetailPanel::Timeline => 0,
                    DetailPanel::Node => 1,
                    DetailPanel::Loss => 2,
                    DetailPanel::Batches => 3,
                };
                f.render_widget(
                    Tabs::new(vec!["[3] TIMELINE", "[4] NODE", "[5] LOSS", "[6] BATCHES"])
                        .select(tab_idx)
                        .style(Style::default().fg(Color::DarkGray))
                        .highlight_style(Style::default().yellow().bold())
                        .divider(symbols::DOT)
                        .padding(" ", " "),
                    ratatui::layout::Rect {
                        x: right_area.x + 1,
                        y: right_area.y,
                        width: right_area.width.saturating_sub(2),
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
            // Category filter keys only active when timeline is focused.
            let timeline_focused = app.focused_box == FocusedBox::Timeline;

            if timeline_focused {
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
            }

            match (key.code, key.modifiers) {
                (KeyCode::Char('q'), _) => break,

                // ── Box focus (numbers) ──────────────────────────────────────
                (KeyCode::Char('1'), KeyModifiers::NONE) => app.focus_box(FocusedBox::Scrubber),
                (KeyCode::Char('2'), KeyModifiers::NONE) => app.focus_box(FocusedBox::NodeList),
                (KeyCode::Char('3'), KeyModifiers::NONE) => app.focus_box(FocusedBox::Timeline),
                (KeyCode::Char('4'), KeyModifiers::NONE) => app.focus_box(FocusedBox::Node),
                (KeyCode::Char('5'), KeyModifiers::NONE) => app.focus_box(FocusedBox::Loss),
                (KeyCode::Char('6'), KeyModifiers::NONE) => app.focus_box(FocusedBox::Batches),

                // ── Playback speed (Shift+1/2/3) ─────────────────────────────
                (KeyCode::Char('!'), _) => app.set_speed(1),
                (KeyCode::Char('@'), _) => app.set_speed(5),
                (KeyCode::Char('#'), _) => app.set_speed(20),

                // ── Scrub ────────────────────────────────────────────────────
                (KeyCode::Char('g'), KeyModifiers::NONE) => app.go_first(),
                (KeyCode::Char('G'), _) | (KeyCode::Char('g'), KeyModifiers::SHIFT) => {
                    app.go_last()
                }
                (KeyCode::Left, KeyModifiers::SHIFT) => app.step_backward(50),
                (KeyCode::Right, KeyModifiers::SHIFT) => app.step_forward(50),
                (KeyCode::Left, _) => app.step_backward(1),
                (KeyCode::Right, _) => app.step_forward(1),

                // ── ↑/↓ routed by focused box ────────────────────────────────
                (KeyCode::Up, _) if app.focused_box == FocusedBox::Batches => {
                    app.scroll_batches_up()
                }
                (KeyCode::Down, _) if app.focused_box == FocusedBox::Batches => {
                    app.scroll_batches_down()
                }
                (KeyCode::Up, _) => app.prev_node(),
                (KeyCode::Down, _) => app.next_node(),

                // ── Tab still cycles panels ───────────────────────────────────
                (KeyCode::Tab, _) => app.cycle_detail_panel(),

                // ── Playback / zoom ───────────────────────────────────────────
                (KeyCode::Char(' '), _) => app.toggle_play(),
                (KeyCode::Char('['), _) => app.zoom_in(),
                (KeyCode::Char(']'), _) => app.zoom_out(),

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

fn outer_layout(area: ratatui::layout::Rect) -> std::rc::Rc<[ratatui::layout::Rect]> {
    Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3), // scrubber
            Constraint::Length(1), // separator ═══
            Constraint::Length(2), // coordinator bar
            Constraint::Min(1),    // main area
        ])
        .split(area)
}
