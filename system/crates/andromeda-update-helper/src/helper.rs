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

use crate::transaction::{self, Event};

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
        authorize(conn, &header, if dry_run { "org.andromeda.update.preview" } else { "org.andromeda.update.upgrade" })
            .await?;
        if !dry_run {
            self.news_gate().await?;
        }
        if self.running.load(Ordering::SeqCst) {
            return Err(zbus::fdo::Error::Failed("an update already runs".into()));
        }
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
        let work = tokio::task::spawn_blocking(move || transaction::upgrade(dry_run, &tx));
        let this = self.clone();
        let emitter = emitter.to_owned();
        tokio::spawn(async move {
            while let Some(Event::Progress(stage, percent, package)) = rx.recv().await {
                let _ = Self::progress(&emitter, &stage, percent, &package).await;
            }
            let (ok, message, targets, questions) = match work.await {
                Ok(Ok(outcome)) if outcome.questions.is_empty() => {
                    let message = if outcome.committed { "updated" } else { "nothing committed" };
                    (true, message.to_owned(), outcome.targets, Vec::new())
                }
                Ok(Ok(outcome)) => (false, "needs a choice".to_owned(), outcome.targets, outcome.questions),
                Ok(Err(e)) => (false, format!("{e:#}"), Vec::new(), Vec::new()),
                Err(e) => (false, e.to_string(), Vec::new(), Vec::new()),
            };
            log::info!("upgrade (dry run {dry_run}): {message}");
            drop(inhibitor);
            this.running.store(false, Ordering::SeqCst);
            let _ = this.running_changed(&emitter).await;
            let _ = Self::finished(&emitter, ok, &message, targets, questions).await;
        });
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

    /// `targets`: "name version" to install, "-name" to remove. `questions`: non-empty when libalpm asked something
    /// (nothing was committed).
    #[zbus(signal)]
    async fn finished(
        emitter: &SignalEmitter<'_>,
        ok: bool,
        message: &str,
        targets: Vec<String>,
        questions: Vec<String>,
    ) -> zbus::Result<()>;
}
