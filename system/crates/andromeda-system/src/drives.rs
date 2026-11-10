//! `org.andromeda.System1.Drives`: udisks2 `PowerOff` for the caller. This service calls udisks2 as root, and udisks2
//! skips its polkit checks for root, so it first asks polkit for udisks2's own action on the real caller. It fails
//! closed, and it passes udisks2's errors back unchanged. Mount and Unmount stay direct calls from the user.

use std::collections::HashMap;
use std::fmt::Display;

use zbus::fdo::{Error, Result};
use zbus::message::Header;
use zbus::zvariant::DynamicType;
use zbus::{Connection, OwnedObjectPath, OwnedVariant, interface};

use crate::nm_control::{NmResult, Options};
use crate::{login, nm_control, polkit};

const UDISKS: &str = "org.freedesktop.UDisks2";
const DRIVE_PREFIX: &str = "/org/freedesktop/UDisks2/drives/";
const BLOCK_IFACE: &str = "org.freedesktop.UDisks2.Block";
const PARTITION_IFACE: &str = "org.freedesktop.UDisks2.Partition";
const DRIVE_IFACE: &str = "org.freedesktop.UDisks2.Drive";
const PROPERTIES_IFACE: &str = "org.freedesktop.DBus.Properties";
const LOGIN1: &str = "org.freedesktop.login1";

const NO_INTERACTION: &str = "auth.no_user_interaction";
const POWER_OFF: &str = "org.freedesktop.udisks2.power-off-drive";
const POWER_OFF_SYSTEM: &str = "org.freedesktop.udisks2.power-off-drive-system";
const POWER_OFF_OTHER_SEAT: &str = "org.freedesktop.udisks2.power-off-drive-other-seat";

type Objects = HashMap<OwnedObjectPath, HashMap<String, HashMap<String, OwnedVariant>>>;

/// Holds no state, so a pending polkit dialog blocks no other call.
pub struct Drives;

#[interface(name = "org.andromeda.System1.Drives")]
impl Drives {
    /// udisks2's `Drive.PowerOff` on `drive`; option `auth.no_user_interaction`.
    async fn power_off(
        &self,
        drive: OwnedObjectPath,
        options: Options,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> std::result::Result<(), nm_control::NmError> {
        power_off(conn, &header, drive, options).await
    }
}

/// Accepts `path` only as one udisks2 object directly under `prefix`; udisks2 escapes object names to `[A-Za-z0-9_]`.
fn check_path(path: &str, prefix: &str) -> Result<()> {
    let named = path
        .strip_prefix(prefix)
        .is_some_and(|name| !name.is_empty() && name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_'));
    if named { Ok(()) } else { Err(Error::InvalidArgs(format!("{path} is no udisks2 object under {prefix}"))) }
}

/// Whether polkit may show its dialog (udisks2's `auth.no_user_interaction`); refuses any other option.
fn interactive(options: &Options) -> Result<bool> {
    if let Some(key) = options.keys().find(|k| k.as_str() != NO_INTERACTION) {
        return Err(Error::InvalidArgs(format!("unknown option {key}")));
    }
    match options.get(NO_INTERACTION) {
        None => Ok(true),
        Some(value) => bool::try_from(value)
            .map(|no| !no)
            .map_err(|_| Error::InvalidArgs(format!("{NO_INTERACTION} is no boolean"))),
    }
}

/// udisks2's power-off action: `-system` for a system drive, else `-other-seat` when the caller has no active session
/// on the drive's seat.
fn power_off_action(system: bool, on_seat: bool) -> &'static str {
    match (system, on_seat) {
        (true, _) => POWER_OFF_SYSTEM,
        (false, false) => POWER_OFF_OTHER_SEAT,
        (false, true) => POWER_OFF,
    }
}

async fn call<B, R>(conn: &Connection, dest: &str, path: &str, iface: &str, method: &str, body: &B) -> NmResult<R>
where
    B: serde::Serialize + DynamicType,
    R: serde::de::DeserializeOwned + zbus::zvariant::Type,
{
    Ok(conn.call_method(Some(dest), path, Some(iface), method, body).await?.body().deserialize::<R>()?)
}

async fn property<R>(conn: &Connection, dest: &str, path: &str, iface: &str, name: &str) -> NmResult<R>
where
    R: TryFrom<OwnedVariant>,
    R::Error: Display,
{
    let value: OwnedVariant = call(conn, dest, path, PROPERTIES_IFACE, "Get", &(iface, name)).await?;
    Ok(R::try_from(value).map_err(|e| Error::Failed(format!("{path} {name}: {e}")))?)
}

/// `sd_uid_is_on_seat(uid, require_active, seat)`, the check udisks2 makes: an active session of `uid` on `seat`.
async fn active_on_seat(conn: &Connection, uid: u32, seat: &str) -> NmResult<bool> {
    let sessions: Vec<(String, u32, String, String, OwnedObjectPath)> =
        call(conn, LOGIN1, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "ListSessions", &()).await?;
    for (_, _, _, _, path) in sessions.iter().filter(|s| s.1 == uid && !seat.is_empty() && s.3 == seat) {
        if property::<bool>(conn, LOGIN1, path, "org.freedesktop.login1.Session", "Active").await? {
            return Ok(true);
        }
    }
    Ok(false)
}

/// The whole-disk block of `drive`, as udisks2 picks it for `PowerOff`.
async fn drive_block(conn: &Connection, drive: &str) -> NmResult<HashMap<String, OwnedVariant>> {
    let mut objects: Objects =
        call(conn, UDISKS, "/org/freedesktop/UDisks2", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &())
            .await?;
    objects
        .drain()
        .filter(|(_, ifaces)| !ifaces.contains_key(PARTITION_IFACE))
        .find_map(|(_, mut ifaces)| {
            let block = ifaces.remove(BLOCK_IFACE)?;
            let owner = OwnedObjectPath::try_from(block.get("Drive")?.try_clone().ok()?).ok()?;
            (owner.as_str() == drive).then_some(block)
        })
        .ok_or_else(|| Error::Failed(format!("no block device of {drive}")).into())
}

pub async fn power_off(
    conn: &Connection,
    header: &Header<'_>,
    drive: OwnedObjectPath,
    options: Options,
) -> NmResult<()> {
    check_path(&drive, DRIVE_PREFIX)?;
    let interactive = interactive(&options)?;
    let (uid, _) = login::caller(conn, header).await?;
    let block = drive_block(conn, &drive).await?;
    // A missing hint counts as a system drive: more authorization, never less.
    let system = block.get("HintSystem").is_none_or(|v| bool::try_from(v).unwrap_or(true));
    let seat: String = property(conn, UDISKS, &drive, DRIVE_IFACE, "Seat").await?;
    let on_seat = active_on_seat(conn, uid, &seat).await?;
    polkit::check_with(conn, header, power_off_action(system, on_seat), interactive).await?;
    call(conn, UDISKS, &drive, DRIVE_IFACE, "PowerOff", &(options,)).await
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use zbus::Variant;

    use super::*;

    #[test]
    fn paths_must_name_one_object_of_their_kind() {
        assert!(check_path("/org/freedesktop/UDisks2/drives/Kingston_DT_2e0_0123", DRIVE_PREFIX).is_ok());
        for bad in [
            "/org/freedesktop/UDisks2/drives/",
            "/org/freedesktop/UDisks2/drives/Stick/x",
            "/org/freedesktop/UDisks2/drives/Kingston-DT",
            "/org/freedesktop/UDisks2/block_devices/sdb1",
            "/org/freedesktop/UDisks2/Manager",
        ] {
            assert!(check_path(bad, DRIVE_PREFIX).is_err(), "{bad}");
        }
    }

    #[test]
    fn power_off_asks_for_the_strongest_action_that_applies() {
        assert_eq!(power_off_action(false, true), POWER_OFF);
        assert_eq!(power_off_action(false, false), POWER_OFF_OTHER_SEAT);
        assert_eq!(power_off_action(true, true), POWER_OFF_SYSTEM);
        assert_eq!(power_off_action(true, false), POWER_OFF_SYSTEM);
    }

    #[test]
    fn options_set_interaction_and_refuse_unknown_keys() {
        let opts = |rows: &[(&str, Variant<'static>)]| -> Options {
            rows.iter().map(|(k, v)| ((*k).to_owned(), OwnedVariant::try_from(v.clone()).unwrap())).collect()
        };
        assert!(interactive(&opts(&[])).unwrap());
        assert!(!interactive(&opts(&[(NO_INTERACTION, Variant::from(true))])).unwrap());
        assert!(interactive(&opts(&[("force", Variant::from(true))])).is_err());
        assert!(interactive(&opts(&[(NO_INTERACTION, Variant::from("yes"))])).is_err());
    }
}
