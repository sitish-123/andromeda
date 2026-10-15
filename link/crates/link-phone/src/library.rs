//! A folder standing in for Android's media store: each subfolder an album, each file in it a photo. Thumbnails are
//! the PNGs themselves, so a test can check what reached the desktop byte for byte.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use link_core::browse::{Library, LibraryItem};
use link_core::messages::Messages;
use link_core::proto::message::{Contact, MessagesRefusal};

pub struct Folders(pub PathBuf);

impl Library for Folders {
    fn albums(&self) -> Vec<String> {
        let Ok(entries) = fs::read_dir(&self.0) else { return Vec::new() };
        entries
            .filter_map(Result::ok)
            .filter(|entry| entry.path().is_dir())
            .filter_map(|entry| entry.file_name().into_string().ok())
            .collect()
    }

    fn album(&self, name: &str) -> Vec<LibraryItem> {
        let Ok(entries) = fs::read_dir(self.0.join(name)) else { return Vec::new() };
        entries
            .filter_map(Result::ok)
            .filter_map(|entry| {
                let metadata = entry.metadata().ok().filter(fs::Metadata::is_file)?;
                Some(LibraryItem {
                    name: entry.file_name().into_string().ok()?,
                    path: entry.path(),
                    size: metadata.len(),
                    mtime: metadata.modified().ok()?.duration_since(UNIX_EPOCH).ok()?.as_secs(),
                })
            })
            .collect()
    }

    fn thumbnail(&self, path: &Path, _px: u16) -> Option<Vec<u8>> {
        fs::read(path).ok().filter(|bytes| bytes.starts_with(b"\x89PNG"))
    }
}

/// A JSON array of `{"name", "numbers"}` standing in for Android's contacts, read at each page so a test can change it.
pub struct Book(pub PathBuf);

impl Messages for Book {
    fn contacts(&self, offset: u32, limit: u32) -> Result<Vec<Contact>, MessagesRefusal> {
        let book: Vec<Contact> = fs::read(&self.0)
            .ok()
            .and_then(|json| serde_json::from_slice(&json).ok())
            .ok_or(MessagesRefusal::Failed)?;
        Ok(book.into_iter().skip(offset as usize).take(limit as usize).collect())
    }
}
