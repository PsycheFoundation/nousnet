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
};

use crate::app::App;
use crate::widgets::{
    cluster::ClusterWidget, loss_graph::LossGraphWidget, node::NodeWidget, scrubber::ScrubberWidget,
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
        // Borrow the snapshot and render
        {
            let snapshot = app.current_snapshot().clone();
            let selected = app.selected_node_idx;
            let cursor = app.cursor;

            terminal.draw(|f| {
                let area = f.area();

                // Layout: top panels | loss graph | scrubber
                let outer_chunks = Layout::default()
                    .direction(Direction::Vertical)
                    .constraints([
                        Constraint::Min(10),   // top: cluster + node panels
                        Constraint::Min(8),    // loss graph
                        Constraint::Length(5), // scrubber
                    ])
                    .split(area);

                // Top row: cluster panel (left) + node panel (right)
                let top_chunks = Layout::default()
                    .direction(Direction::Horizontal)
                    .constraints([Constraint::Percentage(35), Constraint::Percentage(65)])
                    .split(outer_chunks[0]);

                f.render_widget(
                    ClusterWidget {
                        snapshot: &snapshot,
                        selected_node_idx: selected,
                    },
                    top_chunks[0],
                );

                f.render_widget(
                    NodeWidget {
                        snapshot: &snapshot,
                        selected_node_idx: selected,
                    },
                    top_chunks[1],
                );

                f.render_widget(
                    LossGraphWidget {
                        nodes: &snapshot.nodes,
                    },
                    outer_chunks[1],
                );

                f.render_widget(
                    ScrubberWidget {
                        timeline: &app.timeline,
                        cursor,
                    },
                    outer_chunks[2],
                );
            })?;
        }

        // Handle input
        let timeout = tick_rate
            .checked_sub(last_tick.elapsed())
            .unwrap_or(Duration::ZERO);

        if event::poll(timeout)? {
            if let Event::Key(key) = event::read()? {
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
                    (KeyCode::Char(' '), _) => app.toggle_play(),
                    (KeyCode::Tab, _) => app.next_node(),
                    (KeyCode::BackTab, _) => app.prev_node(),
                    (KeyCode::Char('1'), _) => app.set_speed(1),
                    (KeyCode::Char('2'), _) => app.set_speed(5),
                    (KeyCode::Char('3'), _) => app.set_speed(20),
                    _ => {}
                }
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
