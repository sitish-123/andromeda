//! `umbriel-quickshare`: receive from or send to Quick Share devices on this network, one JSON object per line.

use std::io::BufRead as _;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::time::Duration;

use anyhow::Context as _;
use clap::{Parser, Subcommand, ValueEnum};
use link_quickshare::advertise::{self, Ble, Mdns};
use link_quickshare::endpoint::{DEVICE_LAPTOP, Endpoint};
use link_quickshare::receive::{self, Outcome, TextKind};
use link_quickshare::send::{self, SendOutcome};
use serde_json::json;
use tokio::net::TcpListener;

#[derive(Parser)]
struct Cli {
    #[arg(long, default_value = "Umbriel")]
    name: String,
    #[command(subcommand)]
    command: Command,
}

#[derive(Clone, Copy, ValueEnum)]
enum Consent {
    Ask,
    Accept,
    Decline,
}

#[derive(Subcommand)]
enum Command {
    /// Advertise and accept incoming shares until interrupted, or for `--once` connection.
    Receive {
        #[arg(long)]
        dir: PathBuf,
        #[arg(long, default_value_t = 0)]
        port: u16,
        #[arg(long, value_enum, default_value = "ask")]
        consent: Consent,
        /// Also send the BLE hint Android needs before it looks for receivers.
        #[arg(long)]
        ble: bool,
        #[arg(long, default_value = "/org/bluez/hci0")]
        adapter: String,
        #[arg(long)]
        once: bool,
    },
    Send {
        #[arg(long, conflicts_with = "to")]
        addr: Option<SocketAddr>,
        /// A receiver's name as it advertises on mDNS.
        #[arg(long)]
        to: Option<String>,
        /// Test only: offer every file under this name.
        #[arg(long, hide = true)]
        offer_as: Option<String>,
        /// Test only: send this many bytes past each file's announced size.
        #[arg(long, hide = true, default_value_t = 0)]
        extra_bytes: usize,
        files: Vec<PathBuf>,
    },
    Discover {
        #[arg(long, default_value_t = 3)]
        seconds: u64,
    },
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> anyhow::Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).format_timestamp(None).init();
    let cli = Cli::parse();
    let own = Endpoint::new(&cli.name, DEVICE_LAPTOP)?;
    match cli.command {
        Command::Receive { dir, port, consent, ble, adapter, once } => {
            receive_loop(&own, &dir, port, consent, ble.then_some(adapter.as_str()), once).await
        }
        Command::Send { addr, to, offer_as, extra_bytes, files } => {
            let addr = match (addr, to) {
                (Some(addr), _) => addr,
                (None, Some(name)) => find(&name).await?,
                (None, None) => anyhow::bail!("give --addr or --to"),
            };
            let hostile = send::Hostile { name: offer_as, extra_bytes };
            let outcome = send::send(addr, &own, &files, &hostile, |pin| println!("{}", json!({ "pin": pin }))).await?;
            match outcome {
                SendOutcome::Sent => println!("{}", json!({ "sent": files.len() })),
                SendOutcome::Rejected(status) => println!("{}", json!({ "rejected": status })),
            }
            Ok(())
        }
        Command::Discover { seconds } => {
            for found in advertise::browse(Duration::from_secs(seconds), None).await? {
                let addresses: Vec<String> = found.addresses.iter().map(ToString::to_string).collect();
                println!("{}", json!({ "name": found.name, "addresses": addresses }));
            }
            Ok(())
        }
    }
}

async fn find(name: &str) -> anyhow::Result<SocketAddr> {
    advertise::browse(Duration::from_secs(5), Some(name))
        .await?
        .into_iter()
        .find(|found| found.name == name)
        .and_then(|found| found.addresses.into_iter().find(SocketAddr::is_ipv4))
        .with_context(|| format!("no receiver named {name:?}"))
}

async fn receive_loop(
    own: &Endpoint,
    dir: &std::path::Path,
    port: u16,
    consent: Consent,
    ble: Option<&str>,
    once: bool,
) -> anyhow::Result<()> {
    let listener = TcpListener::bind(("0.0.0.0", port)).await.context("listen")?;
    let port = listener.local_addr()?.port();
    let _mdns = Mdns::advertise(own, port)?;
    let _ble = match ble {
        Some(adapter) => Some(Ble::advertise(adapter).await.context("BLE advertisement")?),
        None => None,
    };
    println!("{}", json!({ "listening": port, "instance": own.instance_name() }));
    loop {
        let (stream, remote) = listener.accept().await?;
        let result = receive::receive(stream, dir, |offer| async move {
            let files: Vec<_> = offer.files.iter().map(|f| json!({ "name": f.name, "size": f.size })).collect();
            println!("{}", json!({ "offer": { "from": offer.sender, "pin": offer.pin, "files": files } }));
            match consent {
                Consent::Accept => true,
                Consent::Decline => false,
                Consent::Ask => tokio::task::spawn_blocking(|| {
                    eprint!("accept? [y/N] ");
                    let mut line = String::new();
                    std::io::stdin().lock().read_line(&mut line).is_ok() && line.trim().eq_ignore_ascii_case("y")
                })
                .await
                .unwrap_or(false),
            }
        })
        .await;
        match result {
            Ok(Outcome::Received { files, texts }) => {
                let files: Vec<String> = files.iter().map(|p| p.display().to_string()).collect();
                let texts: Vec<_> =
                    texts.iter().map(|(kind, text)| json!({ "kind": kind_str(kind), "text": text })).collect();
                println!("{}", json!({ "received": { "from": remote.to_string(), "files": files, "texts": texts } }));
            }
            Ok(Outcome::Declined) => println!("{}", json!({ "declined": remote.to_string() })),
            Ok(Outcome::Cancelled) => println!("{}", json!({ "cancelled": remote.to_string() })),
            Err(error) => println!("{}", json!({ "failed": remote.to_string(), "error": error.to_string() })),
        }
        if once {
            return Ok(());
        }
    }
}

fn kind_str(kind: &TextKind) -> &'static str {
    match kind {
        TextKind::Text => "text",
        TextKind::Url => "url",
        TextKind::Address => "address",
        TextKind::Phone => "phone",
    }
}
