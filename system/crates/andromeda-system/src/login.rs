//! `/org/andromeda/System1/Login`: the shell's door to logind for power actions, inhibitors, the session list and
//! backlight brightness. This service calls login1 as root, so it first makes logind's own polkit checks, with
//! logind's action ids, for the caller: each call needs the same authorization as when the caller asks logind.

use std::collections::HashMap;
use std::fmt::Display;

use futures_util::StreamExt;
use zbus::fdo::{Error, Result};
use zbus::message::Header;
use zbus::names::BusName;
use zbus::object_server::{InterfaceRef, SignalEmitter};
use zbus::proxy::CacheProperties;
use zbus::zvariant::{DynamicType, Type};
use zbus::{Connection, OwnedFd, OwnedObjectPath, OwnedVariant, interface};

use crate::polkit::{self, Auth};

pub const PATH: &str = "/org/andromeda/System1/Login";
const LOGIN1: &str = "org.freedesktop.login1";
const LOGIN1_PATH: &str = "/org/freedesktop/login1";
const MANAGER: &str = "org.freedesktop.login1.Manager";
const SESSION: &str = "org.freedesktop.login1.Session";
const USER: &str = "org.freedesktop.login1.User";
/// `SD_LOGIND_ROOT_CHECK_INHIBITORS`: root's calls also honour "block-weak" inhibitors (org.freedesktop.login1(5)).
const ROOT_CHECK_INHIBITORS: u64 = 1 << 0;
/// `SD_LOGIND_SKIP_INHIBITORS`: the call passes "block" and "block-weak" inhibitors.
const SKIP_INHIBITORS: u64 = 1 << 4;
const FIRMWARE: &str = "reboot-to-firmware-setup";
const FIRMWARE_ACTION: &str = "org.freedesktop.login1.set-reboot-to-firmware-setup";
/// logind Manager properties that `ManagerProperties` returns.
const MIRRORED: [&str; 8] = [
    "BlockInhibited",
    "HandleLidSwitch",
    "HandleLidSwitchDocked",
    "HandleLidSwitchExternalPower",
    "HandlePowerKey",
    "PreparingForShutdown",
    "PreparingForSleep",
    "ScheduledShutdown",
];

/// (what, who, why, mode, uid, pid), as logind's `ListInhibitors`.
pub type Inhibitor = (String, String, String, String, u32, u32);
/// (id, uid, user, seat, path), as logind's `ListSessions`.
pub type SessionRow = (String, u32, String, String, OwnedObjectPath);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PowerAction {
    Reboot,
    PowerOff,
    Suspend,
    Hibernate,
    SuspendThenHibernate,
}

impl PowerAction {
    fn parse(name: &str) -> Result<Self> {
        Ok(match name {
            "reboot" => Self::Reboot,
            "power-off" => Self::PowerOff,
            "suspend" => Self::Suspend,
            "hibernate" => Self::Hibernate,
            "suspend-then-hibernate" => Self::SuspendThenHibernate,
            _ => return Err(Error::InvalidArgs(format!("unknown power action {name:?}"))),
        })
    }

    fn method(self) -> &'static str {
        match self {
            Self::Reboot => "RebootWithFlags",
            Self::PowerOff => "PowerOffWithFlags",
            Self::Suspend => "SuspendWithFlags",
            Self::Hibernate => "HibernateWithFlags",
            Self::SuspendThenHibernate => "SuspendThenHibernateWithFlags",
        }
    }

    fn can_method(self) -> &'static str {
        match self {
            Self::Reboot => "CanReboot",
            Self::PowerOff => "CanPowerOff",
            Self::Suspend => "CanSuspend",
            Self::Hibernate => "CanHibernate",
            Self::SuspendThenHibernate => "CanSuspendThenHibernate",
        }
    }

    /// The inhibitor kind that stops it.
    fn what(self) -> &'static str {
        match self {
            Self::Reboot | Self::PowerOff => "shutdown",
            _ => "sleep",
        }
    }

    /// logind's polkit action stem; suspend-then-hibernate uses hibernate's.
    fn polkit(self) -> &'static str {
        match self {
            Self::Reboot => "reboot",
            Self::PowerOff => "power-off",
            Self::Suspend => "suspend",
            Self::Hibernate | Self::SuspendThenHibernate => "hibernate",
        }
    }
}

/// How inhibitors stop an action for a caller, strongest last.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Block {
    None,
    Weak,
    Strong,
}

/// logind's rule since systemd 257: "block" stops every caller, its owner too; "block-weak" stops all uids but its
/// owner's; "delay" stops nobody.
fn block(inhibitors: &[Inhibitor], what: &str, uid: u32) -> Block {
    inhibitors
        .iter()
        .filter(|(kinds, ..)| kinds.split(':').any(|kind| kind == what))
        .map(|(_, _, _, mode, owner, _)| match mode.as_str() {
            "block" => Block::Strong,
            "block-weak" if *owner != uid => Block::Weak,
            _ => Block::None,
        })
        .max()
        .unwrap_or(Block::None)
}

/// Session classes that are no login: per-user service managers.
fn counts_as_login(class: &str) -> bool {
    !class.starts_with("manager")
}

/// The polkit actions logind checks before `action`, in its order, and the flags for System1's call as root. `Err`
/// when a "block" inhibitor stops it and the caller did not ask to skip inhibitors.
fn power_plan(
    action: PowerAction,
    other_sessions: bool,
    block: Block,
    skip_inhibitors: bool,
) -> std::result::Result<(Vec<String>, u64), String> {
    if block == Block::Strong && !skip_inhibitors {
        return Err(format!("{} is blocked by an inhibitor", action.polkit()));
    }
    let blocked = block != Block::None;
    let stem = action.polkit();
    let mut checks = Vec::new();
    if other_sessions {
        checks.push(format!("org.freedesktop.login1.{stem}-multiple-sessions"));
    }
    if blocked {
        checks.push(format!("org.freedesktop.login1.{stem}-ignore-inhibit"));
    }
    if checks.is_empty() {
        checks.push(format!("org.freedesktop.login1.{stem}"));
    }
    // logind refuses root a "block" inhibitor, and with ROOT_CHECK_INHIBITORS a "block-weak" one, that appears after
    // the checks; only a skip the caller asked for and passed ignore-inhibit for goes past them.
    let flags = match (block, skip_inhibitors) {
        (Block::None, _) => ROOT_CHECK_INHIBITORS,
        (Block::Weak, false) => 0,
        _ => SKIP_INHIBITORS,
    };
    Ok((checks, flags))
}

/// logind's `Can*` word for the answers to each needed action.
fn can_answer(answers: &[Auth]) -> &'static str {
    if answers.contains(&Auth::No) {
        "no"
    } else if answers.contains(&Auth::Challenge) {
        "challenge"
    } else {
        "yes"
    }
}

/// logind's polkit action for an inhibitor of one `kind` in `mode`. Only shutdown and sleep: logind applies idle and
/// handle-* inhibitors only while their owner's session is active, and System1 has no session, so logind would apply
/// them in every session and the greeter. Callers take those from logind directly.
fn inhibit_action(kind: &str, mode: &str) -> Option<String> {
    match kind {
        "shutdown" | "sleep" => {
            let mode = if mode == "delay" { "delay" } else { "block" };
            Some(format!("org.freedesktop.login1.inhibit-{mode}-{kind}"))
        }
        _ => None,
    }
}

/// The power action of a shutdown that `ScheduleShutdown` takes, in logind's names.
fn schedulable(action: &str) -> Result<PowerAction> {
    match action {
        "poweroff" => Ok(PowerAction::PowerOff),
        "reboot" => Ok(PowerAction::Reboot),
        _ => Err(Error::InvalidArgs(format!("cannot schedule {action:?}"))),
    }
}

/// logind's polkit action for cancelling a shutdown that another uid scheduled, by its scheduled action name.
fn cancel_action(scheduled: &str) -> Option<&'static str> {
    match scheduled.strip_prefix("dry-").unwrap_or(scheduled) {
        "poweroff" => Some("org.freedesktop.login1.power-off"),
        "reboot" | "kexec" | "soft-reboot" => Some("org.freedesktop.login1.reboot"),
        "halt" => Some("org.freedesktop.login1.halt"),
        _ => None,
    }
}

fn failed(what: &str, e: impl Display) -> Error {
    Error::Failed(format!("logind {what}: {e}"))
}

async fn call<B: serde::Serialize + DynamicType>(
    conn: &Connection,
    path: &str,
    interface: &str,
    method: &str,
    body: &B,
) -> Result<zbus::Message> {
    conn.call_method(Some(LOGIN1), path, Some(interface), method, body).await.map_err(|e| failed(method, e))
}

async fn manager<B, R>(conn: &Connection, method: &str, body: &B) -> Result<R>
where
    B: serde::Serialize + DynamicType,
    R: serde::de::DeserializeOwned + Type,
{
    call(conn, LOGIN1_PATH, MANAGER, method, body).await?.body().deserialize::<R>().map_err(|e| failed(method, e))
}

async fn property<R>(conn: &Connection, path: &str, interface: &str, name: &str) -> Result<R>
where
    R: TryFrom<OwnedVariant>,
    R::Error: Display,
{
    let reply = call(conn, path, "org.freedesktop.DBus.Properties", "Get", &(interface, name)).await?;
    let value: OwnedVariant = reply.body().deserialize().map_err(|e| failed(name, e))?;
    R::try_from(value).map_err(|e| failed(name, e))
}

/// The caller's uid and pid.
async fn caller(conn: &Connection, header: &Header<'_>) -> Result<(u32, u32)> {
    let sender = header.sender().ok_or_else(|| Error::Failed("call without sender".into()))?;
    let dbus = zbus::fdo::DBusProxy::new(conn).await?;
    let uid = dbus.get_connection_unix_user(BusName::Unique(sender.clone())).await?;
    let pid = dbus.get_connection_unix_process_id(BusName::Unique(sender.clone())).await?;
    Ok((uid, pid))
}

/// Whether a user other than `uid` has a login session; logind then asks for the multiple-sessions action.
async fn other_sessions(conn: &Connection, uid: u32) -> Result<bool> {
    let sessions: Vec<SessionRow> = manager(conn, "ListSessions", &()).await?;
    for (_, _, _, _, path) in sessions.iter().filter(|row| row.1 != uid) {
        // A session that ends meanwhile still counts: more authorization, never less.
        if property::<String>(conn, path.as_str(), SESSION, "Class").await.map_or(true, |c| counts_as_login(&c)) {
            return Ok(true);
        }
    }
    Ok(false)
}

/// `power_plan` for the caller with logind's current inhibitors and sessions.
async fn power_plan_for(
    conn: &Connection,
    uid: u32,
    action: PowerAction,
    skip_inhibitors: bool,
) -> Result<std::result::Result<(Vec<String>, u64), String>> {
    let inhibitors: Vec<Inhibitor> = manager(conn, "ListInhibitors", &()).await?;
    let others = other_sessions(conn, uid).await?;
    Ok(power_plan(action, others, block(&inhibitors, action.what(), uid), skip_inhibitors))
}

/// `session`, or with an empty one the session of process `pid`, else the display session of user `uid`.
async fn session_path(conn: &Connection, session: &str, uid: u32, pid: u32) -> Result<OwnedObjectPath> {
    if !session.is_empty() {
        return manager(conn, "GetSession", &(session,)).await;
    }
    if let Ok(path) = manager::<_, OwnedObjectPath>(conn, "GetSessionByPID", &(pid,)).await {
        return Ok(path);
    }
    let user: OwnedObjectPath = manager(conn, "GetUser", &(uid,)).await?;
    let (_, display): (String, OwnedObjectPath) = property(conn, user.as_str(), USER, "Display").await?;
    if display.as_str() == "/" { Err(Error::Failed(format!("user {uid} has no session"))) } else { Ok(display) }
}

/// Makes logind's polkit checks for `action` for the caller and returns the flags for System1's call as root.
async fn authorize_power(
    conn: &Connection,
    header: &Header<'_>,
    action: PowerAction,
    interactive: bool,
    skip_inhibitors: bool,
) -> Result<(u32, u64)> {
    let (uid, _) = caller(conn, header).await?;
    let mut passed: Vec<String> = Vec::new();
    // Inhibitors and sessions can change while a polkit dialog is open: plan again until no new check is due.
    loop {
        let (checks, flags) = power_plan_for(conn, uid, action, skip_inhibitors).await?.map_err(Error::AccessDenied)?;
        let due: Vec<String> = checks.into_iter().filter(|check| !passed.contains(check)).collect();
        if due.is_empty() {
            return Ok((uid, flags));
        }
        for check in due {
            polkit::check_with(conn, header, &check, interactive).await?;
            passed.push(check);
        }
    }
}

async fn scheduled_shutdown(conn: &Connection) -> Result<String> {
    let (action, _): (String, u64) = property(conn, LOGIN1_PATH, MANAGER, "ScheduledShutdown").await?;
    Ok(action)
}

#[zbus::proxy(
    interface = "org.freedesktop.login1.Manager",
    default_service = "org.freedesktop.login1",
    default_path = "/org/freedesktop/login1"
)]
trait Login1 {
    #[zbus(signal)]
    fn prepare_for_sleep(&self, start: bool) -> zbus::Result<()>;
}

/// Relays logind's `PrepareForSleep` and the names of changed mirrored properties until the connection closes.
pub async fn run(conn: Connection) {
    if let Err(e) = relay(&conn).await {
        log::warn!("login: logind signal relay stopped: {e}");
    }
}

async fn relay(conn: &Connection) -> zbus::Result<()> {
    // Proxies of a well-known name take signals only from its current owner, so no other peer can fake one.
    let login1 = Login1Proxy::builder(conn).cache_properties(CacheProperties::No).build().await?;
    let props = zbus::fdo::PropertiesProxy::builder(conn)
        .destination(LOGIN1)
        .path(LOGIN1_PATH)
        .cache_properties(CacheProperties::No)
        .build()
        .await?;
    let mut sleep = login1.receive_prepare_for_sleep().await?;
    let mut changes = props.receive_properties_changed().await?;
    let iface: InterfaceRef<Login> = conn.object_server().interface(PATH).await?;
    let emitter = iface.signal_emitter();
    loop {
        // One bad signal must not end the relay: the shell would miss every later PrepareForSleep.
        let sent = tokio::select! {
            Some(signal) = sleep.next() => match signal.args() {
                Ok(args) => Login::prepare_for_sleep(emitter, *args.start()).await,
                Err(e) => Err(e),
            },
            Some(signal) = changes.next() => match signal.args() {
                Ok(args) if args.interface_name().as_str() == MANAGER => {
                    let names: Vec<&str> = MIRRORED
                        .into_iter()
                        .filter(|name| {
                            args.changed_properties().contains_key(name) || args.invalidated_properties().contains(name)
                        })
                        .collect();
                    if names.is_empty() { Ok(()) } else { Login::manager_properties_changed(emitter, names).await }
                }
                Ok(_) => Ok(()),
                Err(e) => Err(e),
            },
            else => return Ok(()),
        };
        if let Err(e) = sent {
            log::warn!("login: logind signal not relayed: {e}");
        }
    }
}

pub struct Login;

#[interface(name = "org.andromeda.System1.Login")]
impl Login {
    /// What logind answers the caller for `action`: yes, challenge (a password), no, or na (not on this machine).
    /// Actions: reboot, power-off, suspend, hibernate, suspend-then-hibernate, reboot-to-firmware-setup.
    async fn can_power(
        &self,
        action: String,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> Result<String> {
        if action == FIRMWARE {
            let root: String = manager(conn, "CanRebootToFirmwareSetup", &()).await?;
            if root != "yes" {
                return Ok(root);
            }
            return Ok(can_answer(&[polkit::test(conn, &header, FIRMWARE_ACTION).await?]).into());
        }
        let action = PowerAction::parse(&action)?;
        let root: String = manager(conn, action.can_method(), &()).await?;
        if root != "yes" {
            return Ok(root);
        }
        let (uid, _) = caller(conn, &header).await?;
        let (checks, _) = power_plan_for(conn, uid, action, true).await?.map_err(Error::Failed)?;
        let mut answers = Vec::with_capacity(checks.len());
        for check in &checks {
            answers.push(polkit::test(conn, &header, check).await?);
        }
        Ok(can_answer(&answers).into())
    }

    /// Starts `action` (one of `CanPower`'s, without reboot-to-firmware-setup) after logind's polkit checks for the
    /// caller. `interactive` lets polkit ask for a password; `skip_inhibitors` overrides block inhibitors, the
    /// caller's own too, which needs the action's ignore-inhibit authorization. Delay inhibitors still apply.
    async fn power(
        &self,
        action: String,
        interactive: bool,
        skip_inhibitors: bool,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> Result<()> {
        let action = PowerAction::parse(&action)?;
        let (uid, flags) = authorize_power(conn, &header, action, interactive, skip_inhibitors).await?;
        call(conn, LOGIN1_PATH, MANAGER, action.method(), &(flags,)).await?;
        log::info!("login: {} for uid {uid}", action.method());
        Ok(())
    }

    /// Schedules logind's `action` (poweroff or reboot) at `usec`, `CLOCK_REALTIME` microseconds, after the same polkit
    /// checks as `Power` without skipping inhibitors.
    async fn schedule_shutdown(
        &self,
        action: String,
        usec: u64,
        interactive: bool,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> Result<()> {
        authorize_power(conn, &header, schedulable(&action)?, interactive, false).await?;
        call(conn, LOGIN1_PATH, MANAGER, "ScheduleShutdown", &(&action, usec)).await?;
        Ok(())
    }

    /// Cancels the scheduled shutdown; false when none was scheduled. logind records uid 0 as the owner of a shutdown
    /// that System1 scheduled, so every caller needs the polkit action that logind asks of a non-owner.
    async fn cancel_scheduled_shutdown(
        &self,
        interactive: bool,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> Result<bool> {
        let action = scheduled_shutdown(conn).await?;
        if action.is_empty() {
            return Ok(false);
        }
        let check =
            cancel_action(&action).ok_or_else(|| Error::AccessDenied(format!("cannot cancel scheduled {action:?}")))?;
        polkit::check_with(conn, &header, check, interactive).await?;
        // A different shutdown scheduled during the polkit dialog was not authorized.
        if scheduled_shutdown(conn).await? != action {
            return Err(Error::AccessDenied("the scheduled shutdown changed".into()));
        }
        manager(conn, "CancelScheduledShutdown", &()).await
    }

    /// logind's `PrepareForSleep`: true before suspend or hibernate, false after resume or an aborted sleep.
    #[zbus(signal)]
    async fn prepare_for_sleep(emitter: &SignalEmitter<'_>, start: bool) -> zbus::Result<()>;

    /// The names of mirrored logind properties that changed. Read the values with `ManagerProperties`: any peer can
    /// send this signal straight to a reader. logind changes the Handle* settings on a reload without a signal.
    #[zbus(signal)]
    async fn manager_properties_changed(emitter: &SignalEmitter<'_>, names: Vec<&str>) -> zbus::Result<()>;

    /// logind's Manager properties that the shell reads, read live: `BlockInhibited`, `HandleLidSwitch`,
    /// `HandleLidSwitchDocked`, `HandleLidSwitchExternalPower`, `HandlePowerKey`, `PreparingForShutdown`,
    /// `PreparingForSleep` and `ScheduledShutdown`.
    async fn manager_properties(&self, #[zbus(connection)] conn: &Connection) -> Result<HashMap<String, OwnedVariant>> {
        let reply = call(conn, LOGIN1_PATH, "org.freedesktop.DBus.Properties", "GetAll", &(MANAGER,)).await?;
        let mut all: HashMap<String, OwnedVariant> = reply.body().deserialize().map_err(|e| failed("GetAll", e))?;
        all.retain(|name, _| MIRRORED.contains(&name.as_str()));
        Ok(all)
    }

    /// Sets or clears the request to boot into the firmware setup; needs logind's set-reboot-to-firmware-setup.
    async fn set_reboot_to_firmware_setup(
        &self,
        enable: bool,
        interactive: bool,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> Result<()> {
        polkit::check_with(conn, &header, FIRMWARE_ACTION, interactive).await?;
        call(conn, LOGIN1_PATH, MANAGER, "SetRebootToFirmwareSetup", &(enable,)).await?;
        Ok(())
    }

    /// Takes a logind shutdown or sleep inhibitor after logind's polkit check for each kind in `what`
    /// (colon-separated). The inhibitor lasts while the caller keeps the returned fd open.
    async fn inhibit(
        &self,
        what: String,
        who: String,
        why: String,
        mode: String,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> Result<OwnedFd> {
        for kind in what.split(':') {
            let action = inhibit_action(kind, &mode)
                .ok_or_else(|| Error::InvalidArgs(format!("inhibitor kind {kind:?} is not available here")))?;
            polkit::check_with(conn, &header, &action, false).await?;
        }
        manager(conn, "Inhibit", &(&what, &who, &why, &mode)).await
    }

    /// logind's inhibitors: (what, who, why, mode, uid, pid).
    async fn list_inhibitors(&self, #[zbus(connection)] conn: &Connection) -> Result<Vec<Inhibitor>> {
        manager(conn, "ListInhibitors", &()).await
    }

    /// logind's sessions: (id, uid, user, seat, path).
    async fn list_sessions(&self, #[zbus(connection)] conn: &Connection) -> Result<Vec<SessionRow>> {
        manager(conn, "ListSessions", &()).await
    }

    /// Sets a backlight or LED (logind's `Session.SetBrightness`) for `session`, which must be the caller's. An empty
    /// `session` means the session of the caller's process, else its user's display session.
    async fn set_brightness(
        &self,
        session: String,
        subsystem: String,
        name: String,
        value: u32,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> Result<()> {
        let (uid, pid) = caller(conn, &header).await?;
        let path = session_path(conn, &session, uid, pid).await?;
        let (owner, _): (u32, OwnedObjectPath) = property(conn, path.as_str(), SESSION, "User").await?;
        if owner != uid {
            return Err(Error::AccessDenied(format!("session {} is not of uid {uid}", path.as_str())));
        }
        call(conn, path.as_str(), SESSION, "SetBrightness", &(&subsystem, &name, value)).await?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn inhibitor(what: &str, mode: &str, uid: u32) -> Inhibitor {
        (what.into(), "app".into(), "why".into(), mode.into(), uid, 1)
    }

    #[test]
    fn block_inhibitors_stop_their_owner_and_weak_ones_do_not() {
        let own = [inhibitor("shutdown:sleep", "block", 1000)];
        assert_eq!(block(&own, "shutdown", 1000), Block::Strong);
        assert_eq!(block(&own, "shutdown", 1001), Block::Strong);
        assert_eq!(block(&[inhibitor("sleep", "block-weak", 1000)], "sleep", 1000), Block::None);
        assert_eq!(block(&[inhibitor("idle:sleep", "block-weak", 0)], "sleep", 1000), Block::Weak);
        assert_eq!(block(&[inhibitor("sleep", "delay", 0)], "sleep", 1000), Block::None);
        assert_eq!(block(&[inhibitor("sleep", "block", 0)], "shutdown", 1000), Block::None);
        let both = [inhibitor("sleep", "block-weak", 0), inhibitor("sleep", "block", 0)];
        assert_eq!(block(&both, "sleep", 1000), Block::Strong);
    }

    #[test]
    fn blocked_action_without_skip_is_refused() {
        assert!(power_plan(PowerAction::Reboot, false, Block::Strong, false).is_err());
    }

    #[test]
    fn power_checks_follow_logind() {
        let checks = |a, others, block| power_plan(a, others, block, true).map(|p| p.0).unwrap_or_default();
        assert_eq!(checks(PowerAction::PowerOff, false, Block::None), ["org.freedesktop.login1.power-off"]);
        assert_eq!(checks(PowerAction::Reboot, false, Block::Strong), ["org.freedesktop.login1.reboot-ignore-inhibit"]);
        assert_eq!(
            checks(PowerAction::Suspend, true, Block::Weak),
            ["org.freedesktop.login1.suspend-multiple-sessions", "org.freedesktop.login1.suspend-ignore-inhibit"]
        );
        assert_eq!(checks(PowerAction::SuspendThenHibernate, false, Block::None), ["org.freedesktop.login1.hibernate"]);
    }

    #[test]
    fn logind_skips_inhibitors_only_on_an_authorized_request() {
        let flags = |block, skip| power_plan(PowerAction::Reboot, false, block, skip).map(|p| p.1);
        assert_eq!(flags(Block::None, true), Ok(ROOT_CHECK_INHIBITORS));
        assert_eq!(flags(Block::None, false), Ok(ROOT_CHECK_INHIBITORS));
        assert_eq!(flags(Block::Weak, false), Ok(0));
        assert_eq!(flags(Block::Weak, true), Ok(SKIP_INHIBITORS));
        assert_eq!(flags(Block::Strong, true), Ok(SKIP_INHIBITORS));
    }

    #[test]
    fn any_denied_check_answers_no() {
        assert_eq!(can_answer(&[Auth::Yes, Auth::Challenge]), "challenge");
        assert_eq!(can_answer(&[Auth::Challenge, Auth::No]), "no");
        assert_eq!(can_answer(&[Auth::Yes]), "yes");
    }

    #[test]
    fn inhibitors_need_logind_actions() {
        assert_eq!(inhibit_action("sleep", "delay").as_deref(), Some("org.freedesktop.login1.inhibit-delay-sleep"));
        assert_eq!(
            inhibit_action("shutdown", "block-weak").as_deref(),
            Some("org.freedesktop.login1.inhibit-block-shutdown")
        );
        assert_eq!(inhibit_action("handle-power-key", "block"), None);
        assert_eq!(inhibit_action("idle", "block"), None);
        assert_eq!(inhibit_action("everything", "block"), None);
    }

    #[test]
    fn cancelling_needs_the_scheduled_actions_polkit_action() {
        assert_eq!(cancel_action("poweroff"), Some("org.freedesktop.login1.power-off"));
        assert_eq!(cancel_action("dry-reboot"), Some("org.freedesktop.login1.reboot"));
        assert_eq!(cancel_action("kexec"), Some("org.freedesktop.login1.reboot"));
        assert_eq!(cancel_action("halt"), Some("org.freedesktop.login1.halt"));
        assert_eq!(cancel_action("something-new"), None);
        assert!(schedulable("halt").is_err());
        assert_eq!(schedulable("poweroff").ok(), Some(PowerAction::PowerOff));
    }

    #[test]
    fn user_managers_are_no_logins() {
        assert!(!counts_as_login("manager"));
        assert!(!counts_as_login("manager-early"));
        assert!(counts_as_login("user"));
        assert!(counts_as_login("greeter"));
    }
}
