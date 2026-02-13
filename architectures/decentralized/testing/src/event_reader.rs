use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::time::Duration;

use psyche_event_sourcing::Event;
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

pub struct EventReader {
    tx: mpsc::UnboundedSender<(String, Event)>,
    pub rx: mpsc::UnboundedReceiver<(String, Event)>,
}

impl Default for EventReader {
    fn default() -> Self {
        Self::new()
    }
}

impl EventReader {
    pub fn new() -> Self {
        let (tx, rx) = mpsc::unbounded_channel();
        Self { tx, rx }
    }

    /// Watch a directory for .postcard event files. Incrementally reads new
    /// events as files are created or appended to, tagging each with `id`.
    pub fn monitor_dir(&self, id: impl Into<String>, dir: PathBuf) -> JoinHandle<()> {
        let id = id.into();
        let tx = self.tx.clone();

        tokio::spawn(async move {
            let mut file_cursors: HashMap<PathBuf, u64> = HashMap::new();
            let mut ticker = tokio::time::interval(Duration::from_millis(100));

            loop {
                ticker.tick().await;
                poll_dir(&dir, &id, &mut file_cursors, &tx);
            }
        })
    }

    /// Watch a base directory for node subdirectories. Each subdir is named after
    /// the node_id (wallet pubkey) and is auto-discovered as containers start up.
    /// Events from each subdir are tagged with the subdir name (node_id).
    pub fn monitor_events_root(&self, base_dir: PathBuf) -> JoinHandle<()> {
        let tx = self.tx.clone();

        tokio::spawn(async move {
            let mut file_cursors: HashMap<PathBuf, u64> = HashMap::new();
            let mut ticker = tokio::time::interval(Duration::from_millis(100));

            loop {
                ticker.tick().await;

                let Ok(entries) = std::fs::read_dir(&base_dir) else {
                    continue;
                };

                for entry in entries.filter_map(|e| e.ok()) {
                    let node_dir = entry.path();
                    if !node_dir.is_dir() {
                        continue;
                    }
                    let id = node_dir
                        .file_name()
                        .unwrap_or_default()
                        .to_string_lossy()
                        .to_string();

                    if poll_dir(&node_dir, &id, &mut file_cursors, &tx).is_none() {
                        return; // receiver dropped
                    }
                }
            }
        })
    }

    pub async fn recv(&mut self) -> Option<(String, Event)> {
        self.rx.recv().await
    }
}

/// Poll a directory for .postcard files, reading new bytes from each.
/// Returns None if the receiver has been dropped.
fn poll_dir(
    dir: &Path,
    id: &str,
    file_cursors: &mut HashMap<PathBuf, u64>,
    tx: &mpsc::UnboundedSender<(String, Event)>,
) -> Option<()> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Some(());
    };

    let mut files: Vec<PathBuf> = entries
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|ext| ext == "postcard"))
        .collect();

    files.sort();

    for file_path in files {
        let cursor = file_cursors.entry(file_path.clone()).or_insert(0);
        match read_new_events(&file_path, *cursor, id, tx) {
            Some(n) => *cursor += n,
            None => return None,
        }
    }

    Some(())
}

/// Read new events from a file starting at `cursor` bytes. Returns the number
/// of bytes consumed, or None if the receiver has been dropped.
fn read_new_events(
    file_path: &Path,
    cursor: u64,
    id: &str,
    tx: &mpsc::UnboundedSender<(String, Event)>,
) -> Option<u64> {
    let mut file = std::fs::File::open(file_path).ok()?;
    let metadata = file.metadata().ok()?;
    if metadata.len() <= cursor {
        return Some(0);
    }
    file.seek(SeekFrom::Start(cursor)).ok()?;
    let mut new_data = Vec::new();
    file.read_to_end(&mut new_data).ok()?;

    let mut local_cursor = 0usize;
    while local_cursor < new_data.len() {
        match decode_cobs_frame::<Event>(&new_data, &mut local_cursor) {
            Some(event) => {
                if tx.send((id.to_string(), event)).is_err() {
                    return None;
                }
            }
            None => break,
        }
    }
    Some(local_cursor as u64)
}

fn decode_cobs_frame<T: serde::de::DeserializeOwned>(data: &[u8], cursor: &mut usize) -> Option<T> {
    if *cursor >= data.len() {
        return None;
    }
    let remaining = &data[*cursor..];
    let delimiter_pos = remaining.iter().position(|&b| b == 0x00)?;
    let frame = &remaining[..=delimiter_pos];
    match postcard::from_bytes_cobs::<T>(&mut frame.to_vec()) {
        Ok(decoded) => {
            *cursor += delimiter_pos + 1;
            Some(decoded)
        }
        Err(_) => None,
    }
}
