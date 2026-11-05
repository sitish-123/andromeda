//! `org.andromeda.Update1` on the session bus at `/org/andromeda/Update1`: one check of every source, its result as
//! properties, and the Arch news read token. Scheduling (daily, on AC, unmetered) belongs to the shell's client.

use std::path::PathBuf;
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use zbus::object_server::{InterfaceRef, SignalEmitter};
use zbus::{Connection, interface};

use andromeda_update::news;

use crate::{flatpak, fwupd, pacman};

pub const PATH: &str = "/org/andromeda/Update1";
/// checkupdates downloads every sync database; firmware and Flatpak answer faster.
const SOURCE_TIMEOUT: Duration = Duration::from_secs(180);

/// (group: system | apps | firmware, name, current version, new version, download bytes)
pub type Row = (String, String, String, String, u64);

#[derive(Default)]
struct State {
    updates: Vec<Row>,
    unread: Vec<news::Item>,
    /// A news fetch succeeded since start; until then the System group stays blocked.
    news_known: bool,
    last_check: i64,
    checking: bool,
    errors: Vec<String>,
}

#[derive(Clone)]
pub struct Update {
    cache: PathBuf,
    state_dir: PathBuf,
    state: Arc<Mutex<State>>,
}

fn now() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| i64::try_from(d.as_secs()).unwrap_or(i64::MAX))
}

async fn bounded<T>(what: &str, work: impl Future<Output = anyhow::Result<T>>) -> anyhow::Result<T> {
    tokio::time::timeout(SOURCE_TIMEOUT, work).await.unwrap_or_else(|_| Err(anyhow::anyhow!("{what}: timed out")))
}

impl Update {
    pub fn new(cache: PathBuf, state_dir: PathBuf) -> Self {
        Self { cache, state_dir, state: Arc::default() }
    }

    fn lock(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn token_file(&self) -> PathBuf {
        self.state_dir.join("news-read")
    }

    fn read_token(&self) -> i64 {
        std::fs::read_to_string(self.token_file()).ok().and_then(|t| t.trim().parse().ok()).unwrap_or(0)
    }

    async fn emit_all(iface: &InterfaceRef<Self>) {
        let this = iface.get().await;
        let emitter = iface.signal_emitter();
        let _ = this.updates_changed(emitter).await;
        let _ = this.news_changed(emitter).await;
        let _ = this.news_blocks_system_changed(emitter).await;
        let _ = this.errors_changed(emitter).await;
        let _ = this.last_check_changed(emitter).await;
        let _ = this.checking_changed(emitter).await;
    }

    async fn run_check(&self, conn: Connection) {
        let db = self.cache.join("checkup-db");
        let (repo, apps, firmware, feed) = tokio::join!(
            bounded("system", pacman::check(&db)),
            bounded("apps", flatpak::check()),
            bounded("firmware", async {
                let system = Connection::system().await?;
                fwupd::check(&system).await
            }),
            bounded("news", async { tokio::task::spawn_blocking(news::fetch).await? }),
        );
        let mut rows = Vec::new();
        let mut errors = Vec::new();
        match repo {
            Ok(list) => rows.extend(list.into_iter().map(|u| ("system".into(), u.name, u.old, u.new, u.size))),
            Err(e) => errors.push(format!("system: {e:#}")),
        }
        match apps {
            Ok(list) => rows.extend(list.into_iter().map(|u| ("apps".into(), u.id, String::new(), u.version, u.size))),
            Err(e) => errors.push(format!("apps: {e:#}")),
        }
        match firmware {
            Ok(list) => {
                rows.extend(list.into_iter().map(|u| ("firmware".into(), u.device, u.current, u.version, u.size)));
            }
            Err(e) => errors.push(format!("firmware: {e:#}")),
        }
        let token = self.read_token();
        {
            let mut state = self.lock();
            match feed {
                Ok(items) => {
                    state.unread = news::unread(&items, token);
                    state.news_known = true;
                }
                Err(e) => errors.push(format!("news: {e:#}")),
            }
            for error in &errors {
                log::warn!("{error}");
            }
            state.updates = rows;
            state.errors = errors;
            state.last_check = now();
            state.checking = false;
        }
        if let Ok(iface) = conn.object_server().interface::<_, Self>(PATH).await {
            Self::emit_all(&iface).await;
        }
    }
}

#[interface(name = "org.andromeda.Update1")]
impl Update {
    #[zbus(property)]
    fn updates(&self) -> Vec<Row> {
        self.lock().updates.clone()
    }

    /// Unread Arch news, newest first: (title, link, published unix seconds).
    #[zbus(property)]
    fn news(&self) -> Vec<(String, String, i64)> {
        self.lock().unread.iter().map(|i| (i.title.clone(), i.link.clone(), i.published)).collect()
    }

    /// True while unread news, or no successful news fetch yet, blocks the System group (the helper checks the
    /// same token before `Upgrade`).
    #[zbus(property)]
    fn news_blocks_system(&self) -> bool {
        let state = self.lock();
        !state.news_known || !state.unread.is_empty()
    }

    #[zbus(property)]
    fn last_check(&self) -> i64 {
        self.lock().last_check
    }

    #[zbus(property)]
    fn checking(&self) -> bool {
        self.lock().checking
    }

    /// One line per source that failed; the other sources still report.
    #[zbus(property)]
    fn errors(&self) -> Vec<String> {
        self.lock().errors.clone()
    }

    /// Starts a check of every source and returns at once; `Checking` turns false when the results are in. A call
    /// while a check runs does nothing.
    async fn check(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        {
            let mut state = self.lock();
            if state.checking {
                return Ok(());
            }
            state.checking = true;
        }
        let _ = self.checking_changed(&emitter).await;
        let this = self.clone();
        let conn = conn.clone();
        tokio::spawn(async move { this.run_check(conn).await });
        Ok(())
    }

    /// "I have read this": news published up to `until` (unix seconds) no longer blocks. `until` is capped at the
    /// newest unread item, so a call cannot silence news that has not appeared yet.
    async fn mark_news_read(
        &self,
        until: i64,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> zbus::fdo::Result<()> {
        let newest = self.lock().unread.iter().map(|i| i.published).max().unwrap_or(0);
        let token = until.min(newest).max(self.read_token());
        std::fs::create_dir_all(&self.state_dir).map_err(|e| zbus::fdo::Error::IOError(e.to_string()))?;
        std::fs::write(self.token_file(), token.to_string()).map_err(|e| zbus::fdo::Error::IOError(e.to_string()))?;
        self.lock().unread.retain(|i| i.published > token);
        let _ = self.news_changed(&emitter).await;
        let _ = self.news_blocks_system_changed(&emitter).await;
        Ok(())
    }
}
