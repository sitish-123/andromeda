//! `org.andromeda.UpdateHelper1` on the system bus: a closed set of root actions for the Software Update page, each
//! behind its own polkit action (`auth_admin` every time for upgrades). Arguments are bounded; no package names or
//! commands from the caller.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use andromeda_update::news;
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::{Connection, interface};
use zbus_polkit::policykit1::{AuthorityProxy, CheckAuthorizationFlags, Subject};

use crate::migrations;
use crate::snapshot;
use crate::transaction::{self, Event, Scope};

pub const PATH: &str = "/org/andromeda/UpdateHelper1";

#[derive(Clone)]
pub struct Helper {
    pub state_dir: PathBuf,
    pub running: Arc<AtomicBool>,
    /// Seconds since start of the last method call, for the idle exit.
    pub last_call: Arc<std::sync::atomic::AtomicU64>,
    pub started: std::time::Instant,
}

#[zbus::proxy(
    interface = "org.freedesktop.login1.Manager",
    default_service = "org.freedesktop.login1",
    default_path = "/org/freedesktop/login1"
)]
trait Login {
    fn inhibit(&self, what: &str, who: &str, why: &str, mode: &str) -> zbus::Result<zbus::OwnedFd>;
    fn list_sessions(&self) -> zbus::Result<Vec<(String, u32, String, String, zbus::OwnedObjectPath)>>;
}

#[zbus::proxy(
    interface = "org.freedesktop.systemd1.Manager",
    default_service = "org.freedesktop.systemd1",
    default_path = "/org/freedesktop/systemd1"
)]
trait Systemd {
    fn start_unit(&self, name: &str, mode: &str) -> zbus::Result<zbus::OwnedObjectPath>;
    fn stop_unit(&self, name: &str, mode: &str) -> zbus::Result<zbus::OwnedObjectPath>;
}

const TONIGHT_TIMER: &str = "andromeda-update-tonight.timer";

/// The 02:00 timer wakes the machine, so it runs only while an update is scheduled.
async fn set_tonight_timer(conn: &Connection, on: bool) {
    let result = match SystemdProxy::new(conn).await {
        Ok(systemd) if on => systemd.start_unit(TONIGHT_TIMER, "replace").await.map(drop),
        Ok(systemd) => systemd.stop_unit(TONIGHT_TIMER, "replace").await.map(drop),
        Err(e) => Err(e),
    };
    if let Err(e) = result {
        log::warn!("tonight: cannot {} {TONIGHT_TIMER}: {e}", if on { "start" } else { "stop" });
    }
}

fn now_secs() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| i64::try_from(d.as_secs()).unwrap_or(i64::MAX))
}

const TONIGHT_MAX_AGE: i64 = 24 * 3600;

const MAX_QUESTIONS: usize = 256;
const MAX_QUESTION: usize = 4096;

/// The user's "yes" to `question`: it is one of the `accepted` questions they saw.
pub fn accepts(accepted: &[String], question: &str) -> bool {
    accepted.iter().any(|a| a == question)
}

/// A run may commit when the user accepted every question it asked (at prepare or at commit); a run that asks
/// nothing may too.
pub fn consented(questions: &[String], accepted: &[String]) -> bool {
    questions.iter().all(|q| accepts(accepted, q))
}

#[zbus::proxy(interface = "org.freedesktop.login1.Session", default_service = "org.freedesktop.login1")]
trait Session {
    #[zbus(property)]
    fn locked_hint(&self) -> zbus::Result<bool>;
    #[zbus(property, name = "Type")]
    fn kind(&self) -> zbus::Result<String>;
    #[zbus(property)]
    fn class(&self) -> zbus::Result<String>;
    #[zbus(property)]
    fn state(&self) -> zbus::Result<String>;
}

/// True when every open graphical user session is locked (the greeter, closing sessions and none at all count as
/// locked: nobody's work is open there).
async fn all_sessions_locked(conn: &Connection) -> bool {
    let Ok(login) = LoginProxy::new(conn).await else { return false };
    let Ok(sessions) = login.list_sessions().await else { return false };
    for (_, _, _, _, path) in sessions {
        let Ok(session) = SessionProxy::builder(conn).path(path).build().await else { return false };
        let graphical = matches!(session.kind().await.as_deref(), Ok("wayland" | "x11"));
        let user = session.class().await.is_ok_and(|c| c == "user");
        let closing = session.state().await.is_ok_and(|st| st == "closing");
        if graphical && user && !closing && !session.locked_hint().await.unwrap_or(false) {
            return false;
        }
    }
    true
}

async fn authorize(conn: &Connection, header: &Header<'_>, action: &str) -> zbus::fdo::Result<()> {
    let subject = Subject::new_for_message_header(header).map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;
    let authority = AuthorityProxy::new(conn).await?;
    let result = authority
        .check_authorization(&subject, action, &HashMap::new(), CheckAuthorizationFlags::ALLOW_USER_INTERACTION, "")
        .await?;
    if result.is_authorized { Ok(()) } else { Err(zbus::fdo::Error::AccessDenied(format!("not authorized: {action}"))) }
}

impl Helper {
    /// After authorization: the news gate, the inhibitor, then the transaction on its own thread.
    async fn start(
        &self,
        scope: Scope,
        dry_run: bool,
        accepted: Vec<String>,
        conn: &Connection,
        emitter: SignalEmitter<'_>,
    ) -> zbus::fdo::Result<()> {
        // The Arch news is about Arch packages; an Andromeda-only update is not gated by it.
        if !dry_run && scope == Scope::All {
            self.news_gate().await?;
        }
        self.refuse_if_running()?;
        // An upgrade never runs without the inhibitor: a power-off mid-transaction can break the system.
        let inhibitor = if dry_run {
            None
        } else {
            let login = LoginProxy::new(conn).await?;
            Some(login.inhibit("shutdown:sleep", "Andromeda", "Updating the system", "block").await?)
        };
        if self.running.swap(true, Ordering::SeqCst) {
            return Err(zbus::fdo::Error::Failed("an update already runs".into()));
        }
        let _ = self.running_changed(&emitter).await;
        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel();
        let state_dir = self.state_dir.clone();
        let runtime = tokio::runtime::Handle::current();
        let snapper_conn = conn.clone();
        let consent = !accepted.is_empty();
        let work = tokio::task::spawn_blocking(move || -> anyhow::Result<_> {
            // The pre snapshot is taken only once the transaction is sure to commit.
            let mut pre = None;
            let outcome = transaction::upgrade(scope, dry_run, &accepted, &tx, &mut || {
                pre = runtime
                    .block_on(snapshot::pre(&snapper_conn))
                    .map_err(|e| anyhow::anyhow!("the snapshot before the update failed: {e}"))?;
                Ok(())
            });
            if let Some(pre) = pre {
                runtime.block_on(snapshot::post(&snapper_conn, pre));
            }
            // Also after a failed or empty transaction: scripts a partial commit or a plain pacman run brought in.
            let migrated = if dry_run {
                Ok(Vec::new())
            } else {
                migrations::run(std::path::Path::new(migrations::DIR), &state_dir, true)
            };
            Ok((outcome?, migrated))
        });
        let this = self.clone();
        let emitter = emitter.to_owned();
        tokio::spawn(async move {
            while let Some(Event::Progress(stage, percent, package)) = rx.recv().await {
                let _ = Self::progress(&emitter, &stage, percent, &package).await;
            }
            let (ok, message, targets, questions) = match work.await {
                Ok(Ok((outcome, Err(e)))) => (false, format!("updated, but {e:#}"), outcome.targets, Vec::new()),
                Ok(Ok((outcome, Ok(_)))) if outcome.committed => {
                    (true, "updated".to_owned(), outcome.targets, Vec::new())
                }
                Ok(Ok((outcome, Ok(_)))) if !outcome.questions.is_empty() => {
                    let message = if consent { "the plan changed: nothing was changed" } else { "needs a choice" };
                    (false, message.to_owned(), outcome.targets, outcome.questions)
                }
                // A preview, or nothing to update.
                Ok(Ok((outcome, Ok(_)))) if dry_run || outcome.targets.is_empty() => {
                    (true, "nothing committed".to_owned(), outcome.targets, Vec::new())
                }
                Ok(Ok((outcome, Ok(_)))) => (false, "nothing was committed".to_owned(), outcome.targets, Vec::new()),
                Ok(Err(e)) => (false, format!("{e:#}"), Vec::new(), Vec::new()),
                Err(e) => (false, e.to_string(), Vec::new(), Vec::new()),
            };
            log::info!("upgrade (dry run {dry_run}): {message}");
            drop(inhibitor);
            this.running.store(false, Ordering::SeqCst);
            let _ = this.running_changed(&emitter).await;
            let _ = Self::finished(&emitter, ok, &message, targets, questions, scope == Scope::Andromeda).await;
        });
        Ok(())
    }

    /// Checked before polkit too, so a second request fails without a password prompt.
    fn refuse_if_running(&self) -> zbus::fdo::Result<()> {
        if self.running.load(Ordering::SeqCst) {
            return Err(zbus::fdo::Error::Failed("an update already runs".into()));
        }
        Ok(())
    }

    fn tonight_file(&self) -> PathBuf {
        self.state_dir.join("tonight")
    }

    /// When the user scheduled tonight's run (unix seconds), if they did.
    fn tonight_token(&self) -> Option<i64> {
        std::fs::read_to_string(self.tonight_file()).ok().and_then(|t| t.trim().parse().ok())
    }

    fn touch(&self) {
        self.last_call.store(self.started.elapsed().as_secs(), Ordering::Relaxed);
    }

    fn token_file(&self) -> PathBuf {
        self.state_dir.join("news-read")
    }

    fn read_token(&self) -> i64 {
        std::fs::read_to_string(self.token_file()).ok().and_then(|t| t.trim().parse().ok()).unwrap_or(0)
    }

    /// Unread news blocks an upgrade; so does a feed that cannot be read.
    async fn news_gate(&self) -> zbus::fdo::Result<()> {
        if !news::GATES {
            return Ok(());
        }
        let items = tokio::task::spawn_blocking(news::fetch)
            .await
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?
            .map_err(|e| zbus::fdo::Error::Failed(format!("Arch news could not be read: {e:#}")))?;
        let unread = news::unread(&items, self.read_token());
        if let Some(item) = unread.first() {
            return Err(zbus::fdo::Error::Failed(format!("unread Arch news: {}", item.title)));
        }
        Ok(())
    }
}

#[interface(name = "org.andromeda.UpdateHelper1")]
impl Helper {
    #[zbus(property)]
    fn running(&self) -> bool {
        self.running.load(Ordering::Relaxed)
    }

    /// Starts a full system upgrade (`dry_run`: list what it would do, from the current databases, change nothing).
    /// Returns at once; `Progress` signals follow, then one `Finished`. Refused while unread Arch news exists.
    async fn upgrade(
        &self,
        dry_run: bool,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> zbus::fdo::Result<()> {
        self.touch();
        self.refuse_if_running()?;
        authorize(conn, &header, if dry_run { "org.andromeda.update.preview" } else { "org.andromeda.update.upgrade" })
            .await?;
        self.start(Scope::All, dry_run, Vec::new(), conn, emitter).await
    }

    /// Starts an update of the Andromeda repository packages only. Not gated by the Arch news. Fails with a message
    /// that starts "needs a system update first" when they need newer system packages (a partial upgrade).
    async fn upgrade_andromeda(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> zbus::fdo::Result<()> {
        self.touch();
        self.refuse_if_running()?;
        authorize(conn, &header, "org.andromeda.update.upgrade").await?;
        self.start(Scope::Andromeda, false, Vec::new(), conn, emitter).await
    }

    /// The user's "yes" to the `questions` a `Finished` listed: runs that update again (`andromeda`: the
    /// Andromeda-only one) and answers them yes. Commits only when every question the run asks is one of these;
    /// any other gets the safe answer and `Finished` lists the run's questions with the message "the plan changed:
    /// nothing was changed".
    async fn upgrade_accepting(
        &self,
        andromeda: bool,
        questions: Vec<String>,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> zbus::fdo::Result<()> {
        self.touch();
        if questions.is_empty() || questions.len() > MAX_QUESTIONS || questions.iter().any(|q| q.len() > MAX_QUESTION) {
            return Err(zbus::fdo::Error::InvalidArgs("questions: 1 to 256, each at most 4096 bytes".into()));
        }
        self.refuse_if_running()?;
        authorize(conn, &header, "org.andromeda.update.upgrade").await?;
        let scope = if andromeda { Scope::Andromeda } else { Scope::All };
        self.start(scope, false, questions, conn, emitter).await
    }

    /// The user authenticates now for tonight's 02:00 run (`andromeda-update-tonight.timer`); `false` cancels.
    async fn schedule_tonight(
        &self,
        enable: bool,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> zbus::fdo::Result<()> {
        self.touch();
        // Cancelling someone's scheduled update needs the same right as scheduling it.
        authorize(conn, &header, "org.andromeda.update.upgrade").await?;
        if enable {
            std::fs::create_dir_all(&self.state_dir).map_err(|e| zbus::fdo::Error::IOError(e.to_string()))?;
            std::fs::write(self.tonight_file(), now_secs().to_string())
                .map_err(|e| zbus::fdo::Error::IOError(e.to_string()))?;
        } else {
            let _ = std::fs::remove_file(self.tonight_file());
        }
        set_tonight_timer(conn, enable).await;
        let _ = self.tonight_changed(&emitter).await;
        Ok(())
    }

    /// What snapshots the system before an update: "snap-pac", "snapper" or "" (none).
    #[zbus(property)]
    async fn snapshots(&self, #[zbus(connection)] conn: &Connection) -> String {
        snapshot::mode(conn).await.to_owned()
    }

    #[zbus(property)]
    fn tonight(&self) -> bool {
        self.tonight_token().is_some_and(|t| now_secs() - t <= TONIGHT_MAX_AGE)
    }

    /// Called by andromeda-update-tonight.service (root, on AC). Consumes the token; runs only while every graphical
    /// session is locked, so nobody's open work is interrupted.
    async fn run_tonight(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> zbus::fdo::Result<()> {
        self.touch();
        let sender = header.sender().ok_or_else(|| zbus::fdo::Error::AccessDenied("no sender".into()))?;
        let uid = zbus::fdo::DBusProxy::new(conn).await?.get_connection_unix_user(sender.clone().into()).await?;
        if uid != 0 {
            return Err(zbus::fdo::Error::AccessDenied("RunTonight is for the timer".into()));
        }
        let Some(scheduled) = self.tonight_token() else { return Ok(()) };
        let _ = std::fs::remove_file(self.tonight_file());
        set_tonight_timer(conn, false).await;
        let _ = self.tonight_changed(&emitter).await;
        // A token older than a day was for a night that passed (machine off or on battery then).
        if now_secs() - scheduled > TONIGHT_MAX_AGE {
            return Ok(());
        }
        // The token is gone now: every refusal is reported, or the page would only see "Tonight" switch off.
        let refused = if all_sessions_locked(conn).await {
            self.start(Scope::All, false, Vec::new(), conn, emitter.clone()).await.err().map(|e| e.to_string())
        } else {
            Some("a session was unlocked at 02:00".to_owned())
        };
        if let Some(reason) = refused {
            log::info!("tonight: not updating: {reason}");
            let message = format!("tonight's update did not run: {reason}");
            let _ = Self::finished(&emitter, false, &message, vec![], vec![], false).await;
        }
        Ok(())
    }

    /// "I have read this": Arch news up to `until` no longer blocks `Upgrade`; capped at the newest item in the feed.
    async fn mark_news_read(
        &self,
        until: i64,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        self.touch();
        authorize(conn, &header, "org.andromeda.update.news").await?;
        let items = tokio::task::spawn_blocking(news::fetch)
            .await
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?
            .map_err(|e| zbus::fdo::Error::Failed(format!("{e:#}")))?;
        let newest = items.iter().map(|i| i.published).max().unwrap_or(0);
        let token = until.min(newest).max(self.read_token());
        std::fs::create_dir_all(&self.state_dir).map_err(|e| zbus::fdo::Error::IOError(e.to_string()))?;
        std::fs::write(self.token_file(), token.to_string()).map_err(|e| zbus::fdo::Error::IOError(e.to_string()))?;
        Ok(())
    }

    #[zbus(signal)]
    async fn progress(emitter: &SignalEmitter<'_>, stage: &str, percent: i32, package: &str) -> zbus::Result<()>;

    /// `targets`: "name version" to install, "-name" to remove. `questions`: non-empty when the run asked something
    /// the user has not accepted (nothing was committed). `andromeda`: the run was the Andromeda-only update, the
    /// scope an `UpgradeAccepting` for these questions takes.
    #[zbus(signal)]
    async fn finished(
        emitter: &SignalEmitter<'_>,
        ok: bool,
        message: &str,
        targets: Vec<String>,
        questions: Vec<String>,
        andromeda: bool,
    ) -> zbus::Result<()>;
}

#[cfg(test)]
mod tests {
    use super::{accepts, consented};

    fn strings(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| (*s).to_owned()).collect()
    }

    #[test]
    fn consent_with_a_changed_plan_is_refused() {
        let seen = strings(&["remove a", "remove b"]);
        assert!(consented(&strings(&["remove b", "remove a"]), &seen));
        assert!(!consented(&strings(&["remove a", "remove b", "remove c"]), &seen));
        assert!(consented(&strings(&["remove a"]), &seen));
        assert!(consented(&[], &seen));
        assert!(!consented(&seen, &[]));
    }

    #[test]
    fn a_commit_time_question_in_accepted_is_answered_yes() {
        // The first run's commit asked for the key; the consent run's prepare asks nothing.
        let accepted = strings(&["import PGP key ABCD (Packager <p@example.org>)"]);
        assert!(consented(&[], &accepted));
        assert!(accepts(&accepted, "import PGP key ABCD (Packager <p@example.org>)"));
        assert!(!accepts(&accepted, "import PGP key EF01 (Other <o@example.org>)"));
        let asked = strings(&["import PGP key ABCD (Packager <p@example.org>)", "remove c"]);
        assert!(!consented(&asked, &accepted));
    }
}
