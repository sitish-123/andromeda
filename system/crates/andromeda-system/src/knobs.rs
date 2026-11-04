//! `/org/andromeda/System1/Knobs`: the user's choices (docs/system-health.md §5). Step 1 has `Set` only; holds,
//! `Hold`, `Release` and `List` come with the rule engine (step 2).

use zbus::fdo::{Error, Result};
use zbus::message::Header;
use zbus::{Connection, OwnedVariant, interface};

use crate::{battery, polkit, power};

pub const PATH: &str = "/org/andromeda/System1/Knobs";
const NOT_SET: u32 = u32::MAX;

pub struct Knobs;

fn failed(e: &anyhow::Error) -> Error {
    Error::Failed(format!("{e:#}"))
}

#[interface(name = "org.andromeda.System1.Knobs")]
impl Knobs {
    /// The user's choice for a knob, stored by its backend:
    /// `power.profile` (s): power-saver, balanced or performance;
    /// `battery.charge_limit` ((buu) enabled, start %, end %; 4294967295 keeps the current value) on every battery
    /// that powers this machine and supports a limit.
    async fn set(
        &self,
        knob: &str,
        value: OwnedVariant,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> Result<()> {
        match knob {
            "power.profile" => {
                polkit::check(conn, &header, "org.andromeda.system.power-profile").await?;
                let profile =
                    String::try_from(value).map_err(|_| Error::InvalidArgs("power.profile takes s".into()))?;
                power::set_profile(conn, &profile).await.map_err(|e| failed(&e))
            }
            "battery.charge_limit" => {
                polkit::check(conn, &header, "org.andromeda.system.charge-limit").await?;
                let (enabled, start, end) = <(bool, u32, u32)>::try_from(value)
                    .map_err(|_| Error::InvalidArgs("battery.charge_limit takes (buu)".into()))?;
                if start != NOT_SET || end != NOT_SET {
                    return Err(Error::NotSupported("charge-limit values are not settable yet".into()));
                }
                battery::set_charge_limit_enabled(conn, enabled).await.map_err(|e| failed(&e))
            }
            _ => Err(Error::InvalidArgs(format!("unknown knob {knob:?}"))),
        }
    }
}
