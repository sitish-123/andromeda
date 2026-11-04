//! `/org/andromeda/System1/Knobs`: machine-wide knobs (docs/system-health.md §5). `Set` is the user's choice;
//! `Hold` and `Release` are for clients that run rules (the session part). Per-device choices are methods on the
//! device (`Battery.SetChargeLimit`, `KbdBacklight.SetBrightness`).

use futures_util::StreamExt;
use zbus::fdo::{DBusProxy, Error, Result};
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::{Connection, OwnedVariant, Variant, interface};

use crate::holds::{KnobValue, Merge};
use crate::policy::{self, PROFILE, Shared};
use crate::polkit;

pub const PATH: &str = "/org/andromeda/System1/Knobs";

pub struct Knobs {
    pub policy: Shared,
}

fn failed(e: &anyhow::Error) -> Error {
    Error::Failed(format!("{e:#}"))
}

fn action(knob: &str) -> Result<&'static str> {
    match knob {
        PROFILE => Ok("org.andromeda.system.power-profile"),
        _ => Err(Error::InvalidArgs(format!("unknown knob {knob:?}"))),
    }
}

fn sender(header: &Header<'_>) -> Result<String> {
    header.sender().map(ToString::to_string).ok_or_else(|| Error::Failed("no sender".into()))
}

/// A D-Bus value as a rule value: s, b, numbers, as.
fn to_toml(value: OwnedVariant) -> Option<toml::Value> {
    if let Ok(s) = String::try_from(value.try_clone().ok()?) {
        return Some(toml::Value::String(s));
    }
    if let Ok(b) = bool::try_from(&value) {
        return Some(toml::Value::Boolean(b));
    }
    if let Ok(d) = f64::try_from(&value) {
        return Some(toml::Value::Float(d));
    }
    if let Ok(i) = i32::try_from(&value) {
        return Some(toml::Value::Integer(i.into()));
    }
    if let Ok(u) = u32::try_from(&value) {
        return Some(toml::Value::Integer(u.into()));
    }
    let items = Vec::<String>::try_from(value).ok()?;
    Some(toml::Value::Array(items.into_iter().map(toml::Value::String).collect()))
}

#[interface(name = "org.andromeda.System1.Knobs")]
impl Knobs {
    /// The user's choice for a knob, stored by its backend:
    /// `power.profile` (s): power-saver, balanced or performance. It undoes each automatic episode that holds the
    /// knob; safety holds still win.
    async fn set(
        &self,
        knob: &str,
        value: OwnedVariant,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> Result<()> {
        polkit::check(conn, &header, action(knob)?).await?;
        let bad = || Error::InvalidArgs("power.profile takes power-saver, balanced or performance".into());
        let profile = String::try_from(value).map_err(|_| bad())?;
        let choice = KnobValue::from_toml(&toml::Value::String(profile.clone()), Merge::LowestPower).ok_or_else(bad)?;
        let mut policy = self.policy.lock().await;
        let changes = policy
            .engine
            .set_baseline(knob, choice, policy::now_ms())
            .ok_or_else(|| Error::InvalidArgs(format!("unknown knob {knob:?}")))?;
        let released = changes.released.clone();
        policy.commit(conn, changes, Some(&profile)).await.map_err(|e| failed(&e))?;
        let emitter = SignalEmitter::new(conn, PATH)?;
        for (id, owner) in released {
            Self::released(&emitter, &id.to_string(), &owner).await?;
        }
        Ok(())
    }

    /// A user change ended the client hold `id` of `owner`.
    #[zbus(signal)]
    async fn released(emitter: &SignalEmitter<'_>, id: &str, owner: &str) -> zbus::Result<()>;

    /// Holds `knob` at `value` for the rule `reason` until `Release` or until the caller leaves the bus. Client
    /// holds are automatic: the user's next `Set` of the knob ends them (`Released`). Returns the hold id.
    async fn hold(
        &self,
        knob: &str,
        value: OwnedVariant,
        reason: &str,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> Result<String> {
        polkit::check(conn, &header, action(knob)?).await?;
        let value = to_toml(value).ok_or_else(|| Error::InvalidArgs("value is s, b, a number or as".into()))?;
        let owner = sender(&header)?;
        let mut policy = self.policy.lock().await;
        let (id, changes) =
            policy.engine.hold(knob, &owner, reason, &value, policy::now_ms()).map_err(Error::InvalidArgs)?;
        policy.commit(conn, changes, None).await.map_err(|e| failed(&e))?;
        Ok(id.to_string())
    }

    /// Ends one of the caller's holds.
    async fn release(&self, id: &str, #[zbus(header)] header: Header<'_>, #[zbus(connection)] conn: &Connection) -> Result<()> {
        let id = id.parse().map_err(|_| Error::InvalidArgs(format!("bad hold id {id:?}")))?;
        let owner = sender(&header)?;
        let mut policy = self.policy.lock().await;
        let changes = policy.engine.release(&owner, Some(id));
        policy.commit(conn, changes, None).await.map_err(|e| failed(&e))
    }

    /// Each knob: name, effective value (empty string when unset), and its holds: id, owner, value, priority, since
    /// (usec, wall clock).
    async fn list(&self) -> Vec<(String, OwnedVariant, Vec<(String, String, OwnedVariant, String, u64)>)> {
        let policy = self.policy.lock().await;
        let owned = |v: &KnobValue| OwnedVariant::try_from(Variant::from(v)).ok();
        policy
            .engine
            .knobs()
            .filter_map(|k| {
                let effective = k.effective().0.map_or_else(|| Variant::from(""), |v| Variant::from(&v));
                let holds = k
                    .holds
                    .iter()
                    .filter_map(|h| {
                        let priority = format!("{:?}", h.priority).to_lowercase();
                        Some((h.id.to_string(), h.owner.clone(), owned(&h.value)?, priority, h.since_ms * 1000))
                    })
                    .collect();
                Some((k.name.to_owned(), OwnedVariant::try_from(effective).ok()?, holds))
            })
            .collect()
    }
}

/// Ends the holds of each client that leaves the bus.
pub async fn release_on_exit(conn: Connection, policy: Shared) -> zbus::Result<()> {
    let mut gone = DBusProxy::new(&conn).await?.receive_name_owner_changed().await?;
    while let Some(signal) = gone.next().await {
        let Ok(args) = signal.args() else { continue };
        if args.new_owner().is_some() || !args.name().starts_with(':') {
            continue;
        }
        let mut policy = policy.lock().await;
        let changes = policy.engine.release(args.name(), None);
        if !changes.apply.is_empty()
            && let Err(e) = policy.commit(&conn, changes, None).await
        {
            log::warn!("knobs: release for {}: {e:#}", args.name());
        }
    }
    Ok(())
}
