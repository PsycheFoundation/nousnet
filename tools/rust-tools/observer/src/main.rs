mod app;
mod tui;
mod utils;
mod widgets;

use std::path::PathBuf;
use std::process;

use clap::Parser;
use psyche_event_sourcing::timeline::ClusterTimeline;

/// Observer — scrub through a recorded psyche event timeline.
#[derive(Parser, Debug)]
#[command(name = "observer", version, about)]
struct Cli {
    events_dir: PathBuf,
}

fn main() {
    let cli = Cli::parse();

    if !cli.events_dir.exists() {
        eprintln!(
            "Error: events directory does not exist: {}",
            cli.events_dir.display()
        );
        process::exit(1);
    }

    // Load whatever events exist now; the observer will live-refresh as new ones arrive.
    let timeline = match ClusterTimeline::from_events_dir(&cli.events_dir) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("Error reading events directory: {}", e);
            process::exit(1);
        }
    };

    let app = app::App::new(timeline, cli.events_dir);
    if let Err(e) = tui::run(app) {
        eprintln!("TUI error: {}", e);
        process::exit(1);
    }
}
