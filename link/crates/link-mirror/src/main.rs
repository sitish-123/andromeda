//! `umbriel-link-mirror`: a phone's screen in a window, from `umbriel-linkd`'s `OpenMirror`, with taps, swipes,
//! scrolls, keys, and Back/Home/Recents going back as `MirrorInput`. `--frames` decodes to PNG files instead, for tests.

mod input;
mod stream;

use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{Context, bail};
use clap::Parser;
use gtk4::prelude::*;
use gtk4::{gio, glib};

const LINK: &str = "org.umbriel.Link1";
const LINK_PATH: &str = "/org/umbriel/Link1";
/// The phone asks the user first; the daemon gives up after 60 s.
const OPEN_TIMEOUT_MS: i32 = 90_000;

#[derive(Parser)]
#[command(about = "Mirror a paired phone's screen")]
struct Cli {
    /// The phone's Link device id.
    device: String,
    /// The window's title.
    #[arg(long, default_value = "Phone")]
    name: String,
    /// Decode into DIR/frame-NNN.png instead of a window, then exit.
    #[arg(long, value_name = "DIR")]
    frames: Option<PathBuf>,
    /// With --frames, how many access units to decode.
    #[arg(long, default_value_t = 60)]
    count: u32,
}

fn main() -> ExitCode {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).format_timestamp(None).init();
    match run(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            log::error!("{error:#}");
            ExitCode::FAILURE
        }
    }
}

fn run(cli: Cli) -> anyhow::Result<()> {
    gstreamer::init()?;
    let bus = gio::bus_get_sync(gio::BusType::Session, None::<&gio::Cancellable>)?;
    let (socket, width, height) = open(&bus, &cli.device)?;
    log::info!("mirroring {} at {width}x{height}", cli.device);
    if let Some(dir) = cli.frames {
        return stream::to_png(socket, &dir, cli.count);
    }
    // The paintable sink needs a GDK display before its pipeline starts.
    gtk4::init()?;
    let display = std::rc::Rc::new(stream::Display::new(socket)?);
    input::keyframe(&bus, &cli.device);
    let app = gtk4::Application::builder().application_id("org.umbriel.LinkMirror").build();
    let (device, name) = (cli.device, cli.name);
    app.connect_activate(move |app| input::window(app, &bus, &device, &name, &display, width, height));
    let status = app.run_with_args::<&str>(&[]);
    if status == glib::ExitCode::SUCCESS { Ok(()) } else { bail!("the window ended with {status:?}") }
}

/// Calls `OpenMirror`, which answers once the phone's user allowed capture.
fn open(bus: &gio::DBusConnection, device: &str) -> anyhow::Result<(std::os::unix::net::UnixStream, u32, u32)> {
    let (reply, fds) = bus
        .call_with_unix_fd_list_sync(
            Some(LINK),
            LINK_PATH,
            LINK,
            "OpenMirror",
            Some(&(device,).to_variant()),
            Some(glib::VariantTy::new("(huu)")?),
            gio::DBusCallFlags::NONE,
            OPEN_TIMEOUT_MS,
            None::<&gio::UnixFDList>,
            None::<&gio::Cancellable>,
        )
        .context("the phone did not start mirroring")?;
    let (handle, width, height) =
        reply.get::<(glib::variant::Handle, u32, u32)>().context("a malformed OpenMirror reply")?;
    let fds = fds.context("OpenMirror passed no descriptor")?;
    let fd = fds.get(handle.0).context("OpenMirror's descriptor is missing")?;
    Ok((std::os::unix::net::UnixStream::from(fd), width, height))
}
