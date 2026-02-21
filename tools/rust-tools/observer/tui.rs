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
    widgets::Tabs,
};

use crate::app::{App, DetailPanel};
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
            let batch_scroll = app.batch_scroll;
            let node_filter_owned: Option<String> =
                selected.and_then(|i| snapshot.nodes.get_index(i).map(|(id, _)| id.clone()));
            let timeline = &app.timeline;
            let waterfall_zoom = app.waterfall_zoom;
            let waterfall_x_scroll = app.waterfall_x_scroll;

            terminal.draw(|f| {
                let area = f.area();
                let outer = outer_layout(area);

                // ── Coordinator bar ───────────────────────────────────────────
                f.render_widget(
                    CoordinatorBarWidget {
                        snapshot: &snapshot,
                    },
                    outer[0],
                );

                // ── Scrubber ──────────────────────────────────────────────────
                f.render_widget(ScrubberWidget { timeline, cursor }, outer[1]);

                // ── Thick separator ═══════════════════════════════════════════
                let sep = outer[2];
                for x in sep.x..sep.x + sep.width {
                    f.buffer_mut()
                        .set_string(x, sep.y, "═", Style::default().fg(Color::DarkGray));
                }

                // ── Main area: node list (left) + tabbed panel (right) ────────
                let main = outer[3];
                node_list_h = main.height as usize;

                let cols = Layout::default()
                    .direction(Direction::Horizontal)
                    .constraints([Constraint::Length(NODE_LIST_WIDTH), Constraint::Min(10)])
                    .split(main);

                let list_area = cols[0];
                let right_area = cols[1];

                // Always-visible node selector
                f.render_widget(
                    NodeListWidget {
                        nodes: &snapshot.nodes,
                        selected_node_idx: selected,
                        node_scroll,
                    },
                    list_area,
                );

                // Tabbed right panel
                match detail_panel {
                    DetailPanel::Node => f.render_widget(
                        NodeWidget {
                            snapshot: &snapshot,
                            selected_node_idx: selected,
                            file_stats: &file_stats,
                        },
                        right_area,
                    ),
                    DetailPanel::Loss => f.render_widget(
                        LossGraphWidget {
                            nodes: &snapshot.nodes,
                        },
                        right_area,
                    ),
                    DetailPanel::Batches => f.render_widget(
                        BatchesWidget {
                            snapshot: &snapshot,
                            selected_node_id: node_filter_owned.as_deref(),
                            scroll: batch_scroll,
                        },
                        right_area,
                    ),
                    DetailPanel::Timeline => f.render_widget(
                        WaterfallWidget {
                            timeline,
                            cursor,
                            nodes: &snapshot.nodes,
                            selected_node_idx: selected,
                            node_scroll,
                            zoom: waterfall_zoom,
                            x_scroll: waterfall_x_scroll,
                        },
                        right_area,
                    ),
                }

                // Overlay tabs on the right panel's top border row
                let tab_idx = match detail_panel {
                    DetailPanel::Timeline => 0,
                    DetailPanel::Node => 1,
                    DetailPanel::Loss => 2,
                    DetailPanel::Batches => 3,
                };
                let tabs_area = ratatui::layout::Rect {
                    x: right_area.x + 1,
                    y: right_area.y,
                    width: right_area.width.saturating_sub(2),
                    height: 1,
                };
                f.render_widget(
                    Tabs::new(vec!["TIMELINE", "NODE", "LOSS", "BATCHES"])
                        .select(tab_idx)
                        .style(Style::default().fg(Color::DarkGray))
                        .highlight_style(Style::default().yellow().bold())
                        .divider(symbols::DOT)
                        .padding(" ", " "),
                    tabs_area,
                );
            })?;
        }

        // Handle input
        let timeout = tick_rate
            .checked_sub(last_tick.elapsed())
            .unwrap_or(Duration::ZERO);

        if event::poll(timeout)?
            && let Event::Key(key) = event::read()?
        {
            match (key.code, key.modifiers) {
                (KeyCode::Char('q'), _) => break,
                (KeyCode::Char('g'), KeyModifiers::NONE) => app.go_first(),
                (KeyCode::Char('G'), _) | (KeyCode::Char('g'), KeyModifiers::SHIFT) => {
                    app.go_last()
                }
                (KeyCode::Left, KeyModifiers::SHIFT) => app.step_backward(50),
                (KeyCode::Right, KeyModifiers::SHIFT) => app.step_forward(50),
                (KeyCode::Left, _) => app.step_backward(1),
                (KeyCode::Right, _) => app.step_forward(1),
                (KeyCode::Up, _) if app.detail_panel == DetailPanel::Batches => {
                    app.scroll_batches_up()
                }
                (KeyCode::Down, _) if app.detail_panel == DetailPanel::Batches => {
                    app.scroll_batches_down()
                }
                (KeyCode::Up, _) => app.prev_node(),
                (KeyCode::Down, _) => app.next_node(),
                (KeyCode::Tab, _) => app.cycle_detail_panel(),
                (KeyCode::Char(' '), _) => app.toggle_play(),
                (KeyCode::Char('1'), _) => app.set_speed(1),
                (KeyCode::Char('2'), _) => app.set_speed(5),
                (KeyCode::Char('3'), _) => app.set_speed(20),
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
            Constraint::Length(2), // coordinator bar
            Constraint::Length(4), // scrubber
            Constraint::Length(1), // separator ═══
            Constraint::Min(1),    // main area
        ])
        .split(area)
}
