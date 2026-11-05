use andromeda_thumbnailer::cache::{self, Flavor};
use andromeda_thumbnailer::png;
use andromeda_thumbnailer::registry::Registry;
use andromeda_thumbnailer::runner::{Error, Runner, Sandbox};
use std::os::unix::fs::PermissionsExt;
use std::path::Path;
use std::time::{Duration, SystemTime};

struct Fixture {
    dir: tempfile::TempDir,
    runner: Runner,
}

/// Two fake thumbnailers: `text/x-fail` always fails, `text/x-ok` writes a marker PNG. Each run appends a line
/// to `runs`, so a test counts how often a thumbnailer started.
fn fixture() -> Fixture {
    let dir = tempfile::tempdir().unwrap();
    let thumbnailers = dir.path().join("thumbnailers");
    std::fs::create_dir(&thumbnailers).unwrap();
    let runs = dir.path().join("runs");
    let sample = dir.path().join("sample.png");
    std::fs::write(&sample, png::marker(&[("Software", "fake")])).unwrap();
    let scripts = dir.path().join("my scripts");
    std::fs::create_dir(&scripts).unwrap();
    for (name, body, mime) in [
        ("fail", format!("echo fail >> {}\nexit 1", runs.display()), "text/x-fail"),
        // Copies only when the input it was given exists, so a mangled %i fails.
        ("ok", format!("echo ok >> {}\ntest -f \"$1\" && cp {} \"$2\"", runs.display(), sample.display()), "text/x-ok"),
        ("link", format!("echo link >> {}\nln -s /dev/zero \"$2\"", runs.display()), "text/x-link"),
        ("fifo", format!("echo fifo >> {}\nmkfifo \"$2\"", runs.display()), "text/x-fifo"),
    ] {
        let script = scripts.join(format!("{name}.sh"));
        std::fs::write(&script, format!("#!/bin/sh\n{body}\n")).unwrap();
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o755)).unwrap();
        std::fs::write(
            thumbnailers.join(format!("{name}.thumbnailer")),
            format!("[Thumbnailer Entry]\nExec=\"{}\" %i %o %s\nMimeType={mime};\n", script.display()),
        )
        .unwrap();
    }
    let runner = Runner {
        root: dir.path().join("cache"),
        registry: Registry::load(&[thumbnailers], false),
        sandbox: Sandbox::None,
        timeout: Duration::from_secs(5),
    };
    Fixture { dir, runner }
}

fn runs(f: &Fixture) -> usize {
    std::fs::read_to_string(f.dir.path().join("runs")).map_or(0, |s| s.lines().count())
}

fn uri_of(path: &Path) -> String {
    use std::os::unix::ffi::OsStrExt;
    let mut uri = String::from("file://");
    for &b in path.as_os_str().as_bytes() {
        if b.is_ascii_alphanumeric() || b"/-_.~".contains(&b) {
            uri.push(char::from(b));
        } else {
            uri.push_str(&format!("%{b:02X}"));
        }
    }
    uri
}

fn file(f: &Fixture, name: &str) -> (std::path::PathBuf, String) {
    let path = f.dir.path().join(name);
    std::fs::write(&path, b"content").unwrap();
    let uri = uri_of(&path);
    (path, uri)
}

fn touch_later(path: &Path) {
    let later = SystemTime::now() + Duration::from_secs(10);
    std::fs::File::options().write(true).open(path).unwrap().set_modified(later).unwrap();
}

#[test]
fn failed_file_is_not_retried_until_its_mtime_changes() {
    let f = fixture();
    let (path, uri) = file(&f, "broken file.fail");
    assert_eq!(f.runner.thumbnail(&uri, "text/x-fail", Flavor::Normal), Err(Error::Failed));
    assert_eq!(f.runner.thumbnail(&uri, "text/x-fail", Flavor::Large), Err(Error::Failed));
    assert_eq!(runs(&f), 1);
    let marker = std::fs::read(cache::fail_path(&f.runner.root, &uri)).unwrap();
    assert_eq!(png::text_value(&marker, "Thumb::URI").as_deref(), Some(uri.as_str()));

    touch_later(&path);
    assert_eq!(f.runner.thumbnail(&uri, "text/x-fail", Flavor::Normal), Err(Error::Failed));
    assert_eq!(runs(&f), 2);
}

#[test]
fn made_thumbnail_is_tagged_private_and_reused_until_the_file_changes() {
    let f = fixture();
    let (path, uri) = file(&f, "picture.ok");
    let made = f.runner.thumbnail(&uri, "text/x-ok", Flavor::Normal).unwrap();
    assert_eq!(made, cache::thumbnail_path(&f.runner.root, Flavor::Normal, &uri));
    let bytes = std::fs::read(&made).unwrap();
    let mtime = std::fs::metadata(&path).unwrap().modified().unwrap();
    let secs = mtime.duration_since(SystemTime::UNIX_EPOCH).unwrap().as_secs().to_string();
    assert_eq!(png::text_value(&bytes, "Thumb::URI").as_deref(), Some(uri.as_str()));
    assert_eq!(png::text_value(&bytes, "Thumb::MTime"), Some(secs));
    assert_eq!(png::text_value(&bytes, "Software").as_deref(), Some("fake"));
    assert_eq!(std::fs::metadata(&made).unwrap().permissions().mode() & 0o777, 0o600);

    f.runner.thumbnail(&uri, "text/x-ok", Flavor::Normal).unwrap();
    assert_eq!(runs(&f), 1);
    touch_later(&path);
    f.runner.thumbnail(&uri, "text/x-ok", Flavor::Normal).unwrap();
    assert_eq!(runs(&f), 2);
}

#[test]
fn remote_and_unknown_files_start_no_thumbnailer() {
    let f = fixture();
    assert_eq!(f.runner.thumbnail("sftp://host/a.png", "text/x-ok", Flavor::Normal), Err(Error::NotLocal));
    let (_, uri) = file(&f, "notes.txt");
    assert_eq!(f.runner.thumbnail(&uri, "text/plain", Flavor::Normal), Err(Error::Unsupported));
    assert_eq!(runs(&f), 0);
}

#[test]
fn text_tags_replace_old_values_and_survive_a_reread() {
    let base = png::marker(&[("Thumb::URI", "file:///old")]);
    let out = png::with_text(&base, &[("Thumb::URI", "file:///new"), ("Thumb::MTime", "7")]).unwrap();
    let tags = png::read_text(&out);
    assert_eq!(tags.iter().filter(|(k, _)| k == "Thumb::URI").count(), 1);
    assert_eq!(png::text_value(&out, "Thumb::URI").as_deref(), Some("file:///new"));
    assert!(png::with_text(b"not a png", &[("a", "b")]).is_none());
}

#[test]
fn quoted_exec_percent_and_non_utf8_names_reach_the_thumbnailer_intact() {
    use std::os::unix::ffi::OsStrExt;
    let f = fixture();
    let (_, uri) = file(&f, "a%s%%b%o.ok");
    f.runner.thumbnail(&uri, "text/x-ok", Flavor::Normal).unwrap();
    let odd = f.dir.path().join(std::ffi::OsStr::from_bytes(b"caf\xe9.ok"));
    std::fs::write(&odd, b"x").unwrap();
    f.runner.thumbnail(&uri_of(&odd), "text/x-ok", Flavor::Normal).unwrap();
    assert_eq!(runs(&f), 2);
}

#[test]
fn planted_link_or_fifo_output_is_a_failure_not_a_read() {
    let f = fixture();
    let (_, link) = file(&f, "a.link");
    let (_, fifo) = file(&f, "a.fifo");
    let start = std::time::Instant::now();
    assert_eq!(f.runner.thumbnail(&link, "text/x-link", Flavor::Normal), Err(Error::Failed));
    assert_eq!(f.runner.thumbnail(&fifo, "text/x-fifo", Flavor::Normal), Err(Error::Failed));
    assert!(start.elapsed() < Duration::from_secs(4));
}

#[test]
fn a_thumbnailer_that_cannot_start_leaves_no_fail_marker() {
    let f = fixture();
    let (_, uri) = file(&f, "picture.ok");
    std::fs::remove_file(f.dir.path().join("my scripts/ok.sh")).unwrap();
    assert!(matches!(f.runner.thumbnail(&uri, "text/x-ok", Flavor::Normal), Err(Error::Io(_))));
    assert!(!cache::fail_path(&f.runner.root, &uri).exists());
}

/// The real sandbox with the installed glycin thumbnailer; skipped where systemd-run, bwrap or glycin is missing.
#[test]
fn installed_thumbnailer_runs_inside_bwrap() {
    let dir = tempfile::tempdir().unwrap();
    let runner = Runner {
        root: dir.path().join("cache"),
        registry: Registry::load(&[std::path::PathBuf::from("/usr/share/thumbnailers")], true),
        sandbox: Sandbox::Bwrap,
        timeout: Duration::from_secs(30),
    };
    if runner.registry.for_mime("image/png").is_none() || !runner.sandbox_works() {
        eprintln!("skipped: no image/png thumbnailer or no working sandbox");
        return;
    }
    let image = dir.path().join("dot.png");
    std::fs::write(&image, png::marker(&[])).unwrap();
    let made = runner.thumbnail(&uri_of(&image), "image/png", Flavor::Normal).unwrap();
    assert!(png::size(&std::fs::read(made).unwrap()).is_some());
}
