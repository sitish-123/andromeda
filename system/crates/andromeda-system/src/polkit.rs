//! polkit checks for intents (docs/system-health.md §3.3); actions in system/data/org.andromeda.system.policy.

use std::collections::HashMap;

use zbus::Connection;
use zbus::fdo::{Error, Result};
use zbus::message::Header;
use zbus_polkit::policykit1::{AuthorityProxy, AuthorizationResult, CheckAuthorizationFlags, Subject};

/// What polkit answers for a caller without asking it: logind's `Can*` words.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Auth {
    Yes,
    Challenge,
    No,
}

/// Test buses have no polkit. `ANDROMEDA_SYSTEM_TEST=1` skips the check, and only on a bus that
/// `DBUS_SYSTEM_BUS_ADDRESS` names (the systemd unit sets neither).
pub fn test_bus() -> bool {
    std::env::var_os("DBUS_SYSTEM_BUS_ADDRESS").is_some()
        && std::env::var("ANDROMEDA_SYSTEM_TEST").as_deref() == Ok("1")
}

async fn authorize(
    conn: &Connection,
    header: &Header<'_>,
    action: &str,
    flags: CheckAuthorizationFlags,
) -> Result<AuthorizationResult> {
    let subject = Subject::new_for_message_header(header).map_err(|e| Error::Failed(format!("polkit subject: {e}")))?;
    let authority = AuthorityProxy::new(conn).await?;
    Ok(authority.check_authorization(&subject, action, &HashMap::new(), flags, "").await?)
}

/// Allows the call only when polkit authorizes its sender for `action`; may show the user's auth dialog.
pub async fn check(conn: &Connection, header: &Header<'_>, action: &str) -> Result<()> {
    check_with(conn, header, action, true).await
}

/// Like `check`; the auth dialog shows only when `interactive` is set.
pub async fn check_with(conn: &Connection, header: &Header<'_>, action: &str, interactive: bool) -> Result<()> {
    if test_bus() {
        return Ok(());
    }
    let flags =
        if interactive { CheckAuthorizationFlags::ALLOW_USER_INTERACTION } else { CheckAuthorizationFlags::empty() };
    let result = authorize(conn, header, action, flags).await?;
    if result.is_authorized { Ok(()) } else { Err(Error::AccessDenied(format!("not authorized for {action}"))) }
}

/// Whether the sender has `action` now, could get it with a password, or cannot; never shows a dialog.
pub async fn test(conn: &Connection, header: &Header<'_>, action: &str) -> Result<Auth> {
    if test_bus() {
        return Ok(Auth::Yes);
    }
    let result = authorize(conn, header, action, CheckAuthorizationFlags::empty()).await?;
    Ok(match (result.is_authorized, result.is_challenge) {
        (true, _) => Auth::Yes,
        (false, true) => Auth::Challenge,
        (false, false) => Auth::No,
    })
}
