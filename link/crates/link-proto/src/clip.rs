//! The clipboard rules of `link/ARCHITECTURE.md` (Clipboard), per peer: which pulls an offerer serves, and which
//! `clip-data` streams a puller takes.

use crate::message::{ClipOffer, ClipPull};
use crate::transfer::Budget;

#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum ClipRefused {
    #[error("not the latest clipboard offer")]
    Stale,
    #[error("a type the offer does not have")]
    NoSuchType,
    #[error("nothing pulled that")]
    NotPulled,
}

/// What an answered pull is served from.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Serve {
    Text(String),
    Stream { size: u64 },
}

#[derive(Debug, Default)]
pub struct ClipState {
    next_id: u64,
    /// What this side offers the peer.
    sent: Option<ClipOffer>,
    /// What the peer offers this side.
    received: Option<ClipOffer>,
    pulling: Vec<ClipPull>,
}

impl ClipState {
    /// A new offer that replaces this side's previous one.
    pub fn offer(&mut self, mimes: Vec<String>, size: u64, text: Option<String>) -> ClipOffer {
        self.next_id += 1;
        let offer = ClipOffer { id: self.next_id, mimes, size, text };
        self.sent = Some(offer.clone());
        offer
    }

    /// Whether and from what the peer's pull is answered.
    pub fn on_pull(&self, pull: &ClipPull) -> Result<Serve, ClipRefused> {
        let offer = self.sent.as_ref().filter(|offer| offer.id == pull.id).ok_or(ClipRefused::Stale)?;
        if !offer.mimes.contains(&pull.mime) {
            return Err(ClipRefused::NoSuchType);
        }
        Ok(match &offer.text {
            Some(text) if crate::message::is_text_mime(&pull.mime) => Serve::Text(text.clone()),
            _ => Serve::Stream { size: offer.size },
        })
    }

    pub fn on_offer(&mut self, offer: ClipOffer) {
        self.pulling.clear();
        self.received = Some(offer);
    }

    pub fn received(&self) -> Option<&ClipOffer> {
        self.received.as_ref()
    }

    /// Starts pulling `mime` of the peer's offer `id`: from its inline text, or with a `clip-pull` to send.
    pub fn pull(&mut self, id: u64, mime: &str) -> Result<Serve, ClipRefused> {
        let offer = self.received.as_ref().filter(|offer| offer.id == id).ok_or(ClipRefused::Stale)?;
        if !offer.mimes.iter().any(|offered| offered == mime) {
            return Err(ClipRefused::NoSuchType);
        }
        if let Some(text) = offer.text.as_ref().filter(|_| crate::message::is_text_mime(mime)) {
            return Ok(Serve::Text(text.clone()));
        }
        let size = offer.size;
        self.pulling.push(ClipPull { id, mime: mime.to_owned() });
        Ok(Serve::Stream { size })
    }

    /// Admits a `clip-data` stream that answers one of this side's pulls, returning its byte budget.
    pub fn on_data(&mut self, header: &ClipPull) -> Result<Budget, ClipRefused> {
        let index = self.pulling.iter().position(|pull| pull == header).ok_or(ClipRefused::NotPulled)?;
        self.pulling.remove(index);
        let size = self.received.as_ref().filter(|offer| offer.id == header.id).map_or(0, |offer| offer.size);
        Ok(Budget::new(size))
    }
}

#[cfg(test)]
mod tests {
    use ciborium::Value;

    use super::*;
    use crate::message::{DecodeError, Envelope, MAX_CLIP_SIZE};

    fn mimes(list: &[&str]) -> Vec<String> {
        list.iter().map(|mime| (*mime).to_owned()).collect()
    }

    #[expect(clippy::expect_used, reason = "a cbor value always serializes into memory")]
    fn raw_offer(mimes: &[&str], size: u64, text: Option<&str>) -> Vec<u8> {
        let mut body = vec![
            ("id".into(), 1.into()),
            ("mimes".into(), Value::Array(mimes.iter().map(|mime| (*mime).into()).collect())),
            ("size".into(), size.into()),
        ];
        if let Some(text) = text {
            body.push(("text".into(), text.into()));
        }
        let raw = Value::Map(vec![
            ("type".into(), "clip-offer".into()),
            ("id".into(), 3.into()),
            ("body".into(), Value::Map(body)),
        ]);
        let mut out = Vec::new();
        ciborium::into_writer(&raw, &mut out).expect("cbor into memory");
        out
    }

    #[test]
    fn malformed_clip_offers_fail_to_decode() {
        let long = "x".repeat(256);
        let many: Vec<&str> = std::iter::repeat_n("text/plain", 17).collect();
        let cases = [
            raw_offer(&[], 1, None),
            raw_offer(&many, 1, None),
            raw_offer(&[long.as_str()], 1, None),
            raw_offer(&["image/png"], MAX_CLIP_SIZE + 1, None),
            raw_offer(&["image/png"], 4, Some("text")),
        ];
        for bytes in cases {
            let decoded = Envelope::from_cbor(&bytes);
            assert!(matches!(decoded, Err(DecodeError::Invalid(_) | DecodeError::Cbor(_))), "{decoded:?}");
        }
        assert!(Envelope::from_cbor(&raw_offer(&["text/plain;charset=utf-8"], 4, Some("text"))).is_ok());
    }

    #[test]
    fn only_the_latest_offer_and_its_types_are_served() {
        let mut state = ClipState::default();
        let first = state.offer(mimes(&["image/png"]), 10, None);
        let second = state.offer(mimes(&["text/plain", "image/png"]), 5, Some("hello".to_owned()));
        let pull = |id, mime: &str| ClipPull { id, mime: mime.to_owned() };
        assert_eq!(state.on_pull(&pull(first.id, "image/png")), Err(ClipRefused::Stale));
        assert_eq!(state.on_pull(&pull(second.id, "image/jpeg")), Err(ClipRefused::NoSuchType));
        assert_eq!(state.on_pull(&pull(second.id, "text/plain")), Ok(Serve::Text("hello".to_owned())));
        assert_eq!(state.on_pull(&pull(second.id, "image/png")), Ok(Serve::Stream { size: 5 }));
    }

    #[test]
    fn a_data_stream_must_answer_a_pull_and_fit_the_offer() {
        let mut state = ClipState::default();
        let header = ClipPull { id: 7, mime: "image/png".to_owned() };
        assert!(matches!(state.on_data(&header), Err(ClipRefused::NotPulled)));
        state.on_offer(ClipOffer { id: 7, mimes: mimes(&["image/png"]), size: 3, text: None });
        assert_eq!(state.pull(8, "image/png"), Err(ClipRefused::Stale));
        assert_eq!(state.pull(7, "image/png"), Ok(Serve::Stream { size: 3 }));
        let Ok(mut budget) = state.on_data(&header) else { panic!("an answered pull was refused") };
        assert!(budget.take(4).is_err());
        assert!(matches!(state.on_data(&header), Err(ClipRefused::NotPulled)));
    }
}
