//! `umbriel-link-phone`: a headless phone built on link-core, standing in for the Android app in E2E tests.
//! Every command prints one JSON object per result on stdout.

mod files;
mod flood;
mod held;
mod present;
mod relay;
mod transcript;

use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, bail};
use clap::{Parser, Subcommand, ValueEnum};
use link_core::identity::{DeviceId, Identity};
use link_core::inbox::Inbox;
use link_core::phone::{PairTarget, Phone};
use link_core::proto::CloseCode;
use link_core::proto::message::{Message, Share, ShareKind};
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
    /// Where received files go; default `<state>/Downloads`.
    #[arg(long)]
    downloads: Option<PathBuf>,
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
    /// Stays present with the paired desktop, as the app does in the foreground: keep-alive, redial with backoff.
    /// Prints every event as a JSON line; each stdin line `<text|link> <text>` is shared with the desktop.
    /// Other stdin lines: `send <path>...`, `accept <transfer>`, `decline <transfer>`, `cancel <transfer>`.
    Hold {
        /// Stop after this long; otherwise at SIGTERM or SIGINT.
        #[arg(long)]
        seconds: Option<u64>,
        /// How to answer the desktop's file offers; `ask` waits for an `accept` or `decline` line.
        #[arg(long, value_enum, default_value_t = OnOffer::Ask)]
        on_offer: OnOffer,
        /// The status to report, as `<battery>,<charging 0|1>,<network>`; a `status` line changes it.
        #[arg(long)]
        status: Option<String>,
    },
    /// Hostile: sends `count` messages of one kind back to back and prints what the desktop answered.
    Flood {
        #[arg(long, value_enum)]
        kind: flood::Kind,
        #[arg(long, default_value_t = 50)]
        count: u32,
    },
    /// Sends files to the paired desktop, connecting on demand, and waits for the result.
    SendFile {
        paths: Vec<PathBuf>,
        /// The name the desktop sees, per path in order; `%XX` escapes a byte, so `%00` is NUL.
        #[arg(long = "as-name")]
        names: Vec<String>,
        /// Hostile: send this many bytes past the announced size of the first path (the desktop must auto-accept).
        #[arg(long)]
        oversize: Option<u64>,
    },
    /// Shares text or a link with the paired desktop, connecting on demand.
    Share {
        #[arg(long, value_parser = parse_kind)]
        kind: ShareKind,
        #[arg(long)]
        text: String,
        /// Skip the sender's checks, to prove the desktop enforces them.
        #[arg(long)]
        unchecked: bool,
    },
    /// Posts one notification without the sender's checks, as a hostile phone would, and reports whether the desktop
    /// closed the connection within 10 s. The JSON is `hold`'s `notify` line.
    NotifyUnchecked {
        #[arg(long)]
        json: String,
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
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build()?;
    let result = runtime.block_on(run(cli));
    // `hold`'s stdin reader blocks in read(2) on a thread the runtime would otherwise wait for.
    runtime.shutdown_background();
    result
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
    let inbox = || Inbox::new(cli.downloads.clone().unwrap_or_else(|| state.join("Downloads")), &state);
    match cli.command {
        Command::Hold { seconds, on_offer, status } => {
            let id = only_desktop(&phone)?;
            let status = status.as_deref().map(present::parse_status).transpose()?;
            return present::hold(phone, inbox()?, id, present::Options { seconds, on_offer, status }).await;
        }
        Command::Flood { kind, count } => {
            let id = only_desktop(&phone)?;
            return flood::flood(phone, id, kind, count).await;
        }
        Command::SendFile { paths, names, oversize } => {
            let id = only_desktop(&phone)?;
            let names = files::names(&paths, &names)?;
            if let Some(extra) = oversize {
                return files::send_oversize(phone, id, &paths, &names, extra).await;
            }
            return files::send(phone, inbox()?, id, &paths, names).await;
        }
        Command::Share { kind, text, unchecked } => {
            let id = only_desktop(&phone)?;
            let share = Share { kind, text };
            if unchecked {
                return present::share_unchecked(phone, id, share).await;
            }
            return present::share(phone, inbox()?, id, share).await;
        }
        Command::NotifyUnchecked { json } => {
            let id = only_desktop(&phone)?;
            let posted = held::notification(&serde_json::from_str(&json).context("--json")?)?;
            return present::send_unchecked(phone, id, Message::NotificationPosted(posted)).await;
        }
        _ => {}
    }
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
        let via = present::via_name(session.via);
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

#[derive(Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum OnOffer {
    Accept,
    Decline,
    Ask,
}

fn parse_kind(text: &str) -> Result<ShareKind, String> {
    ShareKind::parse(text).ok_or_else(|| "text or link".to_owned())
}

fn only_desktop(phone: &Phone) -> anyhow::Result<DeviceId> {
    match phone.desktops() {
        [peer] => Ok(peer.id.clone()),
        [] => bail!("not paired"),
        _ => bail!("paired with more than one desktop"),
    }
}
