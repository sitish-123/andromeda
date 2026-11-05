//! PDF annotations made in the Viewer (P6): highlight, note, ink and signature stamp, kept in the open document
//! until `save` writes it to an fd the window opened. Bodies come in a sealed memfd; all coordinates are points
//! with the origin at the top left of the page (the window's system), flipped here to PDF space.

use crate::decode::Pdf;
use crate::proto::{ERR_BAD_REQUEST, ERR_DECODE, ERR_RANGE, ERR_TOO_LARGE, Msg, OP_ANNOT_REPLY, OP_SAVED};
use glib::translate::{FromGlibPtrFull, ToGlibPtr};
use poppler::ffi;
use rustix::fd::{IntoRawFd, OwnedFd};
use std::ffi::CString;

#[allow(unsafe_code)]
unsafe extern "C" {
    fn fflush(stream: *mut std::ffi::c_void) -> std::ffi::c_int;
}

pub const ANNOT_HIGHLIGHT: u32 = 1;
pub const ANNOT_NOTE: u32 = 2;
pub const ANNOT_INK: u32 = 3;
pub const ANNOT_STAMP: u32 = 4;

/// Body size cap: a signature image of 2048 x 1024 BGRA plus headers.
const MAX_BODY: u64 = 9 << 20;
const MAX_RECTS: usize = 4096;
const MAX_STROKES: usize = 256;
const MAX_POINTS: usize = 8192;
const MAX_STAMP_SIDE: u32 = 2048;

enum Parsed {
    Highlight(ffi::PopplerRectangle, Vec<ffi::PopplerQuadrilateral>),
    Note(ffi::PopplerRectangle, CString),
    Ink(ffi::PopplerRectangle, Vec<Vec<ffi::PopplerPoint>>),
    Stamp(ffi::PopplerRectangle, cairo::ImageSurface),
}

struct Reader<'a> {
    bytes: &'a [u8],
}

impl Reader<'_> {
    fn u32(&mut self) -> Result<u32, u32> {
        let (head, rest) = self.bytes.split_first_chunk::<4>().ok_or(ERR_BAD_REQUEST)?;
        self.bytes = rest;
        Ok(u32::from_le_bytes(*head))
    }

    fn f64(&mut self) -> Result<f64, u32> {
        let v = f64::from(f32::from_bits(self.u32()?));
        if v.is_finite() { Ok(v) } else { Err(ERR_BAD_REQUEST) }
    }

    fn take(&mut self, n: usize) -> Result<&[u8], u32> {
        if n > self.bytes.len() {
            return Err(ERR_BAD_REQUEST);
        }
        let (head, rest) = self.bytes.split_at(n);
        self.bytes = rest;
        Ok(head)
    }
}

fn read_body(fd: Option<OwnedFd>) -> Result<Vec<u8>, u32> {
    use std::os::unix::fs::FileExt;
    let fd = fd.ok_or(ERR_BAD_REQUEST)?;
    // fstat, not File::metadata: that uses statx, which the document sandbox does not allow.
    let len = u64::try_from(rustix::fs::fstat(&fd).map_err(|_| ERR_BAD_REQUEST)?.st_size).map_err(|_| ERR_BAD_REQUEST)?;
    let file = std::fs::File::from(fd);
    if len > MAX_BODY {
        return Err(ERR_TOO_LARGE);
    }
    // From offset 0 whatever the sender's file offset is: the offset is shared with the sender's copy of the fd.
    let mut out = vec![0; usize::try_from(len).map_err(|_| ERR_TOO_LARGE)?];
    file.read_exact_at(&mut out, 0).map_err(|_| ERR_BAD_REQUEST)?;
    Ok(out)
}

fn color(rgb: u32) -> ffi::PopplerColor {
    // 8-bit channels to poppler's 16-bit ones.
    let c = |shift: u32| u16::try_from((rgb >> shift) & 0xff).unwrap_or(0) * 257;
    ffi::PopplerColor { red: c(16), green: c(8), blue: c(0) }
}

impl Pdf {
    /// Adds one annotation to page `index`; the reply's b is its id for `remove`.
    ///
    /// # Errors
    /// `ERR_RANGE` for a page past the end, `ERR_BAD_REQUEST` for a malformed body, `ERR_DECODE` when poppler
    /// refuses the annotation.
    #[allow(unsafe_code)]
    pub fn annotate(&mut self, index: u32, kind: u32, rgb: u32, fd: Option<OwnedFd>) -> Result<Msg, u32> {
        let page = self.page(index)?;
        let (_, height) = page.size();
        let body = read_body(fd)?;
        let mut r = Reader { bytes: &body };
        // PDF space: y grows upward from the bottom of the page.
        let pdf_rect = |x0: f64, y0: f64, x1: f64, y1: f64| ffi::PopplerRectangle {
            x1: x0.min(x1),
            y1: height - y0.max(y1),
            x2: x0.max(x1),
            y2: height - y0.min(y1),
        };
        let grow = |b: &mut ffi::PopplerRectangle, x: f64, y: f64| {
            b.x1 = b.x1.min(x);
            b.y1 = b.y1.min(y);
            b.x2 = b.x2.max(x);
            b.y2 = b.y2.max(y);
        };
        let empty = || ffi::PopplerRectangle { x1: f64::MAX, y1: f64::MAX, x2: f64::MIN, y2: f64::MIN };

        // The whole body is parsed before the first poppler call, so a malformed one allocates nothing there.
        let parsed = match kind {
            ANNOT_HIGHLIGHT => {
                let n = r.u32()? as usize;
                if n == 0 || n > MAX_RECTS {
                    return Err(ERR_BAD_REQUEST);
                }
                let mut quads = Vec::with_capacity(n);
                let mut bounds = empty();
                for _ in 0..n {
                    let rect = pdf_rect(r.f64()?, r.f64()?, r.f64()?, r.f64()?);
                    // Quad order: top left, top right, bottom left, bottom right (PDF space).
                    quads.push(ffi::PopplerQuadrilateral {
                        p1: ffi::PopplerPoint { x: rect.x1, y: rect.y2 },
                        p2: ffi::PopplerPoint { x: rect.x2, y: rect.y2 },
                        p3: ffi::PopplerPoint { x: rect.x1, y: rect.y1 },
                        p4: ffi::PopplerPoint { x: rect.x2, y: rect.y1 },
                    });
                    grow(&mut bounds, rect.x1, rect.y1);
                    grow(&mut bounds, rect.x2, rect.y2);
                }
                Parsed::Highlight(bounds, quads)
            }
            ANNOT_NOTE => {
                let (x, y) = (r.f64()?, r.f64()?);
                let text = std::str::from_utf8(r.bytes).map_err(|_| ERR_BAD_REQUEST)?;
                Parsed::Note(pdf_rect(x, y, x + 20.0, y + 20.0), CString::new(text).map_err(|_| ERR_BAD_REQUEST)?)
            }
            ANNOT_INK => {
                let strokes = r.u32()? as usize;
                if strokes == 0 || strokes > MAX_STROKES {
                    return Err(ERR_BAD_REQUEST);
                }
                let mut paths = Vec::with_capacity(strokes);
                let mut bounds = empty();
                for _ in 0..strokes {
                    let n = r.u32()? as usize;
                    if n == 0 || n > MAX_POINTS {
                        return Err(ERR_BAD_REQUEST);
                    }
                    let mut points = Vec::with_capacity(n);
                    for _ in 0..n {
                        let p = ffi::PopplerPoint { x: r.f64()?, y: height - r.f64()? };
                        grow(&mut bounds, p.x, p.y);
                        points.push(p);
                    }
                    paths.push(points);
                }
                // The stroke has width: grow the box so its edges are not cut.
                bounds.x1 -= 2.0;
                bounds.y1 -= 2.0;
                bounds.x2 += 2.0;
                bounds.y2 += 2.0;
                Parsed::Ink(bounds, paths)
            }
            ANNOT_STAMP => {
                let rect = pdf_rect(r.f64()?, r.f64()?, r.f64()?, r.f64()?);
                let (w, h) = (r.u32()?, r.u32()?);
                if w == 0 || h == 0 || w > MAX_STAMP_SIDE || h > MAX_STAMP_SIDE {
                    return Err(ERR_BAD_REQUEST);
                }
                let pixels = r.take(w as usize * h as usize * 4)?.to_vec();
                let surface = cairo::ImageSurface::create_for_data(
                    pixels,
                    cairo::Format::ARgb32,
                    i32::try_from(w).map_err(|_| ERR_BAD_REQUEST)?,
                    i32::try_from(h).map_err(|_| ERR_BAD_REQUEST)?,
                    i32::try_from(w * 4).map_err(|_| ERR_BAD_REQUEST)?,
                )
                .map_err(|_| ERR_DECODE)?;
                Parsed::Stamp(rect, surface)
            }
            _ => return Err(ERR_BAD_REQUEST),
        };

        let doc: *mut ffi::PopplerDocument = self.doc().to_glib_none().0;
        // SAFETY: each poppler_annot_*_new returns a new full reference that from_glib_full takes over. Rectangles,
        // quads and the colour are copied by poppler. A PopplerPath owns its point array (poppler_path_free
        // g_frees it), so the points go into g_malloc memory; set_ink_list copies the paths.
        let annot: poppler::Annot = unsafe {
            match parsed {
                Parsed::Highlight(mut bounds, quads) => {
                    let len = u32::try_from(quads.len()).map_err(|_| ERR_BAD_REQUEST)?;
                    let size = u32::try_from(size_of::<ffi::PopplerQuadrilateral>()).map_err(|_| ERR_DECODE)?;
                    let array = glib::ffi::g_array_sized_new(0, 0, size, len);
                    glib::ffi::g_array_append_vals(array, quads.as_ptr().cast(), len);
                    let ptr = ffi::poppler_annot_text_markup_new_highlight(doc, &raw mut bounds, array);
                    glib::ffi::g_array_unref(array);
                    if ptr.is_null() {
                        return Err(ERR_DECODE);
                    }
                    poppler::Annot::from_glib_full(ptr)
                }
                Parsed::Note(mut rect, contents) => {
                    let ptr = ffi::poppler_annot_text_new(doc, &raw mut rect);
                    if ptr.is_null() {
                        return Err(ERR_DECODE);
                    }
                    ffi::poppler_annot_set_contents(ptr, contents.as_ptr());
                    poppler::Annot::from_glib_full(ptr)
                }
                Parsed::Ink(mut bounds, strokes) => {
                    let mut paths: Vec<*mut ffi::PopplerPath> = strokes
                        .iter()
                        .map(|points| {
                            let owned: *mut ffi::PopplerPoint =
                                glib::ffi::g_malloc_n(points.len(), size_of::<ffi::PopplerPoint>()).cast();
                            std::ptr::copy_nonoverlapping(points.as_ptr(), owned, points.len());
                            ffi::poppler_path_new_from_array(owned, points.len())
                        })
                        .collect();
                    let ptr = ffi::poppler_annot_ink_new(doc, &raw mut bounds);
                    if !ptr.is_null() {
                        ffi::poppler_annot_ink_set_ink_list(ptr.cast(), paths.as_mut_ptr(), paths.len());
                    }
                    for p in paths {
                        ffi::poppler_path_free(p);
                    }
                    if ptr.is_null() {
                        return Err(ERR_DECODE);
                    }
                    poppler::Annot::from_glib_full(ptr)
                }
                Parsed::Stamp(mut rect, surface) => {
                    let ptr = ffi::poppler_annot_stamp_new(doc, &raw mut rect);
                    if ptr.is_null() {
                        return Err(ERR_DECODE);
                    }
                    let annot = poppler::Annot::from_glib_full(ptr);
                    let mut error = std::ptr::null_mut();
                    if ffi::poppler_annot_stamp_set_custom_image(ptr.cast(), surface.to_raw_none(), &raw mut error) == 0 {
                        glib::ffi::g_clear_error(&raw mut error);
                        return Err(ERR_DECODE);
                    }
                    annot
                }
            }
        };
        if kind != ANNOT_STAMP {
            let mut c = color(rgb);
            // SAFETY: the annot is alive; poppler copies the colour.
            unsafe { ffi::poppler_annot_set_color(annot.to_glib_none().0, &raw mut c) };
        }
        page.add_annot(&annot);
        let id = self.keep_annot(index, annot);
        Ok(Msg::new(OP_ANNOT_REPLY, index, id, 0, 0))
    }

    /// Removes an annotation that `annotate` added.
    ///
    /// # Errors
    /// `ERR_RANGE` for an unknown id.
    pub fn remove_annot(&mut self, id: u32) -> Result<Msg, u32> {
        let (index, annot) = self.take_annot(id).ok_or(ERR_RANGE)?;
        self.page(index)?.remove_annot(&annot);
        Ok(Msg::new(OP_ANNOT_REPLY, index, id, 0, 0))
    }

    /// Writes the document with its changes to `fd` (a file the window opened for writing).
    ///
    /// # Errors
    /// `ERR_BAD_REQUEST` without an fd, `ERR_DECODE` when poppler cannot write.
    pub fn save(&self, fd: Option<OwnedFd>) -> Result<Msg, u32> {
        let fd = fd.ok_or(ERR_BAD_REQUEST)?;
        // poppler wraps the fd in a FILE* that it never flushes or closes (it leaks one per save): flush every stream,
        // or only the first buffer reaches the file.
        let saved = self.doc().save_to_fd(fd.into_raw_fd(), true);
        // SAFETY: fflush(NULL) flushes all open output streams and touches no Rust-owned memory.
        #[allow(unsafe_code)]
        unsafe {
            fflush(std::ptr::null_mut());
        }
        saved.map_err(|_| ERR_DECODE)?;
        Ok(Msg::new(OP_SAVED, 0, 0, 0, 0))
    }
}
