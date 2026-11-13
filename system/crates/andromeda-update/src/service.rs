//! `org.andromeda.Update1` on the session bus at `/org/andromeda/Update1`: one check of every source, its result as
//! properties, and the Arch news read token. Scheduling (daily, on AC, unmetered) belongs to the shell's client.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use zbus::object_server::{InterfaceRef, SignalEmitter};
use zbus::{Connection, interface};

use andromeda_update::news;

use crate::{flatpak, fwupd, packages, snap};

pub const PATH: &str = "/org/andromeda/Update1";
/// checkupdates downloads every sync database; firmware and Flatpak answer faster.
const SOURCE_TIMEOUT: Duration = Duration::from_secs(180);

/// (group: andromeda | system | apps | snaps | firmware | held, name, current version, new version, download bytes)
pub type Row = (String, String, String, String, u64);

#[derive(Default)]
struct State {
    updates: Vec<Row>,
    unread: Vec<news::Item>,
    /// A news fetch succeeded since start; until then the System group stays blocked.
    news_known: bool,
    last_check: i64,
    run: Run,
    errors: Vec<String>,
}

#[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
enum Run {
    #[default]
    Idle,
    /// `scheduled`: the check came from the daily timer and may notify.
    Checking { scheduled: bool },
}

/// Owns the `Checking` state of one check; dropping it, also on panic, returns the state to `Idle`.
struct CheckGuard(Option<Arc<Mutex<State>>>);

impl CheckGuard {
    fn begin(state: &Arc<Mutex<State>>, scheduled: bool) -> Option<Self> {
        let mut s = state.lock().unwrap_or_else(PoisonError::into_inner);
        if s.run != Run::Idle {
            return None;
        }
        s.run = Run::Checking { scheduled };
        Some(Self(Some(state.clone())))
    }

    /// Returns to `Idle` under the caller's lock and tells whether the check was scheduled.
    fn finish(mut self, state: &mut State) -> bool {
        self.0 = None;
        matches!(std::mem::take(&mut state.run), Run::Checking { scheduled: true })
    }
}

impl Drop for CheckGuard {
    fn drop(&mut self) {
        if let Some(state) = self.0.take() {
            state.lock().unwrap_or_else(PoisonError::into_inner).run = Run::Idle;
        }
    }
}

#[derive(Clone)]
pub struct Update {
    cache: PathBuf,
    state_dir: PathBuf,
    state: Arc<Mutex<State>>,
}

/// NetworkManager's Metered: 1 yes, 3 guessed yes. No NetworkManager: not metered.
async fn metered() -> bool {
    let Ok(system) = zbus::Connection::system().await else { return false };
    let Ok(proxy) = zbus::Proxy::new(
        &system,
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager",
    )
    .await
    else {
        return false;
    };
    matches!(proxy.get_property::<u32>("Metered").await, Ok(1 | 3))
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

    /// A panicking check has already reset `run` through its guard; this tells clients so.
    fn spawn_check(&self, conn: Connection, guard: CheckGuard) {
        let this = self.clone();
        let task = tokio::spawn({
            let conn = conn.clone();
            async move { this.run_check(conn, guard).await }
        });
        tokio::spawn(async move {
            if task.await.is_err()
                && let Ok(iface) = conn.object_server().interface::<_, Self>(PATH).await
            {
                Self::emit_all(&iface).await;
            }
        });
    }

    async fn run_check(&self, conn: Connection, guard: CheckGuard) {
        let db = self.cache.join("checkup-db");
        let (repo, apps, snaps, firmware, feed) = tokio::join!(
            bounded("system", packages::check(&db)),
            bounded("apps", flatpak::check()),
            // The apt helper refreshes snaps with the system upgrade; the Arch helper does not.
            bounded("snaps", async { if cfg!(feature = "apt") { snap::check().await } else { Ok(Vec::new()) } }),
            bounded("firmware", async {
                let system = Connection::system().await?;
                fwupd::check(&system).await
            }),
            bounded("news", async { tokio::task::spawn_blocking(news::fetch).await? }),
        );
        let mut rows = Vec::new();
        let mut errors = Vec::new();
        match repo {
            Ok((list, held)) => {
                rows.extend(list.into_iter().map(|u| {
                    let group = if u.ours { "andromeda" } else { "system" };
                    (group.into(), u.name, u.old, u.new, u.size)
                }));
                rows.extend(held.into_iter().map(|u| ("held".into(), u.name, u.old, u.new, 0)));
            }
            Err(e) => errors.push(format!("system: {e:#}")),
        }
        match apps {
            Ok(list) => rows.extend(list.into_iter().map(|u| ("apps".into(), u.id, String::new(), u.version, u.size))),
            Err(e) => errors.push(format!("apps: {e:#}")),
        }
        match snaps {
            Ok(list) => {
                rows.extend(list.into_iter().map(|u| ("snaps".into(), u.name, String::new(), u.version, u.size)));
            }
            Err(e) => errors.push(format!("snaps: {e:#}")),
        }
        match firmware {
            Ok(list) => {
                rows.extend(list.into_iter().map(|u| ("firmware".into(), u.device, u.current, u.version, u.size)));
            }
            Err(e) => errors.push(format!("firmware: {e:#}")),
        }
        let token = self.read_token();
        let (scheduled, count) = {
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
            (guard.finish(&mut state), state.updates.iter().filter(|row| row.0 != "held").count())
        };
        if let Ok(iface) = conn.object_server().interface::<_, Self>(PATH).await {
            Self::emit_all(&iface).await;
        }
        if scheduled && count > 0 {
            self.notify(&conn, count).await;
        }
    }

    /// One notification a day at most; a click opens Settings › Software Update (x-andromeda-settings-page).
    async fn notify(&self, conn: &Connection, count: usize) {
        // At most one a day: 20 h apart, so the daily timer's random delay never skips a day.
        let file = self.state_dir.join("notified-at");
        let last = std::fs::read_to_string(&file).ok().and_then(|t| t.trim().parse::<i64>().ok()).unwrap_or(0);
        if now() - last < 20 * 3600 {
            return;
        }
        let body =
            if count == 1 { "1 update is available".to_owned() } else { format!("{count} updates are available") };
        let hints: HashMap<&str, zbus::Variant<'_>> =
            HashMap::from([("x-andromeda-settings-page", "software-update".into()), ("category", "system".into())]);
        let reply = conn
            .call_method(
                Some("org.freedesktop.Notifications"),
                "/org/freedesktop/Notifications",
                Some("org.freedesktop.Notifications"),
                "Notify",
                &(
                    "Software Update",
                    0u32,
                    "system-software-update",
                    "Software Update",
                    body.as_str(),
                    vec!["default", "Open"],
                    hints,
                    -1i32,
                ),
            )
            .await;
        match reply {
            Ok(_) => {
                let _ = std::fs::create_dir_all(&self.state_dir);
                let _ = std::fs::write(&file, now().to_string());
            }
            Err(e) => log::warn!("notify: {e}"),
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
        news::GATES && (!state.news_known || !state.unread.is_empty())
    }

    #[zbus(property)]
    fn last_check(&self) -> i64 {
        self.lock().last_check
    }

    #[zbus(property)]
    fn checking(&self) -> bool {
        self.lock().run != Run::Idle
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
        let Some(guard) = CheckGuard::begin(&self.state, false) else { return Ok(()) };
        let _ = self.checking_changed(&emitter).await;
        self.spawn_check(conn.clone(), guard);
        Ok(())
    }

    /// The daily timer's check (andromeda-update-check.timer, on AC only): skipped on a metered network, where a
    /// sync of the package databases and the Flatpak and firmware metadata costs the user data.
    async fn check_scheduled(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        if metered().await {
            log::info!("scheduled check skipped: metered network");
            return Ok(());
        }
        let Some(guard) = CheckGuard::begin(&self.state, true) else { return Ok(()) };
        let _ = self.checking_changed(&emitter).await;
        self.spawn_check(conn.clone(), guard);
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn check_after_panicked_check_is_accepted() {
        let state = Arc::new(Mutex::new(State::default()));
        let Some(guard) = CheckGuard::begin(&state, true) else {
            panic!("the first check must begin");
        };
        assert!(CheckGuard::begin(&state, false).is_none());
        let panicked = std::thread::spawn(move || {
            let _guard = guard;
            panic!("check failed");
        })
        .join();
        assert!(panicked.is_err());
        assert!(CheckGuard::begin(&state, false).is_some());
    }
}
