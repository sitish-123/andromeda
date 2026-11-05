//! Decoders the helper runs inside its sandbox. Each returns a sealed memfd the window maps read-only.

use crate::proto::{
    ERR_BAD_REQUEST, ERR_BINARY, ERR_DECODE, ERR_RANGE, ERR_TOO_LARGE, KIND_PDF, Msg, OP_FIND_REPLY, OP_INFO_REPLY,
    OP_PAGE, OP_SELECT_REPLY, OP_TEXT_REPLY, SELECT_GLYPH, SELECT_LINE, SELECT_WORD,
};
use rustix::fd::OwnedFd;
use rustix::fs::{MemfdFlags, SealFlags};
use std::fs::File;
use std::io::{Read, Seek, Write};

/// Text preview reads at most this much (docs/search.md §5).
pub const TEXT_LIMIT: usize = 1 << 20;
/// A NUL in this prefix marks the file as binary.
const BINARY_PROBE: usize = 8192;
pub const MAX_WIDTH: u32 = 8192;
pub const MAX_HEIGHT: u32 = 16384;
/// Hits per page and selection rects per reply; a page with more is cut there.
pub const MAX_RECTS: usize = 4096;

pub(crate) fn sealed(name: &str, bytes: &[u8]) -> Result<OwnedFd, u32> {
    let fd = rustix::fs::memfd_create(name, MemfdFlags::CLOEXEC | MemfdFlags::ALLOW_SEALING).map_err(|_| ERR_DECODE)?;
    let mut file = File::from(fd);
    file.write_all(bytes).map_err(|_| ERR_DECODE)?;
    let fd = OwnedFd::from(file);
    rustix::fs::fcntl_add_seals(&fd, SealFlags::SHRINK | SealFlags::GROW | SealFlags::WRITE | SealFlags::SEAL)
        .map_err(|_| ERR_DECODE)?;
    Ok(fd)
}

pub struct Text {
    pub text: String,
    pub truncated: bool,
}

/// Reads the first `TEXT_LIMIT` bytes as UTF-8 (invalid sequences become U+FFFD).
///
/// # Errors
/// `ERR_BINARY` for a file with a NUL byte near its start, `ERR_DECODE` for a read error.
pub fn read_text(file: &mut File) -> Result<Text, u32> {
    let mut bytes = Vec::with_capacity(64 * 1024);
    file.rewind().map_err(|_| ERR_DECODE)?;
    file.take(TEXT_LIMIT as u64 + 1).read_to_end(&mut bytes).map_err(|_| ERR_DECODE)?;
    let truncated = bytes.len() > TEXT_LIMIT;
    bytes.truncate(TEXT_LIMIT);
    if bytes[..bytes.len().min(BINARY_PROBE)].contains(&0) {
        return Err(ERR_BINARY);
    }
    let mut text = String::from_utf8_lossy(&bytes).into_owned();
    // Lossy decoding grows each invalid byte to 3, and the limit may split the last character.
    let fits = text.floor_char_boundary(TEXT_LIMIT);
    let cut = truncated || fits < text.len();
    text.truncate(fits);
    if truncated && text.ends_with(char::REPLACEMENT_CHARACTER) {
        text.pop();
    }
    Ok(Text { text, truncated: cut })
}

/// # Errors
/// See [`read_text`].
pub fn text_reply(file: &mut File) -> Result<(Msg, OwnedFd), u32> {
    let text = read_text(file)?;
    let len = u32::try_from(text.text.len()).map_err(|_| ERR_TOO_LARGE)?;
    let fd = sealed("preview-text", text.text.as_bytes())?;
    Ok((Msg::new(OP_TEXT_REPLY, len, u32::from(text.truncated), 0, 0), fd))
}

pub struct Pdf {
    doc: poppler::Document,
}

fn centi_points(v: f64) -> u32 {
    // Page sizes are positive and far below u32::MAX / 100 points.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let out = (v * 100.0).round().clamp(0.0, f64::from(u32::MAX)) as u32;
    out
}

#[allow(clippy::cast_possible_truncation)]
fn push_rect(out: &mut Vec<u8>, x0: f64, y0: f64, x1: f64, y1: f64) {
    for v in [x0, y0, x1, y1] {
        out.extend_from_slice(&(v as f32).to_le_bytes());
    }
}

impl Pdf {
    fn page(&self, index: u32) -> Result<poppler::Page, u32> {
        i32::try_from(index)
            .ok()
            .filter(|&i| i < self.doc.n_pages())
            .and_then(|i| self.doc.page(i))
            .ok_or(ERR_RANGE)
    }

    /// The places of `query` on page `index`, top left origin; no fd when there is none.
    ///
    /// # Errors
    /// `ERR_RANGE` for a page past the end, `ERR_BAD_REQUEST` for an empty or non-UTF-8 query.
    pub fn find(&self, index: u32, query: &[u8]) -> Result<(Msg, Option<OwnedFd>), u32> {
        let query = std::str::from_utf8(query).ok().filter(|q| !q.trim().is_empty()).ok_or(ERR_BAD_REQUEST)?;
        let page = self.page(index)?;
        let (_, height) = page.size();
        let hits = page.find_text_with_options(
            query,
            poppler::FindFlags::DEFAULT | poppler::FindFlags::IGNORE_DIACRITICS | poppler::FindFlags::MULTILINE,
        );
        let mut rects = Vec::new();
        // poppler reports hits with the origin at the bottom left of the page.
        for hit in hits.iter().take(MAX_RECTS) {
            push_rect(&mut rects, hit.x1(), height - hit.y2(), hit.x2(), height - hit.y1());
        }
        let count = u32::try_from(rects.len() / 16).unwrap_or(0);
        let fd = if count > 0 { Some(sealed("preview-find", &rects)?) } else { None };
        Ok((Msg::new(OP_FIND_REPLY, index, count, 0, 0), fd))
    }

    /// The text inside `area` (4 f32 in points, top left origin) on page `index`, and the rects that cover it.
    ///
    /// # Errors
    /// `ERR_RANGE` for a page past the end, `ERR_BAD_REQUEST` for a bad area or unit.
    pub fn select(&self, index: u32, unit: u32, area: &[u8]) -> Result<(Msg, Option<OwnedFd>), u32> {
        let style = match unit {
            SELECT_GLYPH => poppler::SelectionStyle::Glyph,
            SELECT_WORD => poppler::SelectionStyle::Word,
            SELECT_LINE => poppler::SelectionStyle::Line,
            _ => return Err(ERR_BAD_REQUEST),
        };
        let v: Vec<f64> = area
            .as_chunks::<4>()
            .0
            .iter()
            .map(|c| f64::from(f32::from_le_bytes(*c)))
            .collect();
        if area.len() != 16 || v.iter().any(|x| !x.is_finite()) {
            return Err(ERR_BAD_REQUEST);
        }
        let page = self.page(index)?;
        let mut rect = poppler::Rectangle::new();
        rect.set_x1(v[0]);
        rect.set_y1(v[1]);
        rect.set_x2(v[2]);
        rect.set_y2(v[3]);
        let text = page.selected_text(style, &mut rect).map(|t| t.to_string()).unwrap_or_default();
        // The region comes in device pixels at this scale; 10 keeps a tenth of a point.
        const SCALE: f64 = 10.0;
        let region = page.selected_region(SCALE, style, &mut rect);
        let mut out = Vec::new();
        let count = region.as_ref().map_or(0, |r| usize::try_from(r.num_rectangles()).unwrap_or(0)).min(MAX_RECTS);
        for i in 0..count {
            let Some(r) = region.as_ref().map(|r| r.rectangle(i32::try_from(i).unwrap_or(0))) else { break };
            let (x, y) = (f64::from(r.x()) / SCALE, f64::from(r.y()) / SCALE);
            push_rect(&mut out, x, y, x + f64::from(r.width()) / SCALE, y + f64::from(r.height()) / SCALE);
        }
        out.extend_from_slice(text.as_bytes());
        let len = u32::try_from(text.len()).map_err(|_| ERR_TOO_LARGE)?;
        // An empty selection has no fd: the window cannot map an empty file.
        let fd = if out.is_empty() { None } else { Some(sealed("preview-select", &out)?) };
        Ok((Msg::new(OP_SELECT_REPLY, len, u32::try_from(count).unwrap_or(0), 0, 0), fd))
    }

    /// # Errors
    /// `ERR_DECODE` when poppler cannot parse the file (encrypted files included).
    pub fn open(file: File) -> Result<Self, u32> {
        use std::os::fd::IntoRawFd;
        let doc = poppler::Document::from_fd(file.into_raw_fd(), None).map_err(|_| ERR_DECODE)?;
        Ok(Self { doc })
    }

    #[must_use]
    pub fn info(&self) -> Msg {
        let pages = u32::try_from(self.doc.n_pages()).unwrap_or(0);
        let (w, h) = self.doc.page(0).map_or((0.0, 0.0), |p| p.size());
        Msg::new(OP_INFO_REPLY, KIND_PDF, pages, centi_points(w), centi_points(h))
    }

    /// Renders one page `width` pixels wide on white, at most `max_height` pixels high.
    ///
    /// # Errors
    /// `ERR_RANGE` for a page index past the end, `ERR_TOO_LARGE` for a size over the caps.
    pub fn render(&self, index: u32, width: u32, max_height: u32) -> Result<(Msg, OwnedFd), u32> {
        let page = self.page(index)?;
        let (pw, ph) = page.size();
        if width == 0 || width > MAX_WIDTH || pw <= 0.0 || ph <= 0.0 {
            return Err(ERR_TOO_LARGE);
        }
        let scale = f64::from(width) / pw;
        #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
        let height = (ph * scale).ceil().max(1.0) as u32;
        if height > MAX_HEIGHT.min(max_height.max(1)) {
            return Err(ERR_TOO_LARGE);
        }
        let w = i32::try_from(width).map_err(|_| ERR_TOO_LARGE)?;
        let h = i32::try_from(height).map_err(|_| ERR_TOO_LARGE)?;
        let mut surface = cairo::ImageSurface::create(cairo::Format::ARgb32, w, h).map_err(|_| ERR_DECODE)?;
        {
            let cr = cairo::Context::new(&surface).map_err(|_| ERR_DECODE)?;
            cr.set_source_rgb(1.0, 1.0, 1.0);
            cr.paint().map_err(|_| ERR_DECODE)?;
            cr.scale(scale, scale);
            page.render(&cr);
        }
        surface.flush();
        let stride = u32::try_from(surface.stride()).map_err(|_| ERR_DECODE)?;
        let data = surface.data().map_err(|_| ERR_DECODE)?;
        let fd = sealed("preview-page", &data)?;
        Ok((Msg::new(OP_PAGE, index, width, height, stride), fd))
    }
}
