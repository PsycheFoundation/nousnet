use std::{sync::Arc, time::Duration};

use psyche_core::IntegrationTestLogMarker;
use psyche_decentralized_testing::{
    CLIENT_CONTAINER_PREFIX, VALIDATOR_CONTAINER_PREFIX,
    chaos::{ChaosAction, ChaosScheduler, run_chaos_loss_loop},
    docker_setup::setup_test,
    utils::SolanaTestClient,
};

use rstest::*;
use serial_test::serial;

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
    let (docker, mut watcher, _cleanup) = setup_test(n_clients as usize).await;
    let solana_client = Arc::new(SolanaTestClient::new(run_id, None).await);
    let _monitors = watcher
        .monitor_clients(n_clients, vec![IntegrationTestLogMarker::Loss])
        .unwrap();

    tokio::time::sleep(Duration::from_secs(10)).await;

    let chaos_targets = vec![format!("{VALIDATOR_CONTAINER_PREFIX}-1")];
    let chaos_scheduler = ChaosScheduler::new(docker.clone(), solana_client);
    chaos_scheduler
        .schedule_chaos(
            ChaosAction::Pause {
                duration_secs: 60,
                targets: chaos_targets.clone(),
            },
            pause_step,
        )
        .await;

    println!("Train starting");
    run_chaos_loss_loop(&mut watcher, n_clients, 2).await;
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
    let (docker, mut watcher, _cleanup) = setup_test(n_clients as usize).await;
    let solana_client = Arc::new(SolanaTestClient::new(run_id, None).await);
    let _monitors = watcher
        .monitor_clients(n_clients, vec![IntegrationTestLogMarker::Loss])
        .unwrap();

    tokio::time::sleep(Duration::from_secs(10)).await;

    let chaos_targets = vec![format!("{VALIDATOR_CONTAINER_PREFIX}-1")];
    let chaos_scheduler = ChaosScheduler::new(docker.clone(), solana_client);
    chaos_scheduler
        .schedule_chaos(
            ChaosAction::Delay {
                duration_secs: 120,
                latency_ms: delay_milis,
                targets: chaos_targets.clone(),
            },
            delay_step,
        )
        .await;

    println!("Train starting");
    run_chaos_loss_loop(&mut watcher, n_clients, 2).await;
}

#[ignore = "These tests are a bit flaky, so we need to make sure they work properly."]
#[rstest]
#[trace]
#[test_log::test(tokio::test(flavor = "multi_thread"))]
#[serial]
async fn test_delay_solana_client(#[values(1, 2)] n_clients: u8, #[values(0, 10)] delay_step: u64) {
    let run_id = "test".to_string();
    let (docker, mut watcher, _cleanup) = setup_test(n_clients as usize).await;
    let solana_client = Arc::new(SolanaTestClient::new(run_id, None).await);
    let _monitors = watcher
        .monitor_clients(n_clients, vec![IntegrationTestLogMarker::Loss])
        .unwrap();

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
                targets: chaos_targets.clone(),
            },
            delay_step,
        )
        .await;

    println!("Train starting");
    run_chaos_loss_loop(&mut watcher, n_clients, 2).await;
}
