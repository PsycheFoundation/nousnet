use iroh::endpoint::{AfterHandshakeOutcome, ConnectionInfo, EndpointHooks};
use iroh::{EndpointId, Watcher};
use n0_future::task::AbortOnDropHandle;
use psyche_metrics::SelectedPath;
use std::collections::HashMap;
use std::sync::{Arc, RwLock};
use std::time::{Duration, Instant};
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender};
use tokio::task::JoinSet;
use tracing::{Instrument, debug, info};

#[derive(Debug, Clone)]
pub struct ConnectionData {
    pub endpoint_id: EndpointId,
    /// Throughput in bytes/sec
    pub bandwidth: f64,
    pub selected_path: Option<SelectedPath>,
}

impl ConnectionData {
    /// Get the latency from the selected path if available
    pub fn latency(&self) -> Option<Duration> {
        self.selected_path.as_ref().map(|p| p.rtt)
    }
}

/// track active connections and their metadata
#[derive(Clone, Debug)]
pub struct ConnectionMonitor {
    tx: UnboundedSender<ConnectionInfo>,
    connections: Arc<RwLock<HashMap<EndpointId, ConnectionData>>>,
    _task: Arc<AbortOnDropHandle<()>>,
}

impl EndpointHooks for ConnectionMonitor {
    async fn after_handshake(&self, conn: &ConnectionInfo) -> AfterHandshakeOutcome {
        self.tx.send(conn.clone()).ok();
        AfterHandshakeOutcome::Accept
    }
}

impl Default for ConnectionMonitor {
    fn default() -> Self {
        let (tx, rx) = tokio::sync::mpsc::unbounded_channel();
        let connections = Arc::new(RwLock::new(HashMap::new()));
        let connections_clone = connections.clone();

        let task = tokio::spawn(
            Self::run(rx, connections_clone).instrument(tracing::debug_span!("connection_monitor")),
        );

        Self {
            tx,
            connections,
            _task: Arc::new(AbortOnDropHandle::new(task)),
        }
    }
}

impl ConnectionMonitor {
    async fn run(
        mut rx: UnboundedReceiver<ConnectionInfo>,
        connections: Arc<RwLock<HashMap<EndpointId, ConnectionData>>>,
    ) {
        let mut tasks = JoinSet::new();

        loop {
            tokio::select! {
                Some(conn) = rx.recv() => {
                    let remote_id = conn.remote_id();
                    let alpn = String::from_utf8_lossy(conn.alpn()).to_string();

                    let selected_path = Self::extract_selected_path(&conn.paths());

                    if let Some(ref path) = selected_path {
                        info!(
                            remote = %remote_id.fmt_short(),
                            %alpn,
                            path = %path,
                            "new connection"
                        );
                    } else {
                        info!(
                            remote = %remote_id.fmt_short(),
                            %alpn,
                            "new connection (no selected path)"
                        );
                    }

                    {
                        let mut conns = connections.write().unwrap();
                        let prev_bandwidth = conns
                            .get(&remote_id)
                            .map(|d| d.bandwidth)
                            .unwrap_or(0.0);
                        conns.insert(remote_id, ConnectionData {
                            endpoint_id: remote_id,
                            bandwidth: prev_bandwidth,
                            selected_path,
                        });
                    }

                    // spawn a task to monitor this connection continuously
                    let connections_clone = connections.clone();
                    let paths_watcher = conn.paths();
                    tasks.spawn(async move {
                        let mut update_interval = tokio::time::interval(Duration::from_secs(5));
                        update_interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
                        let mut prev_rx_bytes: u64 = 0;
                        let mut prev_rx_time = Instant::now();

                        loop {
                            tokio::select! {
                                _ = update_interval.tick() => {
                                    let rx_bytes = Self::extract_total_rx_bytes(&paths_watcher);

                                    let now = Instant::now();
                                    let elapsed = now.duration_since(prev_rx_time).as_secs_f64();
                                    let bandwidth = if elapsed > 0.0 {
                                        (rx_bytes.saturating_sub(prev_rx_bytes)) as f64 / elapsed
                                    } else {
                                        0.0
                                    };
                                    prev_rx_bytes = rx_bytes;
                                    prev_rx_time = now;

                                    let selected_path = Self::extract_selected_path(&paths_watcher);

                                    let mut conns = connections_clone.write().unwrap();
                                    if let Some(data) = conns.get_mut(&remote_id) {
                                        let path_changed = data.selected_path != selected_path;

                                        // calculate latency delta if both old and new have latency info
                                        let latency_delta = match (&data.selected_path, &selected_path) {
                                            (Some(old_path), Some(new_path)) => {
                                                old_path.rtt.as_millis().abs_diff(new_path.rtt.as_millis())
                                            }
                                            _ => 0,
                                        };

                                        data.bandwidth = bandwidth;

                                        if bandwidth > 0.0 {
                                            debug!(
                                                remote = %remote_id.fmt_short(),
                                                bandwidth_kbps = format_args!("{:.1}", bandwidth / 1024.0),
                                                latency_ms = selected_path.as_ref().map(|p| p.rtt.as_millis()),
                                                "connection stats update"
                                            );
                                        }
                                        data.selected_path = selected_path.clone();

                                        if path_changed {
                                            if let Some(ref path) = selected_path {
                                                info!(
                                                    remote = %remote_id.fmt_short(),
                                                    path = %path,
                                                    "selected path changed"
                                                );
                                            } else {
                                                info!(
                                                    remote = %remote_id.fmt_short(),
                                                    "selected path removed"
                                                );
                                            }
                                        } else if latency_delta > 50 {
                                            if let Some(ref path) = selected_path {
                                                debug!(
                                                    remote = %remote_id.fmt_short(),
                                                    latency_ms = path.rtt.as_millis(),
                                                    delta_ms = latency_delta,
                                                    "latency changed"
                                                );
                                            }
                                        }
                                    }
                                }
                                result = conn.closed() => {
                                    match result {
                                        Some((close_reason, stats)) => {
                                            info!(
                                                remote = %remote_id.fmt_short(),
                                                %alpn,
                                                ?close_reason,
                                                udp_rx = stats.udp_rx.bytes,
                                                udp_tx = stats.udp_tx.bytes,
                                                "connection closed"
                                            );
                                        }
                                        None => {
                                            debug!(
                                                remote = %remote_id.fmt_short(),
                                                %alpn,
                                                "connection closed before tracking started"
                                            );
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }.instrument(tracing::Span::current()));
                }
                Some(res) = tasks.join_next(), if !tasks.is_empty() => {
                    res.expect("connection close task panicked");
                }
                else => break,
            }
        }

        while let Some(res) = tasks.join_next().await {
            res.expect("connection close task panicked");
        }
    }

    /// extract selected path info from a paths watcher
    fn extract_selected_path<T: Watcher<Value = iroh::endpoint::PathInfoList>>(
        paths_watcher: &T,
    ) -> Option<SelectedPath> {
        let paths = paths_watcher.peek();
        paths
            .iter()
            .find(|p| p.is_selected())
            .map(|path| SelectedPath {
                addr: format!("{:?}", path.remote_addr()),
                rtt: path.rtt(),
            })
    }

    /// extract total rx bytes across all paths from a paths watcher
    fn extract_total_rx_bytes<T: Watcher<Value = iroh::endpoint::PathInfoList>>(
        paths_watcher: &T,
    ) -> u64 {
        let paths = paths_watcher.peek();
        paths.iter().map(|p| p.stats().udp_rx.bytes).sum()
    }

    /// get connection data for a specific endpoint
    pub fn get_connection(&self, endpoint_id: &EndpointId) -> Option<ConnectionData> {
        let conns = self.connections.read().unwrap();
        conns.get(endpoint_id).cloned()
    }

    /// get all active connections
    pub fn get_all_connections(&self) -> Vec<ConnectionData> {
        let conns = self.connections.read().unwrap();
        conns.values().cloned().collect()
    }

    /// get latency for a specific endpoint
    pub fn get_latency(&self, endpoint_id: &EndpointId) -> Option<Duration> {
        let conns = self.connections.read().unwrap();
        conns.get(endpoint_id).and_then(|data| data.latency())
    }

    /// get measured throughput (bytes/sec) for a specific endpoint
    pub fn get_bandwidth(&self, endpoint_id: &EndpointId) -> Option<f64> {
        let conns = self.connections.read().unwrap();
        conns.get(endpoint_id).map(|data| data.bandwidth)
    }
}
