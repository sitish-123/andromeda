//! Snapshot before system updates (U4): a snapper pre/post pair of the `root` config around the transaction, through
//! snapper's D-Bus API. When snap-pac is installed its pacman hooks already take that pair for every transaction,
//! ours included, so the helper takes none (unless snap-pac is set to skip `root`).

use std::collections::HashMap;
use std::path::Path;

use zbus::Connection;

#[cfg(feature = "arch")]
const SNAP_PAC_HOOK: &str = "/usr/share/libalpm/hooks/05-snap-pac-pre.hook";
/// Debian's snapper package: `DPkg::Pre-Invoke`/`Post-Invoke` take the pair for every dpkg run, ours included.
#[cfg(feature = "apt")]
const APT_HOOK: &str = "/etc/apt/apt.conf.d/80snapper";
const CONFIG: &str = "root";
const DESCRIPTION: &str = "Andromeda system update";

#[zbus::proxy(
    interface = "org.opensuse.Snapper",
    default_service = "org.opensuse.Snapper",
    default_path = "/org/opensuse/Snapper"
)]
trait Snapper {
    fn list_configs(&self) -> zbus::Result<Vec<(String, String, HashMap<String, String>)>>;
    fn create_pre_snapshot(
        &self,
        config: &str,
        description: &str,
        cleanup: &str,
        userdata: HashMap<&str, &str>,
    ) -> zbus::Result<u32>;
    fn create_post_snapshot(
        &self,
        config: &str,
        pre: u32,
        description: &str,
        cleanup: &str,
        userdata: HashMap<&str, &str>,
    ) -> zbus::Result<u32>;
}

pub struct Pre(u32);

/// The package manager's own snapshot hook, when it takes the `root` pair: "snap-pac" or "snapper-apt".
#[cfg(feature = "arch")]
fn hook() -> Option<&'static str> {
    (Path::new(SNAP_PAC_HOOK).exists() && snap_pac_takes_root()).then_some("snap-pac")
}

/// The hook runs only with a `root` config and without `DISABLE_APT_SNAPSHOT=yes` in /etc/default/snapper.
#[cfg(feature = "apt")]
fn hook() -> Option<&'static str> {
    let disabled = std::fs::read_to_string("/etc/default/snapper").unwrap_or_default().lines().any(|line| {
        line.split_once('=')
            .is_some_and(|(k, v)| k.trim() == "DISABLE_APT_SNAPSHOT" && v.trim().trim_matches(['"', '\'']) == "yes")
    });
    (Path::new(APT_HOOK).exists() && Path::new("/etc/snapper/configs/root").exists() && !disabled)
        .then_some("snapper-apt")
}

/// What takes the snapshot before an update: "snap-pac" or "snapper-apt", "snapper" (us), or "" (none).
pub async fn mode(conn: &Connection) -> &'static str {
    if let Some(hook) = hook() {
        return hook;
    }
    let Ok(snapper) = SnapperProxy::new(conn).await else { return "" };
    match snapper.list_configs().await {
        Ok(configs) if configs.iter().any(|(name, ..)| name == CONFIG) => "snapper",
        _ => "",
    }
}

/// snap-pac snapshots the `root` config unless /etc/snap-pac.ini says `snapshot = no` in its `[root]` section, or
/// `SNAP_PAC_SKIP` is set (snap-pac reads it from the environment of the pacman run, which here is ours).
#[cfg(feature = "arch")]
fn snap_pac_takes_root() -> bool {
    if std::env::var_os("SNAP_PAC_SKIP").is_some() {
        return false;
    }
    let ini = std::fs::read_to_string("/etc/snap-pac.ini").unwrap_or_default();
    let mut in_root = false;
    for line in ini.lines().map(str::trim) {
        if line.starts_with('[') {
            in_root = line == "[root]";
        } else if in_root && let Some((key, value)) = line.split_once('=')
            && key.trim() == "snapshot"
        {
            return !matches!(value.trim().to_ascii_lowercase().as_str(), "no" | "false" | "0");
        }
    }
    true
}

/// `Ok(None)`: no snapper `root` config, or the package manager's hook takes the snapshots. An error means snapper is set up but the
/// snapshot failed; the upgrade must not run then.
pub async fn pre(conn: &Connection) -> zbus::Result<Option<Pre>> {
    if hook().is_some() {
        return Ok(None);
    }
    let snapper = SnapperProxy::new(conn).await?;
    let configs = match snapper.list_configs().await {
        // Snapper not installed: no snapshots. Any other error (snapperd crashed, timed out) stops the upgrade.
        Err(zbus::Error::FDO(e))
            if matches!(
                *e,
                zbus::fdo::Error::ServiceUnknown(_) | zbus::fdo::Error::NameHasNoOwner(_)
            ) =>
        {
            return Ok(None);
        }
        Err(zbus::Error::MethodError(name, ..)) if name.as_str() == "org.freedesktop.DBus.Error.ServiceUnknown" => {
            return Ok(None);
        }
        other => other?,
    };
    if !configs.iter().any(|(name, ..)| name == CONFIG) {
        return Ok(None);
    }
    let number = snapper.create_pre_snapshot(CONFIG, DESCRIPTION, "number", HashMap::new()).await?;
    log::info!("snapper pre snapshot {number}");
    Ok(Some(Pre(number)))
}

pub async fn post(conn: &Connection, pre: Pre) {
    let result = async {
        SnapperProxy::new(conn).await?.create_post_snapshot(CONFIG, pre.0, DESCRIPTION, "number", HashMap::new()).await
    }
    .await;
    match result {
        Ok(number) => log::info!("snapper post snapshot {number}"),
        Err(e) => log::warn!("snapper post snapshot for {}: {e}", pre.0),
    }
}
