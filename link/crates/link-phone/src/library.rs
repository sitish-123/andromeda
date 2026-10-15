//! A folder standing in for Android's media store: each subfolder an album, each file in it a photo. Thumbnails are
//! the PNGs themselves, so a test can check what reached the desktop byte for byte.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use link_core::browse::{Library, LibraryItem};
use link_core::messages::Messages;
use link_core::proto::message::{Contact, MessagesRefusal, Text};

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

/// JSON files standing in for Android's contacts (`[{"name", "numbers"}]`) and texts (`[{"id", "thread", "address",
/// "body", "date", "sent"}]`, newest first), read at each page so a test can change them. Without a file, Android
/// denies access. A sent text goes first into the texts file and prints `{"event": "text-sent"}`; a text to `fail`
/// fails, as with no service.
pub struct Book {
    pub contacts: Option<PathBuf>,
    pub texts: Option<PathBuf>,
}

impl Messages for Book {
    fn contacts(&self, offset: u32, limit: u32) -> Result<Vec<Contact>, MessagesRefusal> {
        Ok(contacts(self.contacts.as_ref())?.into_iter().skip(offset as usize).take(limit as usize).collect())
    }

    fn texts(&self, offset: u32, limit: u32) -> Result<Vec<Text>, MessagesRefusal> {
        Ok(texts(self.texts.as_ref())?.into_iter().skip(offset as usize).take(limit as usize).collect())
    }

    fn send_text(&self, to: &str, body: &str) -> Result<(), MessagesRefusal> {
        let file = self.texts.as_ref().ok_or(MessagesRefusal::Denied)?;
        if to == "fail" {
            return Err(MessagesRefusal::Failed);
        }
        let mut texts = texts(Some(file))?;
        let id = texts.iter().map(|text| text.id).max().unwrap_or(0) + 1;
        let thread = texts.iter().find(|text| text.address == to).map_or(id, |text| text.thread);
        let date = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |since| u64::try_from(since.as_millis()).unwrap_or(u64::MAX));
        texts.insert(0, Text { id, thread, address: to.to_owned(), body: body.to_owned(), date, sent: true });
        fs::write(file, serde_json::to_vec(&texts).map_err(|_| MessagesRefusal::Failed)?)
            .map_err(|_| MessagesRefusal::Failed)?;
        println!("{}", serde_json::json!({ "event": "text-sent", "to": to, "body": body, "id": id }));
        Ok(())
    }
}

fn contacts(file: Option<&PathBuf>) -> Result<Vec<Contact>, MessagesRefusal> {
    serde_json::from_slice(&read(file)?).map_err(|_| MessagesRefusal::Failed)
}

fn texts(file: Option<&PathBuf>) -> Result<Vec<Text>, MessagesRefusal> {
    serde_json::from_slice(&read(file)?).map_err(|_| MessagesRefusal::Failed)
}

fn read(file: Option<&PathBuf>) -> Result<Vec<u8>, MessagesRefusal> {
    fs::read(file.ok_or(MessagesRefusal::Denied)?).map_err(|_| MessagesRefusal::Failed)
}
