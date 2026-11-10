//! NetworkManager writes for callers of `org.andromeda.System1.Network`. This service calls NetworkManager as root,
//! and NetworkManager skips its polkit and `connection.permissions` checks for root, so these functions make the same
//! checks for the real caller first, with NetworkManager's own polkit action ids. They fail closed, and they pass
//! NetworkManager's errors back unchanged.

use std::collections::HashMap;

use zbus::fdo::{Error, Result};
use zbus::message::{Header, Message};
use zbus::names::{ErrorName, OwnedErrorName};
use zbus::zvariant::DynamicType;
use zbus::{Connection, DBusError, OwnedObjectPath, OwnedVariant, Variant};

use crate::{login, polkit};

/// NetworkManager connection settings: setting name to property name to value.
pub type Settings = HashMap<String, HashMap<String, OwnedVariant>>;
pub type Options = HashMap<String, OwnedVariant>;
pub type NmResult<T> = std::result::Result<T, NmError>;

/// A D-Bus error under any name, so NetworkManager's errors reach the caller with NetworkManager's name.
#[derive(Debug)]
pub struct NmError {
    name: OwnedErrorName,
    description: Option<String>,
}

impl From<Error> for NmError {
    fn from(e: Error) -> Self {
        Self { name: e.name().into(), description: e.description().map(str::to_owned) }
    }
}

impl From<zbus::Error> for NmError {
    fn from(e: zbus::Error) -> Self {
        match e {
            zbus::Error::MethodError(name, description, _) => Self { name, description },
            e => Error::from(e).into(),
        }
    }
}

impl DBusError for NmError {
    fn create_reply(&self, call: &Header<'_>) -> zbus::Result<Message> {
        Message::error(call, &self.name).build(&(self.description.as_deref().unwrap_or(""),))
    }

    fn name(&self) -> ErrorName<'_> {
        self.name.as_ref()
    }

    fn description(&self) -> Option<&str> {
        self.description.as_deref()
    }
}

const NM: &str = "org.freedesktop.NetworkManager";
const NM_PATH: &str = "/org/freedesktop/NetworkManager";
const SETTINGS_PATH: &str = "/org/freedesktop/NetworkManager/Settings";
const SETTINGS_IFACE: &str = "org.freedesktop.NetworkManager.Settings";
const CONNECTION_IFACE: &str = "org.freedesktop.NetworkManager.Settings.Connection";
const ACTIVE_IFACE: &str = "org.freedesktop.NetworkManager.Connection.Active";
const DEVICE_IFACE: &str = "org.freedesktop.NetworkManager.Device";
const WIRELESS_IFACE: &str = "org.freedesktop.NetworkManager.Device.Wireless";
const PROPERTIES_IFACE: &str = "org.freedesktop.DBus.Properties";

const NETWORK_CONTROL: &str = "org.freedesktop.NetworkManager.network-control";
const WIFI_SCAN: &str = "org.freedesktop.NetworkManager.wifi.scan";
const ENABLE_WIFI: &str = "org.freedesktop.NetworkManager.enable-disable-wifi";
const MODIFY_OWN: &str = "org.freedesktop.NetworkManager.settings.modify.own";
const MODIFY_SYSTEM: &str = "org.freedesktop.NetworkManager.settings.modify.system";

/// What an object path argument must name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PathKind {
    Device,
    Connection,
    Active,
}

impl PathKind {
    fn prefix(self) -> &'static str {
        match self {
            Self::Device => "/org/freedesktop/NetworkManager/Devices/",
            Self::Connection => "/org/freedesktop/NetworkManager/Settings/",
            Self::Active => "/org/freedesktop/NetworkManager/ActiveConnection/",
        }
    }
}

/// Accepts `path` only as one of NetworkManager's numbered objects of `kind`.
fn check_path(path: &str, kind: PathKind) -> Result<()> {
    let numbered =
        path.strip_prefix(kind.prefix()).is_some_and(|id| !id.is_empty() && id.bytes().all(|b| b.is_ascii_digit()));
    if numbered { Ok(()) } else { Err(Error::InvalidArgs(format!("{path} is no NetworkManager {kind:?} path"))) }
}

/// A permission entry for `user`: `user:name` or `user:name:` (NetworkManager keeps the third field reserved and empty).
fn names(entry: &str, user: &str) -> bool {
    entry.strip_prefix("user:").is_some_and(|rest| rest.strip_suffix(':').unwrap_or(rest) == user)
}

/// `connection.permissions` in `settings`; empty when unset.
fn permissions(settings: &Settings) -> Result<Vec<String>> {
    let Some(value) = settings.get("connection").and_then(|c| c.get("permissions")) else { return Ok(Vec::new()) };
    value
        .try_clone()
        .ok()
        .and_then(|v| Vec::<String>::try_from(v).ok())
        .ok_or_else(|| Error::InvalidArgs("connection.permissions is no string array".into()))
}

/// NetworkManager lets `user` use a connection when its permissions are empty or name the user.
fn may_use(permissions: &[String], user: &str) -> Result<()> {
    if permissions.is_empty() || permissions.iter().any(|p| names(p, user)) {
        Ok(())
    } else {
        Err(Error::AccessDenied(format!("the connection is not for user {user}")))
    }
}

/// A caller may restrict the connections it writes only to itself.
fn names_only(permissions: &[String], user: &str) -> Result<()> {
    if permissions.iter().all(|p| names(p, user)) {
        Ok(())
    } else {
        Err(Error::AccessDenied(format!("connection.permissions may name only user {user}")))
    }
}

/// NetworkManager's settings action: modify.own only when the connection is the caller's alone before and after.
fn modify_action(before: &[String], after: &[String], user: &str) -> &'static str {
    let own = |p: &[String]| p.len() == 1 && names(&p[0], user);
    if own(before) && own(after) { MODIFY_OWN } else { MODIFY_SYSTEM }
}

async fn call<B, R>(conn: &Connection, path: &str, iface: &str, method: &str, body: &B) -> NmResult<R>
where
    B: serde::Serialize + DynamicType,
    R: serde::de::DeserializeOwned + zbus::zvariant::Type,
{
    Ok(conn.call_method(Some(NM), path, Some(iface), method, body).await?.body().deserialize::<R>()?)
}

/// The caller's user name, as `connection.permissions` holds it.
async fn user(conn: &Connection, header: &Header<'_>) -> Result<String> {
    let (uid, _) = login::caller(conn, header).await?;
    let uid = uid.to_string();
    let out = tokio::process::Command::new("getent")
        .args(["passwd", &uid])
        .output()
        .await
        .map_err(|e| Error::Failed(format!("getent: {e}")))?;
    let text = String::from_utf8_lossy(&out.stdout);
    let fields: Vec<&str> = text.lines().next().unwrap_or("").split(':').collect();
    match fields.as_slice() {
        [name, _, id, ..] if out.status.success() && !name.is_empty() && *id == uid => Ok((*name).to_owned()),
        _ => Err(Error::Failed(format!("no user name for uid {uid}"))),
    }
}

async fn connection_settings(conn: &Connection, connection: &str) -> NmResult<Settings> {
    call(conn, connection, CONNECTION_IFACE, "GetSettings", &()).await
}

/// `Settings.AddConnection` for the caller.
pub async fn add(conn: &Connection, header: &Header<'_>, settings: Settings) -> NmResult<OwnedObjectPath> {
    let user = user(conn, header).await?;
    let perms = permissions(&settings)?;
    names_only(&perms, &user)?;
    polkit::check(conn, header, modify_action(&perms, &perms, &user)).await?;
    call(conn, SETTINGS_PATH, SETTINGS_IFACE, "AddConnection", &(settings,)).await
}

/// `Settings.Connection.Update2` for the caller; empty `settings` keep the saved ones, as in NetworkManager.
pub async fn update(
    conn: &Connection,
    header: &Header<'_>,
    connection: OwnedObjectPath,
    settings: Settings,
    flags: u32,
    args: Options,
) -> NmResult<Options> {
    check_path(&connection, PathKind::Connection)?;
    let user = user(conn, header).await?;
    let before = permissions(&connection_settings(conn, &connection).await?)?;
    may_use(&before, &user)?;
    let after = if settings.is_empty() { before.clone() } else { permissions(&settings)? };
    if !settings.is_empty() {
        names_only(&after, &user)?;
    }
    polkit::check(conn, header, modify_action(&before, &after, &user)).await?;
    call(conn, &connection, CONNECTION_IFACE, "Update2", &(settings, flags, args)).await
}

/// `Settings.Connection.Delete` for the caller.
pub async fn delete(conn: &Connection, header: &Header<'_>, connection: OwnedObjectPath) -> NmResult<()> {
    check_path(&connection, PathKind::Connection)?;
    let user = user(conn, header).await?;
    let perms = permissions(&connection_settings(conn, &connection).await?)?;
    may_use(&perms, &user)?;
    polkit::check(conn, header, modify_action(&perms, &perms, &user)).await?;
    call(conn, &connection, CONNECTION_IFACE, "Delete", &()).await
}

/// `DeactivateConnection` for the caller; the active connection's profile must be one the caller may use.
pub async fn deactivate(conn: &Connection, header: &Header<'_>, active: OwnedObjectPath) -> NmResult<()> {
    check_path(&active, PathKind::Active)?;
    let user = user(conn, header).await?;
    let profile: OwnedVariant = call(conn, &active, PROPERTIES_IFACE, "Get", &(ACTIVE_IFACE, "Connection")).await?;
    let profile = OwnedObjectPath::try_from(profile).map_err(|e| Error::Failed(format!("{active} Connection: {e}")))?;
    check_path(&profile, PathKind::Connection)?;
    may_use(&permissions(&connection_settings(conn, &profile).await?)?, &user)?;
    polkit::check(conn, header, NETWORK_CONTROL).await?;
    call(conn, NM_PATH, NM, "DeactivateConnection", &(active,)).await
}

/// `Device.Disconnect` for the caller.
pub async fn disconnect(conn: &Connection, header: &Header<'_>, device: OwnedObjectPath) -> NmResult<()> {
    check_path(&device, PathKind::Device)?;
    polkit::check(conn, header, NETWORK_CONTROL).await?;
    call(conn, &device, DEVICE_IFACE, "Disconnect", &()).await
}

/// NetworkManager's `WirelessEnabled` for the caller.
pub async fn set_wireless_enabled(conn: &Connection, header: &Header<'_>, enabled: bool) -> NmResult<()> {
    polkit::check(conn, header, ENABLE_WIFI).await?;
    call(conn, NM_PATH, PROPERTIES_IFACE, "Set", &(NM, "WirelessEnabled", Variant::from(enabled))).await
}

/// `Device.Wireless.RequestScan` for the caller.
pub async fn request_scan(
    conn: &Connection,
    header: &Header<'_>,
    device: OwnedObjectPath,
    options: Options,
) -> NmResult<()> {
    check_path(&device, PathKind::Device)?;
    polkit::check(conn, header, WIFI_SCAN).await?;
    call(conn, &device, WIRELESS_IFACE, "RequestScan", &(options,)).await
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    fn settings(rows: &[(&str, &str, Variant<'static>)]) -> Settings {
        let mut s = Settings::new();
        for (setting, key, value) in rows {
            let value = OwnedVariant::try_from(value.try_clone().unwrap()).unwrap();
            s.entry((*setting).into()).or_default().insert((*key).into(), value);
        }
        s
    }

    fn perms(list: &[&str]) -> Settings {
        settings(&[(
            "connection",
            "permissions",
            Variant::from(list.iter().map(ToString::to_string).collect::<Vec<_>>()),
        )])
    }

    #[test]
    fn paths_must_name_their_kind_of_object() {
        assert!(check_path("/org/freedesktop/NetworkManager/Devices/3", PathKind::Device).is_ok());
        assert!(check_path("/org/freedesktop/NetworkManager/Settings/12", PathKind::Connection).is_ok());
        assert!(check_path("/org/freedesktop/NetworkManager/ActiveConnection/5", PathKind::Active).is_ok());
        for (path, kind) in [
            ("/", PathKind::Device),
            ("/", PathKind::Connection),
            ("/org/freedesktop/NetworkManager/Settings/1", PathKind::Device),
            ("/org/freedesktop/NetworkManager/Devices/", PathKind::Device),
            ("/org/freedesktop/NetworkManager/Devices/1/x", PathKind::Device),
            ("/org/freedesktop/NetworkManager/Devices/../Settings", PathKind::Device),
            ("/org/freedesktop/NetworkManager/Settings", PathKind::Connection),
            ("/org/freedesktop/NetworkManager", PathKind::Active),
        ] {
            assert!(check_path(path, kind).is_err(), "{path} passed as {kind:?}");
        }
    }

    #[test]
    fn foreign_connections_are_refused() {
        let me = |s: &Settings| may_use(&permissions(s).unwrap(), "me");
        assert!(me(&Settings::new()).is_ok());
        assert!(me(&perms(&[])).is_ok());
        assert!(me(&perms(&["user:bob:", "user:me:"])).is_ok());
        assert!(me(&perms(&["user:bob:"])).is_err());
        assert!(me(&perms(&["user:mel:"])).is_err());
        assert!(me(&perms(&["user:me:x"])).is_err());
    }

    #[test]
    fn written_settings_may_name_only_the_caller() {
        assert!(names_only(&[], "me").is_ok());
        assert!(names_only(&["user:me:".into()], "me").is_ok());
        assert!(names_only(&["user:me:".into(), "user:bob:".into()], "me").is_err());
        assert!(names_only(&["user:bob:".into()], "me").is_err());
        assert!(names_only(&["user:me".into()], "me").is_ok());
        for bad in ["user:me::", "user:me:x", "user:", "user:mex", "me", "group:me:"] {
            assert!(names_only(&[bad.into()], "me").is_err(), "{bad}");
        }
        assert!(permissions(&settings(&[("connection", "permissions", Variant::from("user:me:"))])).is_err());
    }

    #[test]
    fn modify_own_only_for_the_callers_own_connection() {
        let own = vec!["user:me:".to_string()];
        assert_eq!(modify_action(&own, &own, "me"), MODIFY_OWN);
        assert_eq!(modify_action(&[], &[], "me"), MODIFY_SYSTEM);
        assert_eq!(modify_action(&[], &own, "me"), MODIFY_SYSTEM);
        assert_eq!(modify_action(&own, &[], "me"), MODIFY_SYSTEM);
        assert_eq!(modify_action(&["user:bob:".into()], &["user:bob:".into()], "me"), MODIFY_SYSTEM);
    }
}
