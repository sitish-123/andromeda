//! `umbriel-link-phone`: a headless phone built on link-core, standing in for the Android app in E2E tests.
//! Every command prints one JSON object per result on stdout.

mod relay;
mod transcript;

use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, bail};
use clap::{Parser, Subcommand};
use link_core::identity::{DeviceId, Identity};
use link_core::phone::{PairTarget, Phone};
use link_core::proto::CloseCode;
use link_core::reach::Via;
use link_core::uri::PairingUri;
use link_core::{Error, discovery};
use serde_json::json;

#[derive(Parser)]
#[command(about = "Headless Link phone for end-to-end tests")]
struct Cli {
    /// Directory holding this phone's key and paired desktops; needed by pair, connect, and unpair.
    #[arg(long)]
    state: Option<PathBuf>,
    #[arg(long, default_value = "Headless phone")]
    name: String,
    /// Appends every control message as a JSON line with its CBOR in hex.
    #[arg(long)]
    transcript: Option<PathBuf>,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Pairs with a desktop by typed code (found by mDNS unless --addr is given) or by QR URI.
    Pair {
        #[arg(long, conflicts_with = "uri")]
        code: Option<String>,
        #[arg(long)]
        uri: Option<String>,
        #[arg(long)]
        addr: Vec<SocketAddr>,
    },
    /// Reaches the paired desktop `times` times, holding each session for `hold` seconds.
    Connect {
        #[arg(long, default_value_t = 1)]
        times: u32,
        #[arg(long, default_value_t = 0)]
        hold: u64,
    },
    /// Tells the paired desktop this phone unpaired, then forgets it.
    Unpair,
    /// Lists desktops answering mDNS.
    Discover {
        #[arg(long, default_value_t = 3)]
        seconds: u64,
    },
    /// Validates every message of a transcript against a CDDL schema.
    CheckTranscript { schema: PathBuf, transcript: PathBuf },
    /// A relay that terminates TLS on both legs and forwards the control stream: the MITM pairing must defeat.
    Relay {
        #[arg(long)]
        listen: SocketAddr,
        #[arg(long)]
        target: SocketAddr,
    },
}

fn main() -> anyhow::Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("warn")).init();
    let cli = Cli::parse();
    tokio::runtime::Builder::new_current_thread().enable_all().build()?.block_on(run(cli))
}

async fn run(cli: Cli) -> anyhow::Result<()> {
    match cli.command {
        Command::Discover { seconds } => return discover(seconds).await,
        Command::CheckTranscript { schema, transcript } => return transcript::check(&schema, &transcript),
        Command::Relay { listen, target } => return relay::run(listen, target).await,
        _ => {}
    }
    let state = cli.state.context("--state is required for this command")?;
    let mut phone = open(&state, cli.name, cli.transcript.as_deref())?;
    let result = match cli.command {
        Command::Pair { code, uri, addr } => pair(&mut phone, code, uri, addr).await,
        Command::Connect { times, hold } => connect(&mut phone, times, hold).await,
        Command::Unpair => unpair(&mut phone).await,
        _ => unreachable!("handled above"),
    };
    phone.finish().await;
    result
}

fn open(state: &Path, name: String, transcript: Option<&Path>) -> anyhow::Result<Phone> {
    std::fs::create_dir_all(state)?;
    let identity = Identity::load_or_create(&state.join("identity.pk8"))?;
    let tap = transcript.map(transcript::tap).transpose()?;
    Ok(Phone::new(identity, state.join("devices.json"), name, tap)?)
}

async fn discover(seconds: u64) -> anyhow::Result<()> {
    for desktop in discovery::browse(Duration::from_secs(seconds), None).await? {
        println!(
            "{}",
            json!({ "id": desktop.id.as_str(), "pairing": desktop.pairing, "addresses": desktop.addresses })
        );
    }
    Ok(())
}

async fn pair(
    phone: &mut Phone,
    code: Option<String>,
    uri: Option<String>,
    addr: Vec<SocketAddr>,
) -> anyhow::Result<()> {
    let target = match (code, uri) {
        (Some(code), None) => PairTarget::Code { code, candidates: addr },
        (None, Some(uri)) => PairTarget::Uri(PairingUri::parse(&uri)?),
        _ => bail!("give --code or --uri"),
    };
    let session = phone.pair(target).await.context("pairing")?;
    session.close();
    println!("{}", json!({ "paired": session.desktop.id, "name": session.desktop.name, "addr": session.addr }));
    Ok(())
}

async fn connect(phone: &mut Phone, times: u32, hold: u64) -> anyhow::Result<()> {
    let id = only_desktop(phone)?;
    for attempt in 1..=times {
        let session = match phone.connect(&id).await {
            Ok(session) => session,
            Err(Error::Closed(CloseCode::Unpaired)) => {
                println!("{}", json!({ "forgotten": id, "reason": "unpaired by the desktop" }));
                return Ok(());
            }
            Err(error) => bail!("reaching {id}: {error}"),
        };
        let via = match session.via {
            Via::LastKnown => "last-known",
            Via::Mdns => "mdns",
        };
        let line = json!({
            "attempt": attempt, "connected": id, "name": session.desktop.name, "addr": session.addr, "via": via,
            "resumed": session.resumed,
        });
        println!("{line}");
        tokio::time::sleep(Duration::from_secs(hold)).await;
        session.close();
    }
    Ok(())
}

async fn unpair(phone: &mut Phone) -> anyhow::Result<()> {
    let id = only_desktop(phone)?;
    let told = phone.unpair(&id).await?;
    println!("{}", json!({ "unpaired": id, "desktop_told": told }));
    Ok(())
}

fn only_desktop(phone: &Phone) -> anyhow::Result<DeviceId> {
    match phone.desktops() {
        [peer] => Ok(peer.id.clone()),
        [] => bail!("not paired"),
        _ => bail!("paired with more than one desktop"),
    }
}
