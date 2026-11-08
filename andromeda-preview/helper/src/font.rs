//! Font specimens (Quick Look, mockup §15) and font thumbnails: FreeType reads the untrusted font file here, inside
//! the sandbox, from bytes read before it; cairo draws the glyphs.

use crate::decode::{MAX_HEIGHT, MAX_THUMBNAIL, MAX_WIDTH, sealed};
use crate::proto::{ERR_DECODE, ERR_TOO_LARGE, Msg, OP_PAGE};
use rustix::fd::OwnedFd;
use std::fs::File;
use std::io::Read;
use std::rc::Rc;

/// Larger font files are refused (big CJK fonts are about 20 MB).
const MAX_FONT: u64 = 64 << 20;
const SAMPLE: &str = "The quick brown fox jumps over the lazy dog";
const DIGITS: &str = "0123456789";

pub struct Font {
    face: cairo::FontFace,
    // FreeType's face borrows the library: both live as long as the cairo face.
    _ft: freetype::face::Face,
    _library: freetype::Library,
}

impl Font {
    /// Reads the whole file (before the sandbox) and opens its first face.
    ///
    /// # Errors
    /// `ERR_TOO_LARGE` over `MAX_FONT`, `ERR_DECODE` when FreeType cannot read it.
    pub fn read(file: &mut File) -> Result<Vec<u8>, u32> {
        let mut bytes = Vec::new();
        file.take(MAX_FONT + 1).read_to_end(&mut bytes).map_err(|_| ERR_DECODE)?;
        if bytes.len() as u64 > MAX_FONT {
            return Err(ERR_TOO_LARGE);
        }
        Ok(bytes)
    }

    /// # Errors
    /// `ERR_DECODE` when FreeType or cairo cannot use the bytes as a font.
    pub fn open(bytes: Vec<u8>) -> Result<Self, u32> {
        let library = freetype::Library::init().map_err(|_| ERR_DECODE)?;
        let ft = library.new_memory_face(Rc::new(bytes), 0).map_err(|_| ERR_DECODE)?;
        let face = cairo::FontFace::create_from_ft(&ft).map_err(|_| ERR_DECODE)?;
        Ok(Self { face, _ft: ft, _library: library })
    }

    /// The specimen `width` pixels wide on a transparent background in colour `rgb` (0xRRGGBB): "Aa Bb Cc" at 32
    /// logical px, the pangram and the digits at 13; `scale` = buffer pixels per logical px (in 1/100).
    ///
    /// # Errors
    /// `ERR_TOO_LARGE` for a size over the caps, `ERR_DECODE` when drawing fails.
    pub fn specimen(&self, width: u32, max_height: u32, rgb: u32, scale_centi: u32) -> Result<(Msg, OwnedFd), u32> {
        let scale = f64::from(scale_centi.clamp(100, 400)) / 100.0;
        if width == 0 || width > MAX_WIDTH {
            return Err(ERR_TOO_LARGE);
        }
        #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
        let height = ((32.0 + 16.0 + 20.0 + 4.0 + 20.0 + 12.0) * scale).ceil() as u32;
        if height > MAX_HEIGHT.min(max_height.max(1)) {
            return Err(ERR_TOO_LARGE);
        }
        let w = i32::try_from(width).map_err(|_| ERR_TOO_LARGE)?;
        let h = i32::try_from(height).map_err(|_| ERR_TOO_LARGE)?;
        let mut surface = cairo::ImageSurface::create(cairo::Format::ARgb32, w, h).map_err(|_| ERR_DECODE)?;
        {
            let cr = cairo::Context::new(&surface).map_err(|_| ERR_DECODE)?;
            cr.scale(scale, scale);
            cr.set_font_face(&self.face);
            let (r, g, b) = (f64::from((rgb >> 16) & 0xff), f64::from((rgb >> 8) & 0xff), f64::from(rgb & 0xff));
            cr.set_source_rgb(r / 255.0, g / 255.0, b / 255.0);
            cr.set_font_size(32.0);
            cr.move_to(0.0, 32.0);
            cr.show_text("Aa Bb Cc").map_err(|_| ERR_DECODE)?;
            cr.set_font_size(13.0);
            cr.move_to(0.0, 32.0 + 16.0 + 14.0);
            cr.show_text(SAMPLE).map_err(|_| ERR_DECODE)?;
            cr.move_to(0.0, 32.0 + 16.0 + 20.0 + 4.0 + 14.0);
            cr.show_text(DIGITS).map_err(|_| ERR_DECODE)?;
        }
        surface.flush();
        let stride = u32::try_from(surface.stride()).map_err(|_| ERR_DECODE)?;
        let data = surface.data().map_err(|_| ERR_DECODE)?;
        let fd = sealed("preview-font", &data)?;
        Ok((Msg::new(OP_PAGE, 0, width, height, stride), fd))
    }

    /// "Aa" centred on white in a `size` x `size` PNG (the freedesktop thumbnailer contract).
    ///
    /// # Errors
    /// `ERR_TOO_LARGE` over `MAX_THUMBNAIL`, `ERR_DECODE` when drawing or writing fails.
    pub fn thumbnail_png(&self, size: u32, out: &mut impl std::io::Write) -> Result<(), u32> {
        if size == 0 || size > MAX_THUMBNAIL {
            return Err(ERR_TOO_LARGE);
        }
        let side = i32::try_from(size).map_err(|_| ERR_TOO_LARGE)?;
        let surface = cairo::ImageSurface::create(cairo::Format::Rgb24, side, side).map_err(|_| ERR_DECODE)?;
        {
            let cr = cairo::Context::new(&surface).map_err(|_| ERR_DECODE)?;
            cr.set_source_rgb(1.0, 1.0, 1.0);
            cr.paint().map_err(|_| ERR_DECODE)?;
            cr.set_font_face(&self.face);
            cr.set_font_size(f64::from(size) * 0.5);
            let extents = cr.text_extents("Aa").map_err(|_| ERR_DECODE)?;
            cr.set_source_rgb(0.11, 0.11, 0.12);
            cr.move_to(
                (f64::from(size) - extents.width()) / 2.0 - extents.x_bearing(),
                (f64::from(size) - extents.height()) / 2.0 - extents.y_bearing(),
            );
            cr.show_text("Aa").map_err(|_| ERR_DECODE)?;
        }
        surface.write_to_png(out).map_err(|_| ERR_DECODE)
    }
}
