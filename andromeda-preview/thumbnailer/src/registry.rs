//! `.thumbnailer` files (`/usr/share/thumbnailers`, and `thumbnailers/` under each XDG data dir).

use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub struct Thumbnailer {
    pub name: String,
    /// Exec split and unquoted; the program resolved to an absolute path.
    pub exec: Vec<String>,
}

/// Desktop Entry Exec splitting: whitespace separates arguments; double quotes group, with `\"`, `\\`, `\$`
/// and `` \` `` escapes inside them.
fn split_exec(value: &str) -> Option<Vec<String>> {
    let mut out = Vec::new();
    let mut current = String::new();
    let (mut quoted, mut has) = (false, false);
    let mut chars = value.chars();
    while let Some(c) = chars.next() {
        match c {
            '"' => {
                quoted = !quoted;
                has = true;
            }
            '\\' if quoted => current.push(chars.next()?),
            c if c.is_whitespace() && !quoted => {
                if has {
                    out.push(std::mem::take(&mut current));
                    has = false;
                }
            }
            c => {
                current.push(c);
                has = true;
            }
        }
    }
    if quoted {
        return None;
    }
    if has {
        out.push(current);
    }
    Some(out)
}

#[derive(Default, Debug)]
pub struct Registry {
    by_mime: HashMap<String, Thumbnailer>,
}

fn parse(name: &str, text: &str) -> Option<(Thumbnailer, Vec<String>, Option<String>)> {
    let mut in_entry = false;
    let (mut exec, mut mimes, mut try_exec) = (None, Vec::new(), None);
    for line in text.lines().map(str::trim) {
        if line.starts_with('[') {
            in_entry = line == "[Thumbnailer Entry]";
            continue;
        }
        let Some((key, value)) = line.split_once('=').filter(|_| in_entry) else { continue };
        match key.trim() {
            "Exec" => exec = split_exec(value),
            "MimeType" => mimes = value.split(';').filter(|m| !m.is_empty()).map(str::to_owned).collect(),
            "TryExec" => try_exec = Some(value.trim().to_owned()),
            _ => {}
        }
    }
    let exec = exec.filter(|e| !e.is_empty())?;
    Some((Thumbnailer { name: name.to_owned(), exec }, mimes, try_exec))
}

fn resolve(program: &str) -> Option<PathBuf> {
    use std::os::unix::fs::PermissionsExt;
    let found = |p: &Path| p.metadata().is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0);
    if program.contains('/') {
        return found(Path::new(program)).then(|| PathBuf::from(program));
    }
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path).map(|dir| dir.join(program)).find(|p| found(p))
}

impl Registry {
    /// Loads every directory in order; a later file does not replace an earlier one for the same type, so put
    /// the user's directory first.
    #[must_use]
    /// With `usr_only`, a program that does not resolve under /usr is skipped (the sandbox mounts only /usr), so
    /// the next entry for the same type takes over.
    pub fn load(dirs: &[PathBuf], usr_only: bool) -> Self {
        let mut registry = Self::default();
        for dir in dirs {
            let Ok(entries) = std::fs::read_dir(dir) else { continue };
            let mut files: Vec<_> = entries.flatten().map(|e| e.path()).collect();
            files.sort();
            for path in files.iter().filter(|p| p.extension().is_some_and(|e| e == "thumbnailer")) {
                let Ok(text) = std::fs::read_to_string(path) else { continue };
                let name = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
                let Some((mut thumbnailer, mimes, try_exec)) = parse(&name, &text) else { continue };
                if try_exec.as_deref().is_some_and(|t| resolve(t).is_none()) {
                    continue;
                }
                let Some(program) = resolve(&thumbnailer.exec[0]).and_then(|p| p.canonicalize().ok()) else { continue };
                if usr_only && !program.starts_with("/usr") {
                    continue;
                }
                thumbnailer.exec[0] = program.to_string_lossy().into_owned();
                for mime in mimes {
                    registry.by_mime.entry(mime).or_insert_with(|| thumbnailer.clone());
                }
            }
        }
        registry
    }

    /// `$XDG_DATA_HOME/thumbnailers`, then each of `$XDG_DATA_DIRS`.
    #[must_use]
    pub fn system_dirs() -> Vec<PathBuf> {
        let home = std::env::var_os("XDG_DATA_HOME")
            .filter(|v| !v.is_empty())
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")));
        let data = std::env::var("XDG_DATA_DIRS").ok().filter(|v| !v.is_empty());
        let data = data.as_deref().unwrap_or("/usr/local/share:/usr/share");
        home.into_iter().chain(std::env::split_paths(data)).map(|d| d.join("thumbnailers")).collect()
    }

    #[must_use]
    pub fn for_mime(&self, mime: &str) -> Option<&Thumbnailer> {
        self.by_mime.get(mime)
    }

    pub fn mime_types(&self) -> impl Iterator<Item = &str> {
        self.by_mime.keys().map(String::as_str)
    }
}
