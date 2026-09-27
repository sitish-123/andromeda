//! The phone notifications on show, per device, kept across sessions so that a reconnect's re-post shows nothing new.

use std::collections::HashMap;

use link_core::identity::DeviceId;
use link_core::proto::message::NotificationPosted;

/// Live notifications per device; a new one beyond this removes the oldest.
const MAX_LIVE: usize = 64;

#[derive(Debug, PartialEq, Eq)]
pub enum Change {
    Posted(NotificationPosted),
    Removed(String),
}

#[derive(Default)]
pub struct Mirror {
    /// Oldest first.
    live: HashMap<DeviceId, Vec<NotificationPosted>>,
}

impl Mirror {
    /// What a post changes: nothing for an identical re-post; the oldest's removal first when the device is full.
    pub fn post(&mut self, device: &DeviceId, posted: NotificationPosted) -> Vec<Change> {
        let live = self.live.entry(device.clone()).or_default();
        if let Some(index) = live.iter().position(|shown| shown.id == posted.id) {
            if live[index] == posted {
                return Vec::new();
            }
            live.remove(index);
        }
        let mut changes = Vec::new();
        if live.len() >= MAX_LIVE {
            changes.push(Change::Removed(live.remove(0).id));
        }
        live.push(posted.clone());
        changes.push(Change::Posted(posted));
        changes
    }

    /// Whether `id` was on show.
    pub fn remove(&mut self, device: &DeviceId, id: &str) -> bool {
        let Some(live) = self.live.get_mut(device) else { return false };
        let before = live.len();
        live.retain(|shown| shown.id != id);
        live.len() != before
    }

    /// Drops every notification of `device` and returns their ids.
    pub fn forget(&mut self, device: &DeviceId) -> Vec<String> {
        self.live.remove(device).unwrap_or_default().into_iter().map(|shown| shown.id).collect()
    }
}
