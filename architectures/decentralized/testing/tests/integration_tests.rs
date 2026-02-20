// Integration tests for decentralized Psyche.
//
// GPU Support:
// By default, these tests run without GPU. To enable GPU support:
// 1. Set the USE_GPU environment variable: `export USE_GPU=1`
// 2. Or ensure nvidia-smi is available (GPU will be auto-detected)
// The test infrastructure will automatically use docker-compose.gpu.yml when GPU is available.
use std::{path::PathBuf, sync::Arc, time::Duration};

use anchor_client::solana_sdk::signature::{Keypair, Signer};
use bollard::container::StartContainerOptions;
use bollard::{Docker, container::KillContainerOptions};
use psyche_coordinator::{RunState, model::Checkpoint};
use psyche_decentralized_testing::docker_setup::e2e_testing_setup_subscription;
use psyche_decentralized_testing::{
    CLIENT_CONTAINER_PREFIX, EVENTS_BASE_DIR, EventReader, NGINX_PROXY_PREFIX,
    docker_setup::{
        check_client_alive_by_name, check_clients_alive, e2e_testing_setup,
        e2e_testing_setup_with_min, kill_all_clients, spawn_new_client,
        spawn_new_client_with_monitoring,
    },
    utils::{SolanaTestClient, write_keypair_to_file},
};
use psyche_event_sourcing::events::{
    Client, Coordinator, EventData, RunState as EvtRunState, SubscriptionStatus, Train, Warmup,
    client, coordinator, train,
};
use rstest::*;
use serial_test::serial;
use tokio::time;

/// spawn 1 client and run for 3 epochs
/// assert client and coordinator state synchronization
/// assert that the loss decreases in each epoch
#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_one_clients_three_epochs_run() {
    let run_id = "test".to_string();
    let num_of_epochs_to_run = 3;
    let mut current_epoch = -1;
    let mut last_epoch_loss = f64::MAX;

    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());
    let _cleanup = e2e_testing_setup(docker.clone(), 1).await;

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let solana_client = SolanaTestClient::new(run_id, None).await;
    let mut live_interval = time::interval(Duration::from_secs(10));

    loop {
        tokio::select! {
            _ = live_interval.tick() => {
                if let Err(e) = check_clients_alive(docker.clone(), 1).await {
                    panic!("{}", e);
                }
            }
            msg = reader.recv() => {
                let Some((_, event)) = msg else { break };
                match event.data {
                    EventData::Client(Client::StateChanged(client::StateChanged { old_state, new_state, .. })) => {
                        let _coordinator_state = solana_client.get_run_state().await;
                        println!("state change: {old_state:?} => {new_state:?}");
                    }
                    EventData::Train(Train::TrainingFinished(train::TrainingFinished { epoch, step, loss })) => {
                        println!("epoch: {epoch}, step: {step}, loss: {loss:?}");
                        if epoch as i64 > current_epoch {
                            current_epoch = epoch as i64;
                            let Some(loss) = loss else {
                                println!("Reached new epoch but loss was NaN");
                                continue;
                            };
                            assert!(loss < last_epoch_loss * 1.1);
                            last_epoch_loss = loss;
                            if epoch == num_of_epochs_to_run {
                                break;
                            }
                        }
                    }
                    _ => {}
                }
            }
        }
    }
}

/// spawn 2 clients and run for 3 epochs
/// assert that the loss decreases in each epoch
#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_two_clients_three_epochs_run() {
    let run_id = "test".to_string();
    let num_of_epochs_to_run = 3;
    let mut current_epoch = -1;
    let mut last_epoch_loss = f64::MAX;

    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());
    let _cleanup = e2e_testing_setup(docker.clone(), 2).await;

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let solana_client = SolanaTestClient::new(run_id, None).await;
    let mut live_interval = time::interval(Duration::from_secs(10));

    loop {
        tokio::select! {
            _ = live_interval.tick() => {
                if let Err(e) = check_clients_alive(docker.clone(), 2).await {
                    panic!("{}", e);
                }
            }
            msg = reader.recv() => {
                let Some((_, event)) = msg else { break };
                match event.data {
                    EventData::Client(Client::StateChanged(client::StateChanged { old_state, new_state, .. })) => {
                        let _coordinator_state = solana_client.get_run_state().await;
                        println!("state change: {old_state:?} => {new_state:?}");
                    }
                    EventData::Train(Train::TrainingFinished(train::TrainingFinished { epoch, step, loss })) => {
                        println!("epoch: {epoch}, step: {step}, loss: {loss:?}");
                        if epoch as i64 > current_epoch {
                            current_epoch = epoch as i64;
                            let Some(loss) = loss else {
                                println!("Reached new epoch but loss was NaN");
                                continue;
                            };
                            assert!(loss < last_epoch_loss * 1.1);
                            last_epoch_loss = loss;
                            if epoch == num_of_epochs_to_run {
                                break;
                            }
                        }
                    }
                    _ => {}
                }
            }
        }
    }
}

// Test p2p model sharing process
#[rstest]
#[trace]
#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_client_join_and_get_model_p2p(#[values(1, 2)] n_new_clients: u8) {
    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());
    let _cleanup = e2e_testing_setup(docker.clone(), 1).await;

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    println!("Waiting for run to go on with the first client");
    tokio::time::sleep(Duration::from_secs(60)).await;

    println!("Adding new clients");
    for _ in 1..=n_new_clients {
        spawn_new_client(docker.clone(), None).await.unwrap();
    }

    let mut liveness_check_interval = time::interval(Duration::from_secs(10));
    let mut clients_with_model = 0;

    loop {
        tokio::select! {
            _ = liveness_check_interval.tick() => {
                println!("Waiting for epoch to end");
                if let Err(e) = check_clients_alive(docker.clone(), n_new_clients + 1).await {
                    panic!("{}", e);
                }
            }
            msg = reader.recv() => {
                let Some((_, event)) = msg else { break };
                match event.data {
                    EventData::Train(Train::TrainingFinished(train::TrainingFinished { epoch, step, .. })) => {
                        if epoch == 1 && step > 22 {
                            panic!("Second epoch started and the clients did not get the model");
                        }
                    }
                    EventData::Warmup(Warmup::ModelLoadComplete(_)) => {
                        // Check coordinator to verify it's a P2P checkpoint
                        clients_with_model += 1;
                        println!("Client got the model (client #{clients_with_model})");
                        if clients_with_model == n_new_clients {
                            println!("All clients got the model");
                            return;
                        }
                    }
                    _ => {}
                }
            }
        }
    }
}

#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_rejoining_client_delay() {
    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());
    let _cleanup = e2e_testing_setup(docker.clone(), 1).await;

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let solana_client = Arc::new(SolanaTestClient::new("test".to_string(), None).await);

    tokio::time::sleep(Duration::from_secs(30)).await;
    spawn_new_client(docker.clone(), None).await.unwrap();

    let second_client_name = format!("{CLIENT_CONTAINER_PREFIX}-2");

    use psyche_decentralized_testing::chaos::{ChaosAction, ChaosScheduler};
    let scheduler = ChaosScheduler::new(docker.clone(), solana_client.clone());
    scheduler
        .schedule_chaos(
            ChaosAction::Delay {
                duration_secs: 30,
                latency_ms: 2000,
                targets: vec![format!("{CLIENT_CONTAINER_PREFIX}-1")],
            },
            20,
        )
        .await;

    let mut interval = time::interval(Duration::from_secs(10));
    println!("Waiting for training to start");
    loop {
        tokio::select! {
            _ = interval.tick() => {
                println!("Waiting for first epoch to finish");
                if let Err(e) = check_client_alive_by_name(docker.clone(), &second_client_name).await {
                    panic!("{}", e);
                }
                let current_epoch = solana_client.get_current_epoch().await;
                if current_epoch > 1 {
                    panic!("Second epoch started and the clients did not get the model");
                }
            }
            msg = reader.recv() => {
                let Some((_, event)) = msg else { break };
                if let EventData::Warmup(Warmup::ModelLoadComplete(_)) = event.data {
                    println!("Client got the model via P2P");
                    return;
                }
            }
        }
    }
}

/// creates a run and spawns 3 clients
/// Then we kill a client, and we verify that the other two clients are still alive and
/// two healthchecks have been sent by those alive clients.
#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn disconnect_client() {
    let run_id = "test".to_string();
    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());

    let _cleanup = e2e_testing_setup(docker.clone(), 3).await;

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let solana_client = SolanaTestClient::new(run_id, None).await;

    let mut seen_health_check_steps: Vec<u64> = Vec::new();
    let mut untrained_batch_warning_count = 0usize;
    let mut killed_client = false;

    while let Some((node_id, event)) = reader.recv().await {
        match event.data {
            EventData::Client(Client::StateChanged(client::StateChanged {
                old_state,
                new_state,
                epoch,
                step,
            })) => {
                println!(
                    "epoch: {epoch} step: {step} state change {node_id} - {old_state:?} => {new_state:?}"
                );

                if step == 20 {
                    println!("Max number of steps reached for test");
                    break;
                }

                if old_state == EvtRunState::WaitingForMembers {
                    let epoch_clients = solana_client.get_current_epoch_clients().await;
                    println!(
                        "Starting epoch: {epoch} with {} clients",
                        epoch_clients.len()
                    );
                }

                if killed_client && epoch > 0 && new_state == EvtRunState::WaitingForMembers {
                    println!("Epoch ended after killing client, breaking to verify assertions");
                    break;
                }

                if epoch == 0 && step == 2 && old_state == EvtRunState::RoundTrain && !killed_client
                {
                    let epoch_clients = solana_client.get_current_epoch_clients().await;
                    assert_eq!(epoch_clients.len(), 3);

                    docker
                        .kill_container(
                            &format!("{CLIENT_CONTAINER_PREFIX}-1"),
                            Some(KillContainerOptions { signal: "SIGKILL" }),
                        )
                        .await
                        .unwrap();
                    println!("Killed client: {CLIENT_CONTAINER_PREFIX}-1");
                    killed_client = true;
                }

                if killed_client
                    && seen_health_check_steps.len() >= 2
                    && new_state == EvtRunState::Cooldown
                {
                    let epoch_clients = solana_client.get_current_epoch_clients().await;
                    assert!(
                        epoch_clients.len() <= 2,
                        "Expected at most 2 clients after kill, got {}",
                        epoch_clients.len()
                    );
                    break;
                }
            }

            EventData::Client(Client::HealthCheckFailed(client::HealthCheckFailed {
                current_step,
                ..
            })) => {
                println!("Health check failed reported by {node_id}");
                let clients_ids: Vec<String> = solana_client
                    .get_clients()
                    .await
                    .iter()
                    .map(|client| client.id.to_string())
                    .collect();
                seen_health_check_steps.push(current_step);
                assert!(clients_ids.contains(&node_id));
            }

            EventData::Train(Train::UntrainedBatchWarning(train::UntrainedBatchWarning {
                batch_id,
                ..
            })) => {
                println!("Untrained batch warning: {batch_id:?}");
                untrained_batch_warning_count += 1;
            }

            _ => {}
        }
    }

    assert_eq!(
        seen_health_check_steps.len(),
        2,
        "Two healthchecks should have been sent"
    );
    assert!(
        untrained_batch_warning_count <= 3,
        "Num of untrained batch warnings should be less than 4"
    );
}

// Drop a client below the minimum required, go to WaitingForMembers
// Reconnect a client and then go back to warmup
#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn drop_a_client_waitingformembers_then_reconnect() {
    let num_of_epochs_to_run = 3;
    let mut current_epoch = -1;
    let run_id = "test".to_string();
    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());

    let _cleanup = e2e_testing_setup(docker.clone(), 2).await;

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let solana_client = SolanaTestClient::new(run_id, None).await;

    let mut train_reached = false;
    while let Some((_, event)) = reader.recv().await {
        match event.data {
            EventData::Client(Client::StateChanged(client::StateChanged {
                old_state,
                new_state,
                epoch,
                step,
            })) => {
                let coordinator_state = solana_client.get_run_state().await;
                println!(
                    "epoch: {epoch} step: {step} state change: {old_state:?} => {new_state:?}"
                );

                if new_state == EvtRunState::RoundTrain && !train_reached {
                    println!(
                        "Train started, killing container {}...",
                        &format!("{CLIENT_CONTAINER_PREFIX}-2")
                    );
                    docker
                        .kill_container(
                            &format!("{CLIENT_CONTAINER_PREFIX}-2"),
                            Some(KillContainerOptions { signal: "SIGKILL" }),
                        )
                        .await
                        .unwrap();
                    tokio::time::sleep(Duration::from_secs(2)).await;
                    train_reached = true;
                }

                if train_reached && coordinator_state == RunState::WaitingForMembers {
                    println!("WaitingForMembers seen");
                    break;
                }
            }
            EventData::Train(Train::TrainingFinished(train::TrainingFinished {
                epoch,
                step,
                loss,
            })) => {
                println!("epoch: {epoch}, step: {step}, loss: {loss:?}");
                if epoch as i64 > current_epoch {
                    current_epoch = epoch as i64;
                    if epoch == num_of_epochs_to_run {
                        println!("Epoch {epoch} reached. Stopping");
                        break;
                    }
                }
            }
            _ => {}
        }
    }

    println!("Waiting 5s to see if we are still in WaitingForMembers...");
    tokio::time::sleep(Duration::from_secs(5)).await;
    let coordinator_state = solana_client.get_run_state().await;
    assert_eq!(coordinator_state, RunState::WaitingForMembers);
    println!("Still in WaitingForMembers after 5 seconds. Success");

    println!("Starting new client...");
    spawn_new_client(docker.clone(), None).await.unwrap();

    assert!(
        solana_client.wait_for_run_state(RunState::Warmup, 30).await,
        "System should have returned to Warmup state after client reconnection"
    );
    println!("Successfully returned to Warmup state after client reconnection");
}

#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_when_all_clients_disconnect_checkpoint_is_hub() {
    let num_of_epochs_to_run = 3;
    let mut current_epoch = -1;
    let mut last_epoch_loss = f64::MAX;
    let run_id = "test".to_string();
    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());

    let _cleanup = e2e_testing_setup(docker.clone(), 2).await;

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let solana_client = SolanaTestClient::new(run_id, None).await;
    let mut has_spawned_new_client_yet = false;
    let mut has_checked_p2p_checkpoint = false;
    let mut liveness_check_interval = time::interval(Duration::from_secs(10));
    println!("starting loop");
    loop {
        tokio::select! {
            _ = liveness_check_interval.tick() => {
                let clients = solana_client.get_clients().await;
                let current_epoch = solana_client.get_current_epoch().await;
                let current_step = solana_client.get_last_step().await;
                println!(
                    "Clients: {}, Current epoch: {}, Current step: {}",
                    clients.len(), current_epoch, current_step
                );

                if !has_checked_p2p_checkpoint && current_epoch == 1 {
                    let checkpoint = solana_client.get_checkpoint().await;
                    if matches!(checkpoint, Checkpoint::P2P(_)) {
                        println!("Checkpoint was P2P");
                        has_checked_p2p_checkpoint = true;
                    } else {
                        continue;
                    }

                    tokio::time::sleep(Duration::from_secs(10)).await;
                    println!("Killing all clients to test checkpoint change to Hub");
                    kill_all_clients(&docker, "SIGKILL").await;

                    tokio::time::sleep(Duration::from_secs(20)).await;
                    let c1 = spawn_new_client_with_monitoring(docker.clone()).await.unwrap();
                    println!("Spawned new client {c1}");
                    let c2 = spawn_new_client_with_monitoring(docker.clone()).await.unwrap();
                    println!("Spawned new client {c2}");
                    has_spawned_new_client_yet = true;
                    continue;
                }

                if has_spawned_new_client_yet {
                    let checkpoint = solana_client.get_checkpoint().await;
                    if matches!(checkpoint, Checkpoint::Hub(_)) {
                        println!("Checkpoint is Hub, test successful");
                        return;
                    } else {
                        println!("Checkpoint is not Hub yet, waiting...");
                    }
                }
            }
            msg = reader.recv() => {
                let Some((_, event)) = msg else { break };
                match event.data {
                    EventData::Warmup(Warmup::ModelLoadComplete(_)) => {
                        println!("Client loaded model");
                    }
                    EventData::Train(Train::TrainingFinished(train::TrainingFinished { epoch, step, loss })) => {
                        println!("epoch: {epoch}, step: {step}, loss: {loss:?}");
                        if epoch as i64 > current_epoch {
                            current_epoch = epoch as i64;
                            let Some(loss) = loss else {
                                println!("Reached new epoch but loss was NaN");
                                continue;
                            };
                            assert!(loss < last_epoch_loss);
                            last_epoch_loss = loss;
                            if epoch == num_of_epochs_to_run {
                                println!("Epoch {epoch} reached. Stopping");
                                break;
                            }
                        }
                    }
                    _ => {}
                }
            }
        }
    }
}

#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_solana_subscriptions() {
    let num_of_epochs_to_run = 3;

    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());
    let _cleanup = e2e_testing_setup_subscription(docker.clone(), 2).await;

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let mut live_interval = time::interval(Duration::from_secs(10));
    let mut subscription_events: Vec<(String, SubscriptionStatus)> = Vec::new();

    loop {
        tokio::select! {
            _ = live_interval.tick() => {
                if let Err(e) = check_clients_alive(docker.clone(), 2).await {
                    panic!("{}", e);
                }
            }
            msg = reader.recv() => {
                let Some((_, event)) = msg else { break };
                match event.data {
                    EventData::Client(Client::StateChanged(client::StateChanged { old_state, new_state, epoch, step })) => {
                        if old_state == EvtRunState::WaitingForMembers {
                            println!("Starting epoch: {epoch}");
                        }

                        if step == 5 && new_state == EvtRunState::RoundWitness {
                            println!("stop container {NGINX_PROXY_PREFIX}-1");
                            docker
                                .stop_container(&format!("{NGINX_PROXY_PREFIX}-1"), None)
                                .await
                                .unwrap();
                        }
                        if step == 15 && new_state == EvtRunState::RoundWitness {
                            println!("resume container {NGINX_PROXY_PREFIX}-1");
                            docker
                                .start_container(
                                    &format!("{NGINX_PROXY_PREFIX}-1"),
                                    None::<StartContainerOptions<String>>,
                                )
                                .await
                                .unwrap();
                        }
                        if step == 25 && new_state == EvtRunState::RoundWitness {
                            println!("stop container {NGINX_PROXY_PREFIX}-2");
                            docker
                                .stop_container(&format!("{NGINX_PROXY_PREFIX}-2"), None)
                                .await
                                .unwrap();
                        }
                        if step == 30 && new_state == EvtRunState::RoundWitness {
                            println!("resume container {NGINX_PROXY_PREFIX}-2");
                            docker
                                .start_container(
                                    &format!("{NGINX_PROXY_PREFIX}-2"),
                                    None::<StartContainerOptions<String>>,
                                )
                                .await
                                .unwrap();
                        }

                        if epoch == num_of_epochs_to_run {
                            break;
                        }
                    }
                    EventData::Coordinator(Coordinator::SolanaSubscriptionChanged(coordinator::SolanaSubscriptionChanged { url, status })) => {
                        println!("Solana subscription {url} status: {status:?}");
                        subscription_events.push((url, status));
                    }
                    _ => {}
                }
            }
        }
    }

    // skip the first 3 events since init subscriptions can vary the order
    subscription_events = subscription_events[3..].to_vec();
    subscription_events.dedup();

    let expected: Vec<(String, SubscriptionStatus)> = vec![
        // proxy 1 shutdown and reconnection
        (
            format!("ws://{NGINX_PROXY_PREFIX}-1:8901/ws/"),
            SubscriptionStatus::Down,
        ),
        (
            format!("ws://{NGINX_PROXY_PREFIX}-1:8901/ws/"),
            SubscriptionStatus::Up,
        ),
        // proxy 2 shutdown and reconnection
        (
            format!("ws://{NGINX_PROXY_PREFIX}-2:8902/ws/"),
            SubscriptionStatus::Down,
        ),
        (
            format!("ws://{NGINX_PROXY_PREFIX}-2:8902/ws/"),
            SubscriptionStatus::Up,
        ),
    ];

    assert_eq!(subscription_events, expected);
    println!("subscription_events: {subscription_events:?}");
}

#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_everybody_leaves_in_warmup() {
    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());

    let _cleanup = e2e_testing_setup(docker.clone(), 1).await;
    tokio::time::sleep(Duration::from_secs(20)).await;

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let client_1_name = format!("{CLIENT_CONTAINER_PREFIX}-1");

    // Phase 1: wait for Warmup, then kill client 1
    while let Some((_, event)) = reader.recv().await {
        if let EventData::Client(Client::StateChanged(client::StateChanged {
            old_state: EvtRunState::WaitingForMembers,
            new_state: EvtRunState::Warmup,
            ..
        })) = event.data
        {
            println!("Warmup reached, killing container...");
            docker
                .kill_container(
                    &client_1_name,
                    Some(KillContainerOptions { signal: "SIGKILL" }),
                )
                .await
                .unwrap();
            break;
        }
    }

    // Phase 2: spawn client 2, wait for it to reach Cooldown
    println!("Starting new client...");
    spawn_new_client(docker.clone(), None).await.unwrap();
    println!("New client started");

    // Continue receiving from the same reader — it auto-discovers client 2's subdir
    while let Some((_, event)) = reader.recv().await {
        if let EventData::Client(Client::StateChanged(client::StateChanged {
            old_state: EvtRunState::RoundWitness,
            new_state: EvtRunState::Cooldown,
            ..
        })) = event.data
        {
            println!("Epoch restarted correctly, finishing test");
            break;
        }
    }
}

/// Tests that if your only peer disconnects, the new client goes back to fetching the model from Hub
#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_lost_only_peer_go_back_to_hub_checkpoint() {
    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());
    let _cleanup = e2e_testing_setup(docker.clone(), 1).await;

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let mut first_client_killed = false;
    let mut spawned_second_client = false;

    let second_client_name: String = format!("{CLIENT_CONTAINER_PREFIX}-2");
    let mut live_interval = time::interval(Duration::from_secs(10));
    loop {
        tokio::select! {
            _ = live_interval.tick() => {
                if !spawned_second_client {
                    continue;
                }
                if let Err(e) = check_client_alive_by_name(docker.clone(), &second_client_name).await {
                    panic!("Second client crashed after first was killed. {e}");
                }
            }
            msg = reader.recv() => {
                let Some((_, event)) = msg else { break };
                match event.data {
                    EventData::Client(Client::StateChanged(client::StateChanged { new_state, step, .. })) => {
                        if new_state == EvtRunState::RoundTrain && !spawned_second_client {
                            println!("Joining a second client to the run");
                            spawn_new_client(docker.clone(), None).await.unwrap();
                            spawned_second_client = true;
                        }

                        if new_state == EvtRunState::Cooldown
                            && !first_client_killed
                            && spawned_second_client
                        {
                            println!("Cooldown reached, killing the first client");
                            docker
                                .kill_container(
                                    &format!("{CLIENT_CONTAINER_PREFIX}-1"),
                                    Some(KillContainerOptions { signal: "SIGKILL" }),
                                )
                                .await
                                .unwrap();
                            first_client_killed = true;
                            println!("First client killed, waiting to see if second client continues...");
                        }

                        if step > 0 {
                            // suppress RoundTrain/RoundWitness spam
                        } else {
                            println!("step={step} state => {new_state:?}");
                        }
                    }
                    EventData::Warmup(Warmup::ModelLoadComplete(_)) => {
                        if spawned_second_client && first_client_killed {
                            // Use coordinator to verify it's a Hub checkpoint (not P2P)
                            let checkpoint = {
                                // brief wait for coordinator to update
                                tokio::time::sleep(Duration::from_secs(1)).await;
                                let solana = SolanaTestClient::new("test".to_string(), None).await;
                                solana.get_checkpoint().await
                            };
                            assert!(
                                matches!(checkpoint, Checkpoint::Hub(_)),
                                "The model should be obtained from Hub since the other client disconnected"
                            );
                            println!("Model successfully obtained from Hub");
                            return;
                        }
                    }
                    _ => {}
                }
            }
        }
    }
}

#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_pause_and_resume_run() {
    let run_id = "test".to_string();
    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());

    let owner_keypair = Arc::new(Keypair::new());
    let client_keypair = Arc::new(Keypair::new());

    let owner_path = PathBuf::from(format!(
        "/tmp/test-owner-keypair-{}.json",
        std::process::id()
    ));
    let client_path = PathBuf::from(format!(
        "/tmp/test-client-keypair-{}.json",
        std::process::id()
    ));
    write_keypair_to_file(&owner_keypair, &owner_path).expect("Failed to write owner keypair");
    write_keypair_to_file(&client_keypair, &client_path).expect("Failed to write client keypair");

    println!("Generated owner keypair: {}", owner_keypair.pubkey());
    println!("Generated client keypair: {}", client_keypair.pubkey());

    let _cleanup =
        e2e_testing_setup_with_min(docker.clone(), 0, 1, Some(owner_path.as_path())).await;

    let solana_client = SolanaTestClient::new(run_id.clone(), Some(owner_keypair.clone())).await;

    let container = spawn_new_client(docker.clone(), Some(client_path.as_path()))
        .await
        .unwrap();
    println!("Spawned client: {}", container);

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let mut paused = false;
    let mut client_killed = false;
    let mut rejoined_client = false;
    let mut current_epoch = -1;
    let mut last_epoch_loss = f64::MAX;
    let num_epochs_after_rejoin = 2;

    println!("Waiting for training to start...");
    while let Some((_, event)) = reader.recv().await {
        match event.data {
            EventData::Client(Client::StateChanged(client::StateChanged {
                old_state,
                new_state,
                epoch,
                step,
            })) => {
                println!(
                    "epoch: {epoch} step: {step} state change: {old_state:?} => {new_state:?}"
                );

                if !paused && step >= 5 && new_state == EvtRunState::RoundTrain {
                    println!("Pausing the run...");
                    solana_client
                        .set_paused(true)
                        .await
                        .expect("Failed to pause run");
                    paused = true;
                    println!("Run paused! Waiting for Paused state...");
                }

                if paused && !client_killed && new_state == EvtRunState::Paused {
                    println!("Coordinator is in Paused state. Killing client and resuming...");
                    docker
                        .kill_container(
                            &container,
                            Some(KillContainerOptions { signal: "SIGKILL" }),
                        )
                        .await
                        .unwrap();
                    client_killed = true;

                    tokio::time::sleep(Duration::from_secs(2)).await;

                    println!("Resuming the run...");
                    solana_client
                        .set_paused(false)
                        .await
                        .expect("Failed to resume run");

                    tokio::time::sleep(Duration::from_secs(3)).await;

                    println!("Rejoining with same client keypair...");
                    let new_container =
                        spawn_new_client(docker.clone(), Some(client_path.as_path()))
                            .await
                            .unwrap();
                    println!("Rejoined client: {}", new_container);
                    rejoined_client = true;
                }
            }
            EventData::Train(Train::TrainingFinished(train::TrainingFinished {
                epoch,
                step,
                loss,
            })) => {
                println!("epoch: {epoch}, step: {step}, loss: {loss:?}");

                if rejoined_client && epoch as i64 > current_epoch {
                    current_epoch = epoch as i64;

                    let Some(loss) = loss else {
                        println!("Reached new epoch but loss was NaN");
                        continue;
                    };

                    assert!(
                        loss < last_epoch_loss * 1.25,
                        "Loss should not increase significantly"
                    );
                    assert!(loss > 0.0);
                    last_epoch_loss = loss;

                    if epoch >= num_epochs_after_rejoin {
                        println!(
                            "Trained for {num_epochs_after_rejoin} epochs after rejoin. Test successful!"
                        );
                        return;
                    }
                }
            }
            EventData::Warmup(Warmup::ModelLoadComplete(_)) => {
                if rejoined_client {
                    // After rejoin, verify via coordinator that checkpoint is not P2P
                    let checkpoint = solana_client.get_checkpoint().await;
                    assert!(
                        !matches!(checkpoint, Checkpoint::P2P(_)),
                        "After pause/resume with all clients disconnected, checkpoint should be Hub"
                    );
                    println!("Hub checkpoint verified after rejoin!");
                }
            }
            _ => {}
        }
    }
}
