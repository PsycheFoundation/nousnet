use crate::commands::Command;
use anchor_client::solana_sdk::pubkey::Pubkey;
use anchor_client::solana_sdk::system_program;
use anyhow::{Result, bail};
use async_trait::async_trait;
use clap::Args;

use psyche_solana_rpc::SolanaBackend;
use psyche_solana_rpc::instructions;

#[derive(Debug, Clone, Args)]
#[command()]
pub struct CommandJoinAuthorizationCreate {
    #[clap(long, env)]
    pub authorizer: Option<Pubkey>,

    /// Make the authorization permissionless (valid for everyone) instead of
    /// granting it to a specific authorizer
    #[clap(long, default_value_t = false)]
    pub permissionless: bool,
}

#[async_trait]
impl Command for CommandJoinAuthorizationCreate {
    async fn execute(self, backend: SolanaBackend) -> Result<()> {
        let Self {
            authorizer,
            permissionless,
        } = self;

        let grantee = match (permissionless, authorizer) {
            (true, Some(_)) => bail!(
                "--permissionless and --authorizer are mutually exclusive: a permissionless \
                 authorization is valid for everyone and cannot target a specific key"
            ),
            (true, None) => system_program::ID,
            (false, None) => bail!(
                "either --authorizer or --permissionless must be provided"
            ),
            (false, Some(authorizer)) => authorizer,
        };

        let payer = backend.get_payer();
        let grantor = backend.get_payer();
        let scope = psyche_solana_coordinator::logic::JOIN_RUN_AUTHORIZATION_SCOPE;

        println!("Authorization Grantor: {}", grantor);
        println!(
            "Authorization Grantee: {}{}",
            grantee,
            if permissionless { " (permissionless)" } else { "" }
        );

        let authorization_address =
            psyche_solana_authorizer::find_authorization(&grantor, &grantee, scope);
        println!("Authorization Address: {}", authorization_address);
        let authorization_lamports = backend.get_balance(&authorization_address).await?;
        println!("Authorization Lamports: {}", authorization_lamports);

        if authorization_lamports == 0 {
            println!(
                "Created authorization in transaction: {}",
                backend
                    .send_and_retry(
                        "Authorization create",
                        &[instructions::authorizer_authorization_create(
                            &payer, &grantor, &grantee, scope,
                        )],
                        &[],
                    )
                    .await?
            );
        }

        let authorization_content = backend.get_authorization(&authorization_address).await?;
        println!("Authorization Active: {}", authorization_content.active);

        if !authorization_content.active {
            println!(
                "Activated authorization in transaction: {}",
                backend
                    .send_and_retry(
                        "Authorization activate",
                        &[instructions::authorizer_authorization_grantor_update(
                            &grantor, &grantee, scope, true
                        )],
                        &[],
                    )
                    .await?
            );
        }

        Ok(())
    }
}
