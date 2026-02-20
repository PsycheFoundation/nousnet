use std::{path::PathBuf, sync::Arc, time::Duration};

use bollard::Docker;
use psyche_decentralized_testing::{
    CLIENT_CONTAINER_PREFIX, EVENTS_BASE_DIR, EventReader, VALIDATOR_CONTAINER_PREFIX,
    chaos::{ChaosAction, ChaosScheduler},
    docker_setup::{check_clients_alive, e2e_testing_setup},
    utils::SolanaTestClient,
};
use psyche_event_sourcing::events::{EventData, Train, train};

use rstest::*;
use serial_test::serial;
use tokio::time;

#[ignore = "These tests are a bit flaky, so we need to make sure they work properly."]
#[rstest]
#[trace]
#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_pause_solana_validator(
    #[values(1, 2)] n_clients: u8,
    #[values(0, 10)] pause_step: u64,
) {
    let run_id = "test".to_string();
    let num_of_epochs_to_run = 2;
    let mut current_epoch = -1;
    let mut last_epoch_loss = f64::MAX;

    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());
    let _cleanup = if n_clients == 1 {
        e2e_testing_setup(docker.clone(), 1).await
    } else {
        e2e_testing_setup(docker.clone(), 2).await
    };

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let solana_client = Arc::new(SolanaTestClient::new(run_id, None).await);

    tokio::time::sleep(Duration::from_secs(10)).await;

    let chaos_targets = vec![format!("{VALIDATOR_CONTAINER_PREFIX}-1")];
    let chaos_scheduler = ChaosScheduler::new(docker.clone(), solana_client);
    chaos_scheduler
        .schedule_chaos(
            ChaosAction::Pause {
                duration_secs: 60,
                targets: chaos_targets,
            },
            pause_step,
        )
        .await;

    let mut liveness_check_interval = time::interval(Duration::from_secs(10));
    println!("Train starting");

    loop {
        tokio::select! {
            _ = liveness_check_interval.tick() => {
                if let Err(e) = check_clients_alive(docker.clone(), n_clients).await {
                    panic!("{}", e);
                }
            }
            msg = reader.recv() => {
                let Some((_, event)) = msg else { break };
                if let EventData::Train(Train::TrainingFinished(train::TrainingFinished { epoch, step, loss })) = event.data {
                    let loss = loss.unwrap();
                    println!("epoch: {epoch}, step: {step}, loss: {loss}");
                    if epoch as i64 > current_epoch {
                        current_epoch = epoch as i64;
                        assert!(loss < last_epoch_loss);
                        last_epoch_loss = loss;
                        if epoch == num_of_epochs_to_run {
                            break;
                        }
                    }
                }
            }
        }
    }
}

#[ignore = "These tests are a bit flaky, so we need to make sure they work properly."]
#[rstest]
#[trace]
#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_delay_solana_test_validator(
    #[values(1, 2)] n_clients: u8,
    #[values(0, 10)] delay_step: u64,
    #[values(1000, 5000)] delay_milis: i64,
) {
    let run_id = "test".to_string();
    let num_of_epochs_to_run = 2;
    let mut current_epoch = -1;
    let mut last_epoch_loss = f64::MAX;

    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());
    let _cleanup = if n_clients == 1 {
        e2e_testing_setup(docker.clone(), 1).await
    } else {
        e2e_testing_setup(docker.clone(), 2).await
    };

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let solana_client = Arc::new(SolanaTestClient::new(run_id, None).await);

    tokio::time::sleep(Duration::from_secs(10)).await;

    let chaos_targets = vec![format!("{VALIDATOR_CONTAINER_PREFIX}-1")];
    let chaos_scheduler = ChaosScheduler::new(docker.clone(), solana_client);
    chaos_scheduler
        .schedule_chaos(
            ChaosAction::Delay {
                duration_secs: 120,
                latency_ms: delay_milis,
                targets: chaos_targets,
            },
            delay_step,
        )
        .await;

    let mut liveness_check_interval = time::interval(Duration::from_secs(10));
    println!("Train starting");

    loop {
        tokio::select! {
            _ = liveness_check_interval.tick() => {
                if let Err(e) = check_clients_alive(docker.clone(), n_clients).await {
                    panic!("{}", e);
                }
            }
            msg = reader.recv() => {
                let Some((_, event)) = msg else { break };
                if let EventData::Train(Train::TrainingFinished(train::TrainingFinished { epoch, step, loss })) = event.data {
                    let loss = loss.unwrap();
                    println!("epoch: {epoch}, step: {step}, loss: {loss}");
                    if epoch as i64 > current_epoch {
                        current_epoch = epoch as i64;
                        assert!(loss < last_epoch_loss);
                        last_epoch_loss = loss;
                        if epoch == num_of_epochs_to_run {
                            break;
                        }
                    }
                }
            }
        }
    }
}

#[ignore = "These tests are a bit flaky, so we need to make sure they work properly."]
#[rstest]
#[trace]
#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_delay_solana_client(#[values(1, 2)] n_clients: u8, #[values(0, 10)] delay_step: u64) {
    let run_id = "test".to_string();
    let num_of_epochs_to_run = 2;
    let mut current_epoch = -1;
    let mut last_epoch_loss = f64::MAX;

    let docker = Arc::new(Docker::connect_with_socket_defaults().unwrap());
    let _cleanup = if n_clients == 1 {
        e2e_testing_setup(docker.clone(), 1).await
    } else {
        e2e_testing_setup(docker.clone(), 2).await
    };

    let mut reader = EventReader::new();
    let _watch = reader.monitor_events_root(PathBuf::from(EVENTS_BASE_DIR));

    let solana_client = Arc::new(SolanaTestClient::new(run_id, None).await);

    tokio::time::sleep(Duration::from_secs(10)).await;

    let chaos_targets = (1..=n_clients)
        .map(|i| format!("{CLIENT_CONTAINER_PREFIX}-{i}"))
        .collect::<Vec<String>>();

    let chaos_scheduler = ChaosScheduler::new(docker.clone(), solana_client);
    chaos_scheduler
        .schedule_chaos(
            ChaosAction::Delay {
                duration_secs: 120,
                latency_ms: 1000,
                targets: chaos_targets,
            },
            delay_step,
        )
        .await;

    let mut liveness_check_interval = time::interval(Duration::from_secs(10));
    println!("Train starting");

    loop {
        tokio::select! {
            _ = liveness_check_interval.tick() => {
                if let Err(e) = check_clients_alive(docker.clone(), n_clients).await {
                    panic!("{}", e);
                }
            }
            msg = reader.recv() => {
                let Some((_, event)) = msg else { break };
                if let EventData::Train(Train::TrainingFinished(train::TrainingFinished { epoch, step, loss })) = event.data {
                    let loss = loss.unwrap();
                    println!("epoch: {epoch}, step: {step}, loss: {loss}");
                    if epoch as i64 > current_epoch {
                        current_epoch = epoch as i64;
                        assert!(loss < last_epoch_loss);
                        last_epoch_loss = loss;
                        if epoch == num_of_epochs_to_run {
                            break;
                        }
                    }
                }
            }
        }
    }
}
