//! Arch news (<https://archlinux.org/feeds/news/>): items newer than the user's read token block System updates until
//! the user confirms "I have read this" (docs/system.md §1, the manual-intervention guard).

use anyhow::{Context, Result};
use quick_xml::Reader;
use quick_xml::events::Event;

pub const FEED: &str = "https://archlinux.org/feeds/news/";
const MAX_FEED_BYTES: u64 = 2 * 1024 * 1024;
const FETCH_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30);
const MAX_ITEMS: usize = 50;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Item {
    pub title: String,
    pub link: String,
    date: String,
    /// Unix seconds; 0 when the date does not parse (such an item never blocks).
    pub published: i64,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Field {
    None,
    Title,
    Link,
    Date,
}

/// RSS 2.0 `<item>`s; anything else in the feed is ignored. Bounded: at most 50 items.
pub fn parse(xml: &str) -> Result<Vec<Item>> {
    let mut reader = Reader::from_str(xml);
    let mut items = Vec::new();
    let mut current: Option<Item> = None;
    let mut field = Field::None;
    loop {
        match reader.read_event().context("news feed")? {
            Event::Start(e) => match e.local_name().as_ref() {
                b"item" => current = Some(Item::default()),
                b"title" if current.is_some() => field = Field::Title,
                b"link" if current.is_some() => field = Field::Link,
                b"pubDate" if current.is_some() => field = Field::Date,
                _ => field = Field::None,
            },
            Event::Text(t) => append(&mut current, field, &t.decode().context("news text")?),
            Event::CData(t) => append(&mut current, field, &String::from_utf8_lossy(&t)),
            Event::GeneralRef(r) => {
                if let Some(c) = r.resolve_char_ref().ok().flatten() {
                    append(&mut current, field, c.encode_utf8(&mut [0; 4]));
                } else {
                    let name: &[u8] = &r;
                    let entity = match name {
                        b"amp" => "&",
                        b"lt" => "<",
                        b"gt" => ">",
                        b"quot" => "\"",
                        b"apos" => "'",
                        _ => "",
                    };
                    append(&mut current, field, entity);
                }
            }
            Event::End(e) => {
                if e.local_name().as_ref() == b"item"
                    && let Some(mut item) = current.take()
                {
                    trim(&mut item.title);
                    trim(&mut item.link);
                    item.published = chrono::DateTime::parse_from_rfc2822(item.date.trim()).map_or(0, |d| d.timestamp());
                    items.push(item);
                    if items.len() == MAX_ITEMS {
                        break;
                    }
                }
                field = Field::None;
            }
            Event::Eof => break,
            _ => {}
        }
    }
    Ok(items)
}

fn trim(text: &mut String) {
    text.truncate(text.trim_end().len());
    let start = text.len() - text.trim_start().len();
    text.drain(..start);
}

fn append(current: &mut Option<Item>, field: Field, text: &str) {
    let Some(item) = current else { return };
    match field {
        Field::Title => item.title.push_str(text),
        Field::Link => item.link.push_str(text),
        Field::Date => item.date.push_str(text),
        Field::None => {}
    }
}

/// Items newer than `read_until` (unix seconds), newest first.
pub fn unread(items: &[Item], read_until: i64) -> Vec<Item> {
    let mut unread: Vec<Item> = items.iter().filter(|i| i.published > read_until).cloned().collect();
    unread.sort_by_key(|i| std::cmp::Reverse(i.published));
    unread
}

/// `ANDROMEDA_UPDATE_FEED` replaces the feed URL, for tests.
pub fn fetch() -> Result<Vec<Item>> {
    let url = std::env::var("ANDROMEDA_UPDATE_FEED").unwrap_or_else(|_| FEED.to_owned());
    let agent: ureq::Agent = ureq::Agent::config_builder().timeout_global(Some(FETCH_TIMEOUT)).build().into();
    let body = agent
        .get(&url)
        .call()
        .with_context(|| url.clone())?
        .into_body()
        .with_config()
        .limit(MAX_FEED_BYTES)
        .read_to_string()
        .with_context(|| url.clone())?;
    let items = parse(&body)?;
    // A 200 answer with no items (a captive portal page, a format change) is not a read feed.
    if items.is_empty() {
        anyhow::bail!("{url}: no news items in the answer");
    }
    Ok(items)
}

#[cfg(test)]
#[allow(clippy::expect_used)]
mod tests {
    use super::*;

    const FEED_XML: &str = r#"<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0"><channel><title>Arch Linux: Recent news updates</title>
<item><title>Manual intervention for pacman &amp; keyring</title>
<link>https://archlinux.org/news/one/</link><description>&lt;p&gt;body&lt;/p&gt;</description>
<pubDate>Sun, 01 Nov 2026 10:00:00 +0000</pubDate></item>
<item><title><![CDATA[Older item]]></title><link>https://archlinux.org/news/two/</link>
<pubDate>Mon, 01 Jun 2026 10:00:00 +0000</pubDate></item>
<item><title>No date</title><link>https://archlinux.org/news/three/</link></item>
</channel></rss>"#;

    #[test]
    fn parses_items_with_entities_cdata_and_dates() {
        let items = parse(FEED_XML).expect("feed");
        assert_eq!(items.len(), 3);
        assert_eq!(items[0].title, "Manual intervention for pacman & keyring");
        assert_eq!(items[0].link, "https://archlinux.org/news/one/");
        assert_eq!(items[0].published, 1_793_527_200);
        assert_eq!(items[1].title, "Older item");
        assert_eq!(items[2].published, 0);
    }

    #[test]
    fn only_items_after_the_read_token_are_unread() {
        let items = parse(FEED_XML).expect("feed");
        let unread = unread(&items, items[1].published);
        assert_eq!(unread.len(), 1);
        assert_eq!(unread[0].link, "https://archlinux.org/news/one/");
    }
}
