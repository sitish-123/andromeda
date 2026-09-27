//! `umbriel-link-phone`: a headless phone built on link-core, standing in for the Android app in E2E tests.
//! Every command prints one JSON object per result on stdout.

mod relay;
mod transcript;

use std::net::SocketAddr;
use std::path::{Path, PathBuf};

use anyhow::{Context, bail};
use clap::{Parser, Subcommand};
use link_core::control::{Control, Tap};
use link_core::identity::{Identity, Spki};
use link_core::pairing::pair_as_client;
use link_core::proto::message::{Hello, Message, PairMethod};
use link_core::proto::pairing::Secret;
use link_core::proto::{CloseCode, VERSION};
use link_core::reach::{self, Via};
use link_core::store::{Peer, Store};
use link_core::tls::{self, ServerPin};
use link_core::transport::Dialer;
use link_core::uri::PairingUri;
use link_core::{Error, close, discovery};
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

struct Phone {
    identity: Identity,
    store: Store,
    devices: PathBuf,
    name: String,
    dialer: Dialer,
    tap: Option<Tap>,
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
    let mut phone = Phone::open(&state, cli.name, cli.transcript.as_deref())?;
    let result = match cli.command {
        Command::Pair { code, uri, addr } => phone.pair(code, uri, addr).await,
        Command::Connect { times, hold } => phone.connect(times, hold).await,
        Command::Unpair => phone.unpair().await,
        _ => unreachable!("handled above"),
    };
    phone.dialer.finish().await;
    result
}

async fn discover(seconds: u64) -> anyhow::Result<()> {
    for desktop in discovery::browse(std::time::Duration::from_secs(seconds), None).await? {
        println!(
            "{}",
            json!({ "id": desktop.id.as_str(), "pairing": desktop.pairing, "addresses": desktop.addresses })
        );
    }
    Ok(())
}

impl Phone {
    fn open(state: &Path, name: String, transcript: Option<&Path>) -> anyhow::Result<Self> {
        std::fs::create_dir_all(state)?;
        let identity = Identity::load_or_create(&state.join("identity.pk8"))?;
        let devices = state.join("devices.json");
        let store = Store::load(&devices)?;
        let dialer = Dialer::new(&identity)?;
        let tap = transcript.map(transcript::tap).transpose()?;
        Ok(Self { identity, store, devices, name, dialer, tap })
    }

    fn hello(&self) -> Hello {
        Hello { version: VERSION, name: self.name.clone(), addresses: Vec::new() }
    }

    async fn pair(&mut self, code: Option<String>, uri: Option<String>, addr: Vec<SocketAddr>) -> anyhow::Result<()> {
        let (candidates, pin, secret, method) = match (code, uri) {
            (Some(code), None) => {
                if code.len() != 6 || !code.bytes().all(|byte| byte.is_ascii_digit()) {
                    bail!("the code is six digits");
                }
                let candidates = if addr.is_empty() { pairing_desktops().await? } else { addr };
                (candidates, ServerPin::Any, Secret::new(code.into_bytes()), PairMethod::Code)
            }
            (None, Some(uri)) => {
                let uri = PairingUri::parse(&uri)?;
                (uri.addresses, ServerPin::Key(uri.fingerprint), Secret::new(uri.secret.to_vec()), PairMethod::Qr)
            }
            _ => bail!("give --code or --uri"),
        };
        let (dialed, addr) = reach::race(&self.dialer, &candidates, pin).await.context("reaching the desktop")?;
        let connection = dialed.connection;
        let mut control = Control::open(&connection, self.tap.clone()).await?;
        let hello = control.hello_as_client(self.hello()).await?;
        pair_as_client(&connection, &mut control, &secret, method, self.identity.spki()).await?;
        let desktop = tls::peer_spki(connection.peer_identity())?;
        let id = self.remember(&desktop, &hello, addr)?;
        close(&connection, CloseCode::Done);
        println!("{}", json!({ "paired": id, "name": hello.name, "addr": addr }));
        Ok(())
    }

    async fn connect(&mut self, times: u32, hold: u64) -> anyhow::Result<()> {
        for attempt in 1..=times {
            let peer = self.desktop()?.clone();
            let reached = match reach::reach(&self.dialer, &peer).await {
                Ok(reached) => reached,
                Err(error) => return self.report_failure(&peer, &error),
            };
            let connection = reached.dialed.connection;
            let mut control = Control::open(&connection, self.tap.clone()).await?;
            let hello = match control.hello_as_client(self.hello()).await {
                Ok(hello) => hello,
                Err(error) => return self.report_failure(&peer, &error),
            };
            self.remember(&peer.spki()?, &hello, reached.addr)?;
            let via = match reached.via {
                Via::LastKnown => "last-known",
                Via::Mdns => "mdns",
            };
            println!(
                "{}",
                json!({ "attempt": attempt, "connected": peer.id, "name": hello.name, "addr": reached.addr, "via": via, "resumed": reached.dialed.resumed })
            );
            tokio::time::sleep(std::time::Duration::from_secs(hold)).await;
            close(&connection, CloseCode::Done);
        }
        Ok(())
    }

    async fn unpair(&mut self) -> anyhow::Result<()> {
        let peer = self.desktop()?.clone();
        let reached = reach::reach(&self.dialer, &peer).await?;
        let connection = reached.dialed.connection;
        let mut control = Control::open(&connection, self.tap.clone()).await?;
        control.hello_as_client(self.hello()).await?;
        control.send(Message::Unpair).await?;
        let closed = connection.closed().await;
        self.forget(&peer)?;
        println!("{}", json!({ "unpaired": peer.id, "desktop_closed": closed.to_string() }));
        Ok(())
    }

    /// A desktop that closed with `unpaired` is forgotten, as the protocol requires.
    fn report_failure(&mut self, peer: &Peer, error: &Error) -> anyhow::Result<()> {
        if matches!(error, Error::Closed(CloseCode::Unpaired)) {
            self.forget(peer)?;
            println!("{}", json!({ "forgotten": peer.id, "reason": "unpaired by the desktop" }));
            return Ok(());
        }
        bail!("reaching {}: {error}", peer.id)
    }

    fn desktop(&self) -> anyhow::Result<&Peer> {
        match self.store.peers.as_slice() {
            [peer] => Ok(peer),
            [] => bail!("not paired"),
            _ => bail!("paired with more than one desktop"),
        }
    }

    fn remember(&mut self, desktop: &Spki, hello: &Hello, addr: SocketAddr) -> anyhow::Result<String> {
        let id = desktop.device_id();
        let mut peer = self.store.peer(&id).cloned().unwrap_or_else(|| Peer::new(desktop, hello.name.clone()));
        peer.name.clone_from(&hello.name);
        peer.remember(addr);
        peer.learn(hello.addresses.iter().filter_map(|text| text.parse().ok()));
        peer.touch();
        self.store.upsert(peer);
        self.store.save(&self.devices)?;
        Ok(id.to_string())
    }

    fn forget(&mut self, peer: &Peer) -> anyhow::Result<()> {
        self.store.remove(&peer.id);
        Ok(self.store.save(&self.devices)?)
    }
}

/// Addresses of the one desktop advertising an open pairing window.
async fn pairing_desktops() -> anyhow::Result<Vec<SocketAddr>> {
    let open: Vec<_> =
        discovery::browse(reach::MDNS_WINDOW, None).await?.into_iter().filter(|desktop| desktop.pairing).collect();
    match open.as_slice() {
        [desktop] => Ok(desktop.addresses.clone()),
        [] => bail!("no desktop is pairing"),
        _ => bail!("more than one desktop is pairing; give --addr"),
    }
}
