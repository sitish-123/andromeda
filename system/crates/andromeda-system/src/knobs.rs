//! `/org/andromeda/System1/Knobs`: the user's machine-wide choices (docs/system-health.md §5). Per-device choices
//! are methods on the device (`Battery.SetChargeLimit`, `KbdBacklight.SetBrightness`). Step 1 has `Set` only;
//! holds, `Hold`, `Release` and `List` come with the rule engine (step 2).

use zbus::fdo::{Error, Result};
use zbus::message::Header;
use zbus::{Connection, OwnedVariant, interface};

use crate::{polkit, power};

pub const PATH: &str = "/org/andromeda/System1/Knobs";

pub struct Knobs;

fn failed(e: &anyhow::Error) -> Error {
    Error::Failed(format!("{e:#}"))
}

#[interface(name = "org.andromeda.System1.Knobs")]
impl Knobs {
    /// The user's choice for a knob, stored by its backend:
    /// `power.profile` (s): power-saver, balanced or performance.
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
            _ => Err(Error::InvalidArgs(format!("unknown knob {knob:?}"))),
        }
    }
}
