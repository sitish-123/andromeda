//! The system upgrade on libalpm, the equivalent of `pacman -Syu` without `--noconfirm`: any question libalpm asks
//! (a replacement, a conflict, a provider, a key) gets the safe answer, is recorded, and stops the transaction, so the
//! page can show it instead of a default answer being taken (docs/system.md §1). Blocking: runs on its own thread.

use std::cell::RefCell;
use std::rc::Rc;

use alpm::{Alpm, PrepareData, Progress, Question, TransFlag};
use anyhow::{Context, Result, anyhow, bail};

pub enum Event {
    /// (stage, percent 0-100, package)
    Progress(String, i32, String),
}

/// What an upgrade covers.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Scope {
    /// The full system upgrade.
    All,
    /// Only the packages of the Andromeda repository; refused when they need a newer system package.
    Andromeda,
}

pub struct Outcome {
    /// Packages the transaction installs or upgrades ("name version"), then those it removes ("-name").
    pub targets: Vec<String>,
    /// Questions libalpm asked; non-empty means nothing was committed.
    pub questions: Vec<String>,
    pub committed: bool,
}

fn stage(progress: Progress) -> &'static str {
    match progress {
        Progress::AddStart | Progress::UpgradeStart | Progress::DowngradeStart | Progress::ReinstallStart => "install",
        Progress::RemoveStart => "remove",
        Progress::ConflictsStart => "conflicts",
        Progress::DiskspaceStart => "disk space",
        Progress::IntegrityStart => "integrity",
        Progress::LoadStart => "load",
        Progress::KeyringStart => "keyring",
    }
}

fn record_questions(alpm: &Alpm, asked: &Rc<RefCell<Vec<String>>>) {
    let asked = asked.clone();
    alpm.set_question_cb((), move |question, ()| {
        let text = match question.question() {
            Question::InstallIgnorepkg(mut q) => {
                q.set_install(false);
                format!("install {} although IgnorePkg lists it", q.pkg().name())
            }
            Question::Replace(q) => {
                q.set_replace(false);
                format!("replace {} with {}/{}", q.oldpkg().name(), q.newdb().name(), q.newpkg().name())
            }
            Question::Conflict(mut q) => {
                q.set_remove(false);
                let conflict = q.conflict();
                format!("{} conflicts with {}", conflict.package1().name(), conflict.package2().name())
            }
            Question::Corrupted(mut q) => {
                q.set_remove(false);
                format!("{} is corrupted", q.filepath())
            }
            Question::RemovePkgs(mut q) => {
                q.set_skip(true);
                let names: Vec<&str> = q.packages().iter().map(|p| p.name()).collect();
                format!("remove {} {UNMET}", names.join(", "))
            }
            Question::SelectProvider(mut q) => {
                q.set_index(0);
                let names: Vec<&str> = q.providers().iter().map(|p| p.name()).collect();
                format!("choose a provider for {}: {}", q.depend(), names.join(", "))
            }
            Question::ImportKey(mut q) => {
                q.set_import(false);
                format!("import PGP key {} ({})", q.fingerprint(), q.uid())
            }
        };
        log::warn!("question: {text}");
        asked.borrow_mut().push(text);
    });
}

/// Adds each installed package that has a newer version in the Andromeda repository.
fn add_andromeda(alpm: &Alpm) -> Result<()> {
    let db = alpm.syncdbs().iter().find(|db| db.name() == andromeda_update::repo::PACMAN_REPO);
    let db = db.context("no [andromeda] repository in pacman.conf")?;
    for pkg in db.pkgs() {
        let Ok(local) = alpm.localdb().pkg(pkg.name()) else { continue };
        // IgnorePkg / IgnoreGroup: held, as the full upgrade keeps them.
        if pkg.should_ignore() {
            continue;
        }
        if pkg.version().vercmp(local.version()) == std::cmp::Ordering::Greater {
            alpm.trans_add_pkg(pkg).map_err(|e| anyhow!("add {}: {e}", pkg.name()))?;
        }
    }
    Ok(())
}

/// An Andromeda update that also upgrades an installed system package is a partial upgrade: refused.
fn refuse_system_upgrades(alpm: &Alpm) -> Result<()> {
    let system: Vec<&str> = alpm
        .trans_add()
        .iter()
        .filter(|p| p.db().is_some_and(|db| db.name() != andromeda_update::repo::PACMAN_REPO))
        .filter(|p| alpm.localdb().pkg(p.name()).is_ok())
        .map(|p| p.name())
        .collect();
    if system.is_empty() {
        return Ok(());
    }
    Err(anyhow!("{NEEDS_SYSTEM}: {}", system.join(", ")))
}

/// The end of the question text for a package libalpm would drop for unmet dependencies.
const UNMET: &str = "(their dependencies cannot be met)";

/// The message start the page shows as "update the system first".
pub const NEEDS_SYSTEM: &str = "needs a system update first";

/// `dry_run`: prepares against the databases as they are (no sync: a sync without the upgrade would leave a partial
/// upgrade state) and lists the targets. Otherwise syncs, prepares and commits.
/// `before_commit` runs once the commit is certain (the snapshot); its error stops the upgrade.
pub fn upgrade(
    scope: Scope,
    dry_run: bool,
    events: &tokio::sync::mpsc::UnboundedSender<Event>,
    before_commit: &mut dyn FnMut() -> Result<()>,
) -> Result<Outcome> {
    let conf = pacmanconf::Config::new().context("pacman-conf")?;
    let mut alpm = alpm_utils::alpm_with_conf(&conf).context("libalpm")?;
    // alpm_with_conf sets IgnorePkg but not IgnoreGroup.
    alpm.set_ignoregroups(conf.ignore_group.iter()).context("IgnoreGroup")?;
    let asked = Rc::new(RefCell::new(Vec::new()));
    record_questions(&alpm, &asked);
    if !dry_run {
        match scope {
            Scope::All => {
                alpm.syncdbs_mut().update(false).context("sync databases")?;
            }
            // Only our database: the others stay at the state the installed system came from, so dependencies
            // resolve against it, never against newer Arch packages.
            Scope::Andromeda => {
                let mut only = conf.clone();
                only.repos.retain(|r| r.name == andromeda_update::repo::PACMAN_REPO);
                if only.repos.is_empty() {
                    bail!("no [andromeda] repository in pacman.conf");
                }
                let mut ours = alpm_utils::alpm_with_conf(&only).context("libalpm")?;
                ours.syncdbs_mut().update(false).context("sync database")?;
            }
        }
    }
    // A preview takes no database lock, so it never blocks a pacman run.
    alpm.trans_init(if dry_run { TransFlag::NO_LOCK } else { TransFlag::NONE }).context("start transaction")?;
    let result = (|| -> Result<Outcome> {
        match scope {
            Scope::All => alpm.sync_sysupgrade(false).context("system upgrade")?,
            Scope::Andromeda => add_andromeda(&alpm)?,
        }
        alpm.trans_prepare().map_err(|e| match (scope, e.data()) {
            // Our packages need a newer system package than the installed system's databases offer.
            (Scope::Andromeda, Some(PrepareData::UnsatisfiedDeps(missing))) => {
                let deps: Vec<String> = missing.iter().map(|m| m.depend().to_string()).collect();
                anyhow!("{NEEDS_SYSTEM}: {}", deps.join(", "))
            }
            _ => anyhow!("prepare: {e}"),
        })?;
        if scope == Scope::Andromeda {
            refuse_system_upgrades(&alpm)?;
        }
        let mut targets: Vec<String> =
            alpm.trans_add().iter().map(|p| format!("{} {}", p.name(), p.version())).collect();
        targets.extend(alpm.trans_remove().iter().map(|p| format!("-{}", p.name())));
        let questions = asked.borrow().clone();
        if dry_run || !questions.is_empty() || targets.is_empty() {
            return Ok(Outcome { targets, questions, committed: false });
        }
        before_commit()?;
        alpm.set_progress_cb(events.clone(), |p: Progress, pkg: &str, percent: i32, _: usize, _: usize, tx| {
            let _ = tx.send(Event::Progress(stage(p).to_owned(), percent, pkg.to_owned()));
        });
        alpm.trans_commit().map_err(|e| anyhow!("commit: {e}"))?;
        let questions = asked.borrow().clone();
        Ok(Outcome { targets, questions, committed: true })
    })();
    let _ = alpm.trans_release();
    // A safe answer often makes libalpm fail (a conflict not removed, a key not imported): the question is the cause.
    let questions = asked.borrow().clone();
    // libalpm asks to drop a package whose dependencies the installed system's databases cannot meet.
    if scope == Scope::Andromeda && questions.iter().any(|q| q.ends_with(UNMET)) {
        return Err(anyhow!("{NEEDS_SYSTEM}: {}", questions.join("; ")));
    }
    match result {
        Err(_) if !questions.is_empty() => Ok(Outcome { targets: Vec::new(), questions, committed: false }),
        Err(e) if !dry_run && scope == Scope::All => Err(e.context("the package databases were refreshed, nothing was upgraded")),
        other => other,
    }
}
