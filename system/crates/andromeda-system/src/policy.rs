//! The system part's policy state (docs/system-health.md §5-§6): the knob table and rule engine, the explain log,
//! and the actuators that apply a knob's effective value to its backend.

use std::path::PathBuf;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{Result, anyhow};
use tokio::sync::Mutex;
use zbus::Connection;
use zbus::object_server::SignalEmitter;

use crate::engine::{Changes, Engine};
use crate::explain::{self, Log};
use crate::holds::{Knob, KnobValue, Merge, Source};
use crate::power;

pub const PROFILE: &str = "power.profile";

pub struct Policy {
    pub engine: Engine,
    pub log: Log,
    /// Our power-profiles-daemon hold: cookie and profile.
    ppd_hold: Option<(u32, String)>,
}

pub type Shared = Arc<Mutex<Policy>>;

pub fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| u64::try_from(d.as_millis()).unwrap_or(u64::MAX))
}

pub fn new(log: PathBuf) -> Shared {
    let (engine, _) = Engine::new(vec![Knob::new(PROFILE, Merge::LowestPower)], Vec::new(), now_ms());
    Arc::new(Mutex::new(Policy { engine, log: Log::new(log), ppd_hold: None }))
}

impl Policy {
    /// Writes the entries, then applies each moved knob. `user_profile` is set when the user picked a profile: it
    /// becomes `ActiveProfile` first (power-profiles-daemon drops every hold then), and our hold is placed again.
    pub async fn commit(&mut self, conn: &Connection, changes: Changes, user_profile: Option<&str>) -> Result<()> {
        for entry in &changes.entries {
            if let Err(e) = self.log.append(entry) {
                log::warn!("explain log: {e}");
            }
            let emitter = SignalEmitter::new(conn, explain::PATH)?;
            explain::Explain::entry(&emitter, entry.to_dict()).await?;
        }
        if let Some(profile) = user_profile {
            power::set_profile(conn, profile).await?;
            self.ppd_hold = None;
        }
        if user_profile.is_some() || changes.apply.iter().any(|(k, _)| *k == PROFILE) {
            self.apply_profile(conn).await?;
        }
        Ok(())
    }

    async fn apply_profile(&mut self, conn: &Connection) -> Result<()> {
        let Some(knob) = self.engine.knob(PROFILE) else { return Ok(()) };
        let (value, source) = knob.effective();
        let wanted = match (value, source) {
            (Some(KnobValue::Str(p)), Source::Holds(..)) if Some(&p) != knob_baseline(knob).as_ref() => Some(p),
            _ => None,
        };
        if self.ppd_hold.as_ref().map(|(_, p)| p) == wanted.as_ref() {
            return Ok(());
        }
        if let Some((cookie, _)) = self.ppd_hold.take() {
            power::release_profile(conn, cookie).await?;
        }
        if let Some(profile) = wanted {
            if profile == "balanced" {
                return Err(anyhow!("power-profiles-daemon cannot hold balanced"));
            }
            let cookie = power::hold_profile(conn, &profile, "andromeda-system rule").await?;
            self.ppd_hold = Some((cookie, profile));
        }
        Ok(())
    }
}

fn knob_baseline(knob: &Knob) -> Option<String> {
    match &knob.baseline {
        Some(KnobValue::Str(p)) => Some(p.clone()),
        _ => None,
    }
}
