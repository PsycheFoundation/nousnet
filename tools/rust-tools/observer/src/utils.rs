/// Format a byte count as a human-readable string.
pub fn fmt_bytes(b: u64) -> String {
    if b >= 1_000_000 {
        format!("{:.1} MB", b as f64 / 1_000_000.0)
    } else if b >= 1_000 {
        format!("{:.1} KB", b as f64 / 1_000.0)
    } else {
        format!("{} B", b)
    }
}
