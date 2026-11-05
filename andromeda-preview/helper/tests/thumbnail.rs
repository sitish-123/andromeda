//! `andromeda-preview-helper thumbnail <size> <in> <out>`: the PDF thumbnailer entry point.

use std::process::Command;

/// A one-page A4 PDF.
fn pdf(dir: &std::path::Path) -> std::path::PathBuf {
    let path = dir.join("doc.pdf");
    let surface = cairo::PdfSurface::new(595.0, 842.0, &path).unwrap();
    let cr = cairo::Context::new(&surface).unwrap();
    cr.set_font_size(48.0);
    cr.move_to(72.0, 144.0);
    cr.show_text("Report").unwrap();
    cr.show_page().unwrap();
    drop(cr);
    surface.finish();
    path
}

fn run(size: &str, input: &std::path::Path, output: &std::path::Path) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_andromeda-preview-helper"))
        .args(["thumbnail", size])
        .arg(input)
        .arg(output)
        .output()
        .unwrap()
}

/// Width and height from a PNG's IHDR.
fn png_size(bytes: &[u8]) -> (u32, u32) {
    assert_eq!(&bytes[..8], b"\x89PNG\r\n\x1a\n", "not a PNG");
    let at = |i: usize| u32::from_be_bytes([bytes[i], bytes[i + 1], bytes[i + 2], bytes[i + 3]]);
    (at(16), at(20))
}

#[test]
fn first_page_fits_the_requested_box() {
    let dir = tempfile::tempdir().unwrap();
    let out = dir.path().join("out.png");
    let result = run("256", &pdf(dir.path()), &out);
    assert!(result.status.success(), "{}", String::from_utf8_lossy(&result.stderr));
    // A4 portrait: the height takes the box.
    assert_eq!(png_size(&std::fs::read(&out).unwrap()), (181, 256));
}

#[test]
fn an_existing_output_is_never_written_through() {
    let dir = tempfile::tempdir().unwrap();
    let target = dir.path().join("victim");
    std::fs::write(&target, b"keep").unwrap();
    let out = dir.path().join("out.png");
    std::os::unix::fs::symlink(&target, &out).unwrap();
    assert!(!run("256", &pdf(dir.path()), &out).status.success());
    assert_eq!(std::fs::read(&target).unwrap(), b"keep");
}

#[test]
fn a_file_that_is_not_a_pdf_gives_no_thumbnail() {
    let dir = tempfile::tempdir().unwrap();
    let input = dir.path().join("fake.pdf");
    std::fs::write(&input, b"%PDF-1.7 but nothing else").unwrap();
    let out = dir.path().join("out.png");
    assert!(!run("256", &input, &out).status.success());
}
