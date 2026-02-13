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
    batches::BatchesWidget, cluster::ClusterWidget, event_scroll::EventScrollWidget,
    loss_graph::LossGraphWidget, node::NodeWidget, scrubber::ScrubberWidget,
};

pub fn run(mut app: App) -> io::Result<()> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let tick_rate = Duration::from_millis(200);
    let mut last_tick = Instant::now();

    loop {
        {
            let snapshot = app.current_snapshot().clone();
            let selected = app.selected_node_idx;
            let cursor = app.cursor;
            let file_stats = app.node_file_stats.clone();
            let detail_panel = app.detail_panel;
            let batch_scroll = app.batch_scroll;
            let node_filter_owned: Option<String> =
                selected.and_then(|i| snapshot.nodes.get_index(i).map(|(id, _)| id.clone()));
            let timeline = &app.timeline;

            terminal.draw(|f| {
                let area = f.area();

                // Layout: [top: left + right] | [scrubber]
                let outer_chunks = Layout::default()
                    .direction(Direction::Vertical)
                    .constraints([
                        Constraint::Min(10),   // top panels
                        Constraint::Length(4), // scrubber: border + bar + ts + keybinds
                    ])
                    .split(area);

                // Top: left panel (35%) | right panel (65%)
                let top_chunks = Layout::default()
                    .direction(Direction::Horizontal)
                    .constraints([Constraint::Percentage(35), Constraint::Percentage(65)])
                    .split(outer_chunks[0]);

                // Left: cluster+nodes (fills) | event scroll (6 lines)
                let left_chunks = Layout::default()
                    .direction(Direction::Vertical)
                    .constraints([Constraint::Min(5), Constraint::Length(6)])
                    .split(top_chunks[0]);

                f.render_widget(
                    ClusterWidget {
                        snapshot: &snapshot,
                        selected_node_idx: selected,
                    },
                    left_chunks[0],
                );

                f.render_widget(
                    EventScrollWidget {
                        timeline,
                        cursor,
                        node_filter: node_filter_owned.as_deref(),
                    },
                    left_chunks[1],
                );

                // Right panel: render content widget (draws its own bordered block),
                // then overlay Tabs on its top border row.
                let right_area = top_chunks[1];

                match detail_panel {
                    DetailPanel::Node => {
                        f.render_widget(
                            NodeWidget {
                                snapshot: &snapshot,
                                selected_node_idx: selected,
                                file_stats: &file_stats,
                            },
                            right_area,
                        );
                    }
                    DetailPanel::Loss => {
                        f.render_widget(
                            LossGraphWidget {
                                nodes: &snapshot.nodes,
                            },
                            right_area,
                        );
                    }
                    DetailPanel::Batches => {
                        f.render_widget(
                            BatchesWidget {
                                snapshot: &snapshot,
                                selected_node_id: node_filter_owned.as_deref(),
                                scroll: batch_scroll,
                            },
                            right_area,
                        );
                    }
                }

                // Overlay Tabs onto the content block's top border row.
                let tab_idx = match detail_panel {
                    DetailPanel::Node => 0,
                    DetailPanel::Loss => 1,
                    DetailPanel::Batches => 2,
                };
                let tabs_area = ratatui::layout::Rect {
                    x: right_area.x + 1,
                    y: right_area.y,
                    width: right_area.width.saturating_sub(2),
                    height: 1,
                };
                f.render_widget(
                    Tabs::new(vec!["NODE", "LOSS", "BATCHES"])
                        .select(tab_idx)
                        .style(Style::default().fg(Color::DarkGray))
                        .highlight_style(Style::default().yellow().bold())
                        .divider(symbols::DOT)
                        .padding(" ", " "),
                    tabs_area,
                );

                f.render_widget(ScrubberWidget { timeline, cursor }, outer_chunks[1]);
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
