//! The phone as the desktop's remote (`link/ARCHITECTURE.md`, Control): the commands the desktop lets phones run.

use std::fmt::Write as _;
use std::process::{Command as Process, Stdio};

use link_core::proto::message::{Command, Commands, MAX_COMMANDS, MAX_TITLE_LEN};
use link_core::store::StoredCommand;

/// The first 8 bytes of SHA-256 over the name and the command line, so a phone's older list never runs an edited
/// command.
pub fn id(stored: &StoredCommand) -> String {
    let digest = ring::digest::digest(&ring::digest::SHA256, format!("{}\0{}", stored.name, stored.command).as_bytes());
    digest.as_ref()[..8].iter().fold(String::new(), |mut hex, byte| {
        let _ = write!(hex, "{byte:02x}");
        hex
    })
}

pub fn listing(commands: &[StoredCommand]) -> Commands {
    Commands { commands: commands.iter().map(|stored| Command { id: id(stored), name: stored.name.clone() }).collect() }
}

pub fn valid(commands: &[StoredCommand]) -> bool {
    commands.len() <= MAX_COMMANDS
        && commands.iter().all(|stored| (1..=MAX_TITLE_LEN).contains(&stored.name.len()) && !stored.command.is_empty())
}

/// Runs the command line under `sh -c`, detached from the daemon's output.
pub fn run(stored: &StoredCommand) -> std::io::Result<()> {
    let mut child = Process::new("sh")
        .arg("-c")
        .arg(&stored.command)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()?;
    std::thread::spawn(move || child.wait());
    Ok(())
}
