use psyche_event_sourcing::timeline::ClusterTimeline;
use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, Paragraph, Widget},
};

pub struct ScrubberWidget<'a> {
    pub timeline: &'a ClusterTimeline,
    pub cursor: usize,
}

impl<'a> Widget for ScrubberWidget<'a> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        let block = Block::default().borders(Borders::TOP);
        let inner = block.inner(area);
        block.render(area, buf);

        let total = self.timeline.len();
        let (ts_start, ts_end, ts_cursor) =
            if let Some((start, end)) = self.timeline.timestamp_range() {
                let cursor_ts = self
                    .timeline
                    .entries()
                    .get(self.cursor)
                    .map(|e| e.timestamp())
                    .unwrap_or(start);
                (
                    start.format("%H:%M:%S").to_string(),
                    end.format("%H:%M:%S").to_string(),
                    cursor_ts.format("%H:%M:%S").to_string(),
                )
            } else {
                ("--:--:--".into(), "--:--:--".into(), "--:--:--".into())
            };

        // Scrubber bar: ◄──────────────●──────────────────►
        let bar_width = inner.width.saturating_sub(4) as usize;
        let pos = if total > 1 && bar_width > 0 {
            (self.cursor * bar_width / (total - 1)).min(bar_width)
        } else {
            0
        };

        let bar: String = std::iter::once('◄')
            .chain(std::iter::repeat('─').take(pos))
            .chain(std::iter::once('●'))
            .chain(std::iter::repeat('─').take(bar_width.saturating_sub(pos)))
            .chain(std::iter::once('►'))
            .collect();

        let lines = vec![
            Line::from(Span::styled(bar, Style::default().fg(Color::Cyan))),
            Line::from(vec![
                Span::styled(&ts_start, Style::default().fg(Color::DarkGray)),
                Span::raw(format!(
                    "{:^width$}",
                    ts_cursor,
                    width = inner
                        .width
                        .saturating_sub((ts_start.len() + ts_end.len()) as u16)
                        as usize
                )),
                Span::styled(&ts_end, Style::default().fg(Color::DarkGray)),
            ]),
            Line::from(Span::styled(
                format!(
                    "{}/{} events  [←/→] step  [Shift+←/→] ×50  [Space] play  [Tab] next node  [1/2/3] speed  [g/G] first/last  [q] quit",
                    self.cursor + 1,
                    total,
                ),
                Style::default()
                    .fg(Color::DarkGray)
                    .add_modifier(Modifier::ITALIC),
            )),
        ];

        Paragraph::new(lines).render(inner, buf);
    }
}
