use anchor_client::solana_sdk::signature::{EncodableKey, Keypair};
use anyhow::{Context, Result, bail};
use clap::Args;
use std::path::PathBuf;

#[derive(Debug, Clone, Args)]
#[command()]
pub struct CommandKeygen {
    /// Path where the new keypair JSON file will be written
    #[clap(short, long)]
    pub output: PathBuf,

    /// Overwrite the output file if it already exists
    #[clap(short, long, default_value_t = false)]
    pub force: bool,
}

impl CommandKeygen {
    /// Generates a new Solana keypair and writes it to the requested path as
    /// a JSON array of 64 bytes, the same format the solana-cli produces
    /// (`solana-keygen new --outfile`), so the file can be dropped into
    /// `WALLET_PATH` / `WALLET_PRIVATE_KEY_PATH` or used with any other
    /// solana tooling.
    pub fn execute(self) -> Result<()> {
        let Self { output, force } = self;

        if output.exists() && !force {
            bail!(
                "output file {} already exists, use --force to overwrite",
                output.display()
            );
        }

        let keypair = Keypair::new();
        keypair
            .write_to_file(&output)
            .with_context(|| format!("Failed to write keypair to {}", output.display()))?;

        println!("Wrote new keypair to {}", output.display());
        println!("Public key: {}", keypair.pubkey());

        Ok(())
    }
}
