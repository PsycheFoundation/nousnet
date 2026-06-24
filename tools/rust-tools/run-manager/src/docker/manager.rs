use anchor_client::solana_sdk::bs58;
use anchor_client::solana_sdk::pubkey::Pubkey;
use anchor_client::solana_sdk::signature::{EncodableKey, Keypair, Signer};
use anyhow::{Context, Result, anyhow, bail};
use psyche_coordinator::{
    RunState,
    model::{Checkpoint, LLMArchitecture, Model},
};
use std::env;
use std::io::{BufRead, BufReader, Cursor};
#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use tokio::signal;
use tracing::{debug, error, info, warn};

use crate::docker::RunInfo;
use crate::docker::coordinator_client::CoordinatorClient;
use crate::get_env_var;
use crate::load_and_apply_env_file;
use crate::load_wallet_key;

const RETRY_DELAY_SECS: u64 = 5;
const VERSION_MISMATCH_EXIT_CODE: i32 = 10;
const WORKSPACE_CLIENT_VERSION: &str = env!("CARGO_PKG_VERSION");

pub struct RunManager {
    env_file: PathBuf,
    wallet_key: String,
    run_id: String,
    client_launch: ClientLaunch,
    coordinator_client: CoordinatorClient,
    scratch_dir: Option<String>,
    client_authorizer: Pubkey,
}

#[derive(Debug)]
pub enum ClientLaunch {
    Docker {
        local: bool,
    },
    Native {
        client_binary: PathBuf,
        client_version: Option<String>,
        client_args: Vec<String>,
    },
}

#[derive(Debug)]
pub struct Entrypoint {
    pub entrypoint: String,
    pub args: Vec<String>,
}

enum NativeClientExit {
    Code(i32),
    Interrupted,
}

impl RunManager {
    pub fn new(
        coordinator_program_id: String,
        env_file: PathBuf,
        client_launch: ClientLaunch,
        authorizer: Option<Pubkey>,
    ) -> Result<Self> {
        if matches!(client_launch, ClientLaunch::Docker { .. }) {
            // Verify docker is available
            Command::new("docker")
                .arg("--version")
                .output()
                .context("Failed to execute docker command. Is Docker installed and accessible?")?;
        }

        load_and_apply_env_file(&env_file)?;

        let wallet_key = load_wallet_key()?;
        let user_pubkey = parse_wallet_pubkey(&wallet_key)?;
        info!("User pubkey: {}", user_pubkey);

        let coordinator_program_id = coordinator_program_id
            .parse::<Pubkey>()
            .context("Failed to parse coordinator program ID")?;

        info!("Using coordinator program ID: {}", coordinator_program_id);

        let rpc = get_env_var("RPC")?;
        let scratch_dir = std::env::var("SCRATCH_DIR").ok();

        let coordinator_client = CoordinatorClient::new(rpc, coordinator_program_id);

        // Read delegate key from AUTHORIZER env var (separate from --authorizer flag)
        let delegate_authorizer = parse_delegate_authorizer_from_env()?;

        // Try to get RUN_ID from env, or discover available runs
        if let Ok(run_id) = std::env::var("RUN_ID") {
            if !run_id.is_empty() {
                info!("Using RUN_ID from environment: {}", run_id);
                let client_authorizer = resolve_client_authorizer(
                    &coordinator_client,
                    &run_id,
                    &user_pubkey,
                    delegate_authorizer.as_ref(),
                )?;
                return Ok(Self {
                    wallet_key,
                    run_id,
                    coordinator_client,
                    env_file,
                    client_launch,
                    scratch_dir,
                    client_authorizer,
                });
            }
        }

        info!("RUN_ID not set, discovering available runs...");
        let runs = coordinator_client.get_all_runs()?;
        if runs.is_empty() {
            bail!("No runs found on coordinator program");
        }

        let (run_id, client_authorizer) = select_best_run(
            &runs,
            &user_pubkey,
            &coordinator_client,
            authorizer.as_ref(),
            delegate_authorizer.as_ref(),
        )?;

        Ok(Self {
            wallet_key,
            run_id,
            coordinator_client,
            env_file,
            client_launch,
            scratch_dir,
            client_authorizer,
        })
    }

    /// Determine which Docker image to use and pull it if necessary
    async fn prepare_image(&self) -> Result<String> {
        let local = match self.client_launch {
            ClientLaunch::Docker { local } => local,
            ClientLaunch::Native { .. } => unreachable!("native launch does not use Docker image"),
        };
        let docker_tag = self
            .coordinator_client
            .get_docker_tag_for_run(&self.run_id, local)?;
        info!("Docker tag for run '{}': {}", self.run_id, docker_tag);

        if local {
            info!("Using local image (skipping pull): {}", docker_tag);
        } else {
            info!("Pulling image from registry: {}", docker_tag);
            self.pull_image(&docker_tag)?;
        }
        Ok(docker_tag)
    }

    fn pull_image(&self, image_name: &str) -> Result<()> {
        info!("Pulling image: {}", image_name);

        let mut child = Command::new("docker")
            .arg("pull")
            .arg(image_name)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .context("Failed to start docker pull")?;

        // Stream stdout
        if let Some(stdout) = child.stdout.take() {
            let reader = BufReader::new(stdout);
            for line in reader.lines() {
                match line {
                    Ok(line) => println!("{}", line),
                    Err(e) => error!("Error reading stdout: {}", e),
                }
            }
        }

        let status = child.wait().context("Failed to wait for docker pull")?;
        if !status.success() {
            return Err(anyhow!("Docker pull failed with status: {}", status));
        }

        info!("Successfully pulled image: {}", image_name);
        Ok(())
    }

    fn run_container(&self, image_name: &str, entrypoint: &Option<Entrypoint>) -> Result<String> {
        info!("Creating container from image: {}", image_name);

        let client_version = if image_name.contains("sha256:") {
            if matches!(self.client_launch, ClientLaunch::Docker { local: true }) {
                image_name
            } else {
                image_name
                    .split('@')
                    .nth(1)
                    .context("Could not split image name")?
            }
        } else {
            image_name
                .split(':')
                .nth(1)
                .context("Could not split image name")?
        };

        let mut cmd = Command::new("docker");
        cmd.arg("run")
            .arg("-d")
            .arg("--network=host")
            .arg("--shm-size=1g")
            .arg("--privileged")
            .arg("--runtime=nvidia")
            .arg("--gpus=all")
            .arg("--device=/dev/infiniband:/dev/infiniband")
            .arg("--env")
            .arg(format!("RAW_WALLET_PRIVATE_KEY={}", &self.wallet_key))
            .arg("--env")
            .arg(format!("CLIENT_VERSION={}", client_version))
            .arg("--env")
            .arg(format!("RUN_ID={}", &self.run_id))
            .arg("--env")
            .arg(format!("AUTHORIZER={}", &self.client_authorizer))
            .arg("--env-file")
            .arg(&self.env_file);

        if let Some(dir) = &self.scratch_dir {
            cmd.arg("--mount")
                .arg(format!("type=bind,src={dir},dst=/scratch"));
        }

        if let Some(Entrypoint { entrypoint, .. }) = entrypoint {
            cmd.arg("--entrypoint").arg(entrypoint);
        }

        if image_name.contains("sha256:")
            && matches!(self.client_launch, ClientLaunch::Docker { local: true })
        {
            // This is a special case for the local version - for ease of use we just
            // run the container using the ImageId SHA256 instead of a full name
            cmd.arg(client_version);
        } else {
            cmd.arg(image_name);
        }

        if let Some(Entrypoint { args, .. }) = entrypoint {
            cmd.args(args);
        }

        let output = cmd.output().context("Failed to run docker container")?;
        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            return Err(anyhow!("Docker run failed: {}", stderr));
        }

        let container_id = String::from_utf8(output.stdout)
            .context("Failed to parse container ID")?
            .trim()
            .to_string();

        info!("Started container: {}", container_id);
        Ok(container_id)
    }

    async fn stream_logs(&self, container_id: &str) -> Result<()> {
        info!("Streaming logs for container: {}", container_id);

        let mut child = tokio::process::Command::new("docker")
            .arg("logs")
            .arg("-f")
            .arg(container_id)
            .stdout(Stdio::inherit())
            .stderr(Stdio::inherit())
            .kill_on_drop(true)
            .spawn()
            .context("Failed to start docker logs")?;

        let status = child
            .wait()
            .await
            .context("Failed to wait for docker logs")?;
        if !status.success() {
            return Err(anyhow!("Docker logs failed with status: {}", status));
        }

        Ok(())
    }

    fn wait_for_container(&self, container_id: &str) -> Result<i32> {
        let output = Command::new("docker")
            .arg("wait")
            .arg(container_id)
            .output()
            .context("Failed to wait for container")?;

        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            return Err(anyhow!("Docker wait failed: {}", stderr));
        }

        let exit_code_str = String::from_utf8(output.stdout)
            .context("Failed to parse exit code")?
            .trim()
            .to_string();

        let exit_code = exit_code_str
            .parse::<i32>()
            .context("Failed to parse exit code as integer")?;

        Ok(exit_code)
    }

    fn stop_and_remove_container(&self, container_id: &str) -> Result<()> {
        info!("Stopping and removing container: {}", container_id);

        // Stop the container
        let stop_output = Command::new("docker")
            .arg("stop")
            .arg(container_id)
            .output()
            .context("Failed to stop container")?;

        if !stop_output.status.success() {
            let stderr = String::from_utf8_lossy(&stop_output.stderr);
            error!("Warning: Docker stop failed: {}", stderr);
        }

        // Remove the container
        let rm_output = Command::new("docker")
            .arg("rm")
            .arg(container_id)
            .output()
            .context("Failed to remove container")?;

        if !rm_output.status.success() {
            let stderr = String::from_utf8_lossy(&rm_output.stderr);
            error!("Warning: Docker rm failed: {}", stderr);
        }

        Ok(())
    }

    async fn run_native_client(
        &self,
        client_binary: &PathBuf,
        client_version: &Option<String>,
        client_args: &[String],
        entrypoint: &Option<Entrypoint>,
    ) -> Result<NativeClientExit> {
        if entrypoint.is_some() {
            bail!("--entrypoint is only supported in Docker mode");
        }
        ensure_native_client_binary(client_binary)?;

        let (expected_version, native_client_version) =
            self.preflight_native_client(client_version, client_args)?;
        info!(
            "Coordinator expects client version '{}' for run '{}'",
            expected_version, self.run_id
        );

        let mut cmd = tokio::process::Command::new(client_binary);
        cmd.arg("train")
            .args(client_args)
            .env("RAW_WALLET_PRIVATE_KEY", &self.wallet_key)
            .env("RUN_ID", &self.run_id)
            .env("AUTHORIZER", self.client_authorizer.to_string())
            .stdout(Stdio::inherit())
            .stderr(Stdio::inherit())
            .kill_on_drop(true);

        #[cfg(target_os = "macos")]
        {
            cmd.env("PYTORCH_ENABLE_MPS_FALLBACK", "1");
        }

        if let Some(client_version) = native_client_version {
            info!("Using native CLIENT_VERSION={}", client_version);
            cmd.env("CLIENT_VERSION", client_version);
        } else {
            warn!(
                "No native client version was provided. The client will warn and skip coordinator version validation."
            );
        }

        info!(
            "Starting native Psyche client: {} train {}",
            client_binary.display(),
            client_args.join(" ")
        );
        let mut child = cmd.spawn().with_context(|| {
            format!(
                "Failed to start native Psyche client '{}'",
                client_binary.display()
            )
        })?;

        let (status, interrupted) = tokio::select! {
            status = child.wait() => {
                (status.context("Failed to wait for native Psyche client")?, false)
            },
            _ = signal::ctrl_c() => {
                info!("\nReceived interrupt signal, stopping native client...");
                child.start_kill().context("Failed to stop native Psyche client")?;
                (
                    child.wait().await.context("Failed to wait for stopped native Psyche client")?,
                    true,
                )
            }
        };

        if interrupted {
            Ok(NativeClientExit::Interrupted)
        } else {
            Ok(NativeClientExit::Code(status.code().unwrap_or(1)))
        }
    }

    fn preflight_native_client(
        &self,
        client_version: &Option<String>,
        client_args: &[String],
    ) -> Result<(String, Option<String>)> {
        let run_info = self.coordinator_client.get_run_client_info(&self.run_id)?;
        #[allow(unreachable_patterns)]
        let llm = match run_info.model {
            Model::LLM(llm) => llm,
            _ => bail!("Native client mode only supports LLM runs"),
        };

        if matches!(llm.checkpoint, Checkpoint::Ephemeral) {
            bail!("Native client mode cannot join ephemeral-checkpoint runs");
        }

        let device = client_arg_value(client_args, "--device")
            .or_else(|| env::var("DEVICE").ok())
            .unwrap_or_else(|| "auto".to_string());
        let dp =
            client_arg_usize(client_args, "--data-parallelism", "DATA_PARALLELISM")?.unwrap_or(1);
        let tp = client_arg_usize(client_args, "--tensor-parallelism", "TENSOR_PARALLELISM")?
            .unwrap_or(1);
        let device_class = classify_native_device(&device)?;
        let enforce_non_cuda_limits = device_class == NativeDeviceClass::NonCuda;

        if enforce_non_cuda_limits && dp.saturating_mul(tp) > 1 {
            bail!(
                "Native non-CUDA devices currently support only one local rank. Use -- --data-parallelism 1 --tensor-parallelism 1 or select a CUDA device."
            );
        }

        match llm.architecture {
            LLMArchitecture::Torchtitan if enforce_non_cuda_limits => {
                bail!(
                    "Torchtitan runs currently require CUDA; native silicon mode cannot join this run."
                )
            }
            LLMArchitecture::HfAuto if enforce_non_cuda_limits => {
                warn!(
                    "HfAuto on non-CUDA native devices requires a client binary built with the python feature and supports only single-rank training."
                );
            }
            LLMArchitecture::HfLlama | LLMArchitecture::HfDeepseek | LLMArchitecture::HfAuto => {}
            LLMArchitecture::Torchtitan => {}
        }

        let native_client_version =
            client_version
                .clone()
                .or_else(|| infer_native_client_version(&run_info.client_version))
                .ok_or_else(|| {
                    anyhow!(
                        "Coordinator requires client version '{}', but the local workspace package version is '{}'. Pass --native-client-version {} only if this native client binary is compatible with the run.",
                        run_info.client_version,
                        WORKSPACE_CLIENT_VERSION,
                        run_info.client_version
                    )
                })?;

        Ok((run_info.client_version, Some(native_client_version)))
    }

    pub async fn run(&self, entrypoint: Option<Entrypoint>) -> Result<()> {
        loop {
            if let ClientLaunch::Native {
                client_binary,
                client_version,
                client_args,
            } = &self.client_launch
            {
                let exit_code = self
                    .run_native_client(client_binary, client_version, client_args, &entrypoint)
                    .await?;
                let NativeClientExit::Code(exit_code) = exit_code else {
                    info!("Native client interrupted, shutting down");
                    return Ok(());
                };
                if exit_code == VERSION_MISMATCH_EXIT_CODE {
                    bail!(
                        "Native client exited with version mismatch. Rebuild or select a client binary compatible with run '{}'.",
                        self.run_id
                    );
                }
                if exit_code != 0 {
                    bail!("Native client exited with code {}", exit_code);
                }
                info!(
                    "Native client exited with code {}, shutting down",
                    exit_code
                );
                return Ok(());
            }

            let docker_tag = self.prepare_image().await?;
            info!("Starting container...");

            let start_time = tokio::time::Instant::now();
            let container_id = self.run_container(&docker_tag, &entrypoint)?;

            // Race between container completion and Ctrl+C
            let exit_code = tokio::select! {
                result = async {
                        self.stream_logs(&container_id).await?;
                        self.wait_for_container(&container_id)
                } => {
                    result?
                },
                _ = signal::ctrl_c() => {
                    info!("\nReceived interrupt signal, cleaning up container...");
                    self.stop_and_remove_container(&container_id)?;
                    info!("Container stopped successfully");
                    return Ok(());
                }
            };

            let duration = start_time.elapsed().as_secs();
            info!(
                "Container exited with code: {} after {} seconds",
                exit_code, duration
            );

            self.stop_and_remove_container(&container_id)?;

            // Only retry on version mismatch (exit code 10)
            if exit_code == VERSION_MISMATCH_EXIT_CODE {
                warn!("Version mismatch detected, re-checking coordinator for new version...");
                info!("Waiting {} seconds before retry...", RETRY_DELAY_SECS);
                tokio::time::sleep(tokio::time::Duration::from_secs(RETRY_DELAY_SECS)).await;
            } else {
                info!("Container exited with code {}, shutting down", exit_code);
                return Ok(());
            }
        }
    }
}

/// Parse wallet key string to extract the user's pubkey.
pub fn parse_wallet_pubkey(wallet_key: &str) -> Result<Pubkey> {
    let keypair = if wallet_key.starts_with('[') {
        // Assume Keypair::read format (JSON array of bytes)
        Keypair::read(&mut Cursor::new(wallet_key))
            .map_err(|e| anyhow!("Failed to parse wallet key: {}", e))?
    } else {
        // from_base58_string has an internal unwrap() so we use these functions to handle
        // errors more gracefuly
        let decoded = bs58::decode(wallet_key)
            .into_vec()
            .map_err(|e| anyhow!("Failed to decode base58 wallet key: {}", e))?;

        Keypair::from_bytes(&decoded)
            .map_err(|e| anyhow!("Failed to create keypair from decoded bytes: {}", e))?
    };
    Ok(keypair.pubkey())
}

/// Read the AUTHORIZER env var as a delegate key pubkey, if set.
pub fn parse_delegate_authorizer_from_env() -> Result<Option<Pubkey>> {
    match std::env::var("AUTHORIZER") {
        Ok(val) if !val.is_empty() => {
            let pubkey = val.parse::<Pubkey>().with_context(|| {
                format!("Failed to parse AUTHORIZER env var as pubkey: {}", val)
            })?;
            info!(
                "Using delegate authorizer from AUTHORIZER env var: {}",
                pubkey
            );
            Ok(Some(pubkey))
        }
        _ => {
            info!("AUTHORIZER env var not set, skipping delegate key authorization");
            Ok(None)
        }
    }
}

/// Determine the correct AUTHORIZER value for the client container by checking
/// which authorization type (permissionless, user-specific, or delegate) is valid for this run.
fn resolve_client_authorizer(
    coordinator_client: &CoordinatorClient,
    run_id: &str,
    user_pubkey: &Pubkey,
    delegate_authorizer: Option<&Pubkey>,
) -> Result<Pubkey> {
    let Some(grantee) =
        coordinator_client.can_user_join_run(run_id, user_pubkey, delegate_authorizer)?
    else {
        bail!(
            "User {} is not authorized to join run {}",
            user_pubkey,
            run_id
        );
    };

    info!("Resolved AUTHORIZER={} for run {}", grantee, run_id);
    Ok(grantee)
}

/// Filter runs to only those that are joinable and authorized for the given user.
/// Returns (run_info, grantee_pubkey) pairs sorted by priority (WaitingForMembers first).
///
/// - `join_authority_filter`: if set, only consider runs whose join_authority matches this pubkey
/// - `delegate_authorizer`: if set, also try delegate-key authorization via this pubkey
pub fn find_joinable_runs(
    runs: &[RunInfo],
    user_pubkey: &Pubkey,
    coordinator_client: &CoordinatorClient,
    join_authority_filter: Option<&Pubkey>,
    delegate_authorizer: Option<&Pubkey>,
) -> Result<Vec<(RunInfo, Pubkey)>> {
    // Filter out unjoinable run states
    let mut candidates: Vec<_> = runs
        .iter()
        .filter(|run| {
            !matches!(
                run.run_state,
                RunState::Uninitialized | RunState::Finished | RunState::Paused
            )
        })
        .cloned()
        .collect();

    if candidates.is_empty() {
        return Ok(Vec::new());
    }

    // Filter by join_authority if specified
    if let Some(auth) = join_authority_filter {
        info!("Filtering runs by join_authority: {}", auth);
        candidates.retain(
            |run| match coordinator_client.fetch_coordinator_data(&run.run_id) {
                Ok(data) => data.join_authority == *auth,
                Err(e) => {
                    debug!("Skipping run {} - failed to fetch data: {}", run.run_id, e);
                    false
                }
            },
        );
    }

    // Filter to runs the user is authorized to join, capturing the matched grantee
    let mut authorized_candidates: Vec<(RunInfo, Pubkey)> = Vec::new();
    for run in candidates {
        match coordinator_client.can_user_join_run(&run.run_id, user_pubkey, delegate_authorizer) {
            Ok(Some(grantee)) => authorized_candidates.push((run, grantee)),
            Ok(None) => {}
            Err(e) => {
                debug!(
                    "Skipping run {} - authorization check failed: {}",
                    run.run_id, e
                );
            }
        }
    }

    // Prioritize runs waiting for members
    authorized_candidates.sort_by_key(|(run, _)| match run.run_state {
        RunState::WaitingForMembers => 0,
        _ => 1,
    });

    Ok(authorized_candidates)
}

/// Returns (run_id, client_authorizer) where client_authorizer is the grantee
/// to pass to the container as AUTHORIZER.
fn select_best_run(
    runs: &[RunInfo],
    user_pubkey: &Pubkey,
    coordinator_client: &CoordinatorClient,
    join_authority_filter: Option<&Pubkey>,
    delegate_authorizer: Option<&Pubkey>,
) -> Result<(String, Pubkey)> {
    let authorized_candidates = find_joinable_runs(
        runs,
        user_pubkey,
        coordinator_client,
        join_authority_filter,
        delegate_authorizer,
    )?;

    if authorized_candidates.is_empty() {
        bail!("No joinable runs found for user {}", user_pubkey);
    }

    println!("Found {} available run(s):", authorized_candidates.len());
    let candidate_runs: Vec<_> = authorized_candidates.iter().map(|(r, _)| r).collect();
    for line in RunInfo::format_table(&candidate_runs) {
        println!("{}", line);
    }

    let (selected_run, grantee) = &authorized_candidates[0];
    println!(
        "Selected run: {} ({}, {})",
        selected_run.run_id,
        selected_run.run_state,
        selected_run.clients_display()
    );
    info!(
        "Resolved AUTHORIZER={} for run {}",
        grantee, selected_run.run_id
    );

    Ok((selected_run.run_id.clone(), *grantee))
}

fn client_arg_value(client_args: &[String], flag: &str) -> Option<String> {
    let flag_with_equals = format!("{flag}=");
    for (index, arg) in client_args.iter().enumerate() {
        if arg == flag {
            return client_args.get(index + 1).cloned();
        }
        if let Some(value) = arg.strip_prefix(&flag_with_equals) {
            return Some(value.to_string());
        }
    }
    None
}

fn client_arg_usize(client_args: &[String], flag: &str, env_var: &str) -> Result<Option<usize>> {
    client_arg_value(client_args, flag)
        .or_else(|| env::var(env_var).ok())
        .map(|value| {
            value
                .parse::<usize>()
                .with_context(|| format!("Failed to parse {flag}/{env_var} value '{value}'"))
        })
        .transpose()
}

fn ensure_native_client_binary(client_binary: &Path) -> Result<()> {
    let metadata = std::fs::metadata(client_binary).with_context(|| {
        format!(
            "Native client binary '{}' does not exist",
            client_binary.display()
        )
    })?;
    if !metadata.is_file() {
        bail!(
            "Native client path '{}' is not a file",
            client_binary.display()
        );
    }

    #[cfg(unix)]
    if metadata.permissions().mode() & 0o111 == 0 {
        bail!(
            "Native client binary '{}' is not executable",
            client_binary.display()
        );
    }

    Ok(())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum NativeDeviceClass {
    Cuda,
    NonCuda,
}

fn classify_native_device(device: &str) -> Result<NativeDeviceClass> {
    let device = device.trim().to_ascii_lowercase();
    match device.as_str() {
        "" | "auto" => Ok(default_auto_device_class()),
        "cpu" | "mps" => Ok(NativeDeviceClass::NonCuda),
        "cuda" => Ok(NativeDeviceClass::Cuda),
        device if device.starts_with("cuda:") => {
            let ids = device
                .strip_prefix("cuda:")
                .expect("starts_with cuda: means strip_prefix succeeds");
            if ids.is_empty() {
                bail!("invalid native client device '{device}'");
            }
            for id in ids.split(',') {
                id.trim()
                    .parse::<usize>()
                    .with_context(|| format!("invalid CUDA device id '{id}' in '{device}'"))?;
            }
            Ok(NativeDeviceClass::Cuda)
        }
        _ => bail!(
            "invalid native client device '{}'. Expected auto, cpu, mps, cuda, or cuda:N[,M...]",
            device
        ),
    }
}

fn default_auto_device_class() -> NativeDeviceClass {
    #[cfg(target_os = "macos")]
    {
        NativeDeviceClass::NonCuda
    }

    #[cfg(not(target_os = "macos"))]
    {
        if has_obvious_nvidia_device() {
            NativeDeviceClass::Cuda
        } else {
            NativeDeviceClass::NonCuda
        }
    }
}

#[cfg(not(target_os = "macos"))]
fn has_obvious_nvidia_device() -> bool {
    if Path::new("/dev/nvidiactl").exists() || Path::new("/dev/nvidia0").exists() {
        return true;
    }

    Command::new("nvidia-smi")
        .arg("-L")
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false)
}

fn infer_native_client_version(expected_version: &str) -> Option<String> {
    if expected_version == WORKSPACE_CLIENT_VERSION
        || expected_version == format!("v{WORKSPACE_CLIENT_VERSION}")
    {
        Some(expected_version.to_string())
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn client_arg_value_reads_space_and_equals_forms() {
        let args = vec![
            "--device".to_string(),
            "mps".to_string(),
            "--data-parallelism=2".to_string(),
        ];

        assert_eq!(client_arg_value(&args, "--device").as_deref(), Some("mps"));
        assert_eq!(
            client_arg_value(&args, "--data-parallelism").as_deref(),
            Some("2")
        );
        assert_eq!(client_arg_value(&args, "--tensor-parallelism"), None);
    }

    #[test]
    fn classify_native_devices() {
        assert_eq!(
            classify_native_device("cpu").unwrap(),
            NativeDeviceClass::NonCuda
        );
        assert_eq!(
            classify_native_device("mps").unwrap(),
            NativeDeviceClass::NonCuda
        );
        assert_eq!(
            classify_native_device("cuda").unwrap(),
            NativeDeviceClass::Cuda
        );
        assert_eq!(
            classify_native_device("cuda:0,2").unwrap(),
            NativeDeviceClass::Cuda
        );
        assert!(classify_native_device("cuda:apple").is_err());
        assert!(classify_native_device("cuda-but-not-really").is_err());
    }

    #[test]
    fn classify_auto_matches_platform_default() {
        #[cfg(target_os = "macos")]
        assert_eq!(
            classify_native_device("auto").unwrap(),
            NativeDeviceClass::NonCuda
        );

        #[cfg(not(target_os = "macos"))]
        assert_eq!(
            classify_native_device("auto").unwrap(),
            default_auto_device_class()
        );
    }

    #[test]
    fn infer_native_version_only_for_workspace_version() {
        assert_eq!(
            infer_native_client_version(WORKSPACE_CLIENT_VERSION).as_deref(),
            Some(WORKSPACE_CLIENT_VERSION)
        );
        let tagged_version = format!("v{WORKSPACE_CLIENT_VERSION}");
        assert_eq!(
            infer_native_client_version(&tagged_version).as_deref(),
            Some(tagged_version.as_str())
        );
        assert_eq!(infer_native_client_version("sha256:abc123"), None);
    }
}
