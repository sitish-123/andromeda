//! Per-feature rate limits (`link/ARCHITECTURE.md`, Status and limits): a token bucket per feature and connection.

use std::time::{Duration, Instant};

use crate::message::Message;

/// `capacity` messages at once, then one per `refill`.
#[derive(Debug, Clone)]
pub struct Bucket {
    capacity: u32,
    refill: Duration,
    tokens: u32,
    since: Option<Instant>,
}

impl Bucket {
    pub const fn new(capacity: u32, refill: Duration) -> Self {
        Self { capacity, refill, tokens: capacity, since: None }
    }

    /// Takes a token if one is left at `now`.
    pub fn take(&mut self, now: Instant) -> bool {
        let since = *self.since.get_or_insert(now);
        let elapsed = now.saturating_duration_since(since);
        let earned = u32::try_from(elapsed.as_nanos() / self.refill.as_nanos().max(1)).unwrap_or(u32::MAX);
        if earned > 0 {
            self.tokens = self.tokens.saturating_add(earned).min(self.capacity);
            self.since = Some(since + self.refill * earned);
        }
        if self.tokens == 0 {
            return false;
        }
        if self.tokens == self.capacity {
            self.since = Some(now);
        }
        self.tokens -= 1;
        true
    }
}

/// What a desktop accepts from one phone connection before it drops the excess.
#[derive(Debug, Clone)]
pub struct Limits {
    shares: Bucket,
    offers: Bucket,
    clips: Bucket,
    statuses: Bucket,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            shares: Bucket::new(10, Duration::from_secs(2)),
            offers: Bucket::new(5, Duration::from_secs(6)),
            clips: Bucket::new(20, Duration::from_secs(1)),
            statuses: Bucket::new(3, Duration::from_secs(10)),
        }
    }
}

impl Limits {
    /// Whether `message` is within its feature's rate; messages without a limit always are.
    pub fn admit(&mut self, message: &Message, now: Instant) -> bool {
        let bucket = match message {
            Message::Share(_) => &mut self.shares,
            Message::Offer(_) => &mut self.offers,
            Message::ClipOffer(_) => &mut self.clips,
            Message::Status(_) => &mut self.statuses,
            _ => return true,
        };
        bucket.take(now)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::{Share, ShareKind};

    #[test]
    fn a_flood_is_cut_to_the_burst_then_the_rate() {
        let start = Instant::now();
        let share = Message::Share(Share { kind: ShareKind::Text, text: "x".to_owned() });
        let mut limits = Limits::default();
        let burst = (0..50).filter(|_| limits.admit(&share, start)).count();
        assert_eq!(burst, 10);
        assert!(!limits.admit(&share, start + Duration::from_secs(1)));
        assert!(limits.admit(&share, start + Duration::from_secs(2)));
        assert!(!limits.admit(&share, start + Duration::from_secs(3)));
        let later = (0..50).filter(|_| limits.admit(&share, start + Duration::from_secs(600))).count();
        assert_eq!(later, 10, "a quiet period refills the burst, not more");
    }

    #[test]
    fn features_have_their_own_buckets() {
        let now = Instant::now();
        let mut limits = Limits::default();
        let share = Message::Share(Share { kind: ShareKind::Text, text: "x".to_owned() });
        while limits.admit(&share, now) {}
        assert!(limits.admit(&Message::Unpair, now));
        let status = Message::Status(crate::message::Status {
            battery: 50,
            charging: false,
            network: crate::message::NetworkKind::Wifi,
        });
        assert!(limits.admit(&status, now));
    }
}
