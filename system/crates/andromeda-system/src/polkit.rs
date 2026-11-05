//! polkit checks for intents (docs/system-health.md §3.3); actions in system/data/org.andromeda.system.policy.

use std::collections::HashMap;

use zbus::Connection;
use zbus::fdo::{Error, Result};
use zbus::message::Header;
use zbus_polkit::policykit1::{AuthorityProxy, CheckAuthorizationFlags, Subject};

/// Test buses have no polkit. `ANDROMEDA_SYSTEM_TEST=1` skips the check, and only on a bus that
/// `DBUS_SYSTEM_BUS_ADDRESS` names (the systemd unit sets neither).
fn test_bus() -> bool {
    std::env::var_os("DBUS_SYSTEM_BUS_ADDRESS").is_some()
        && std::env::var("ANDROMEDA_SYSTEM_TEST").as_deref() == Ok("1")
}

/// Allows the call only when polkit authorizes its sender for `action`; may show the user's auth dialog.
pub async fn check(conn: &Connection, header: &Header<'_>, action: &str) -> Result<()> {
    if test_bus() {
        return Ok(());
    }
    let subject = Subject::new_for_message_header(header).map_err(|e| Error::Failed(format!("polkit subject: {e}")))?;
    let authority = AuthorityProxy::new(conn).await?;
    let result = authority
        .check_authorization(&subject, action, &HashMap::new(), CheckAuthorizationFlags::ALLOW_USER_INTERACTION, "")
        .await?;
    if result.is_authorized { Ok(()) } else { Err(Error::AccessDenied(format!("not authorized for {action}"))) }
}
