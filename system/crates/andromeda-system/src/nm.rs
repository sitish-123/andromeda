//! NetworkManager queries for the session part.

use anyhow::Result;
use zbus::Connection;

const NM: &str = "org.freedesktop.NetworkManager";

/// The URL NetworkManager probes for connectivity; a captive portal redirects it to its sign-in page. NM has no
/// portal URL of its own (docs/system-health.md §12.1).
pub async fn connectivity_check_uri(system: &Connection) -> Result<String> {
    let props = zbus::fdo::PropertiesProxy::builder(system)
        .destination(NM)
        .path("/org/freedesktop/NetworkManager")
        .build()
        .await?;
    let uri = props.get(zbus::names::InterfaceName::from_static_str(NM)?, "ConnectivityCheckUri").await?;
    Ok(String::try_from(uri)?)
}
