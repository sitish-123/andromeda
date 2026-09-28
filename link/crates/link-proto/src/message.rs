use std::fmt::{self, Write as _};

use ciborium::Value;
use serde::{Deserialize, Serialize};

use crate::pairing::{MAC_LEN, SPAKE_MSG_LEN};

pub const MAX_NAME_LEN: usize = 64;
/// Leaves room for the envelope, so a share always fits one control frame.
pub const MAX_SHARE_LEN: usize = 60 * 1024;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Hello {
    pub version: u32,
    pub name: String,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub addresses: Vec<String>,
}

/// A Bluetooth address among a hello's `addresses`: `bt:` then six colon-separated hex octets. Phones from before
/// Bluetooth skip it, since it is not a socket address.
pub const BLUETOOTH_SCHEME: &str = "bt:";

/// The address in a `bt:` entry, upper-case, or None when `entry` is not one.
pub fn bluetooth_address(entry: &str) -> Option<String> {
    let address = entry.strip_prefix(BLUETOOTH_SCHEME)?;
    let octets: Vec<&str> = address.split(':').collect();
    let valid = octets.len() == 6
        && octets.iter().all(|octet| octet.len() == 2 && octet.bytes().all(|b| b.is_ascii_hexdigit()));
    valid.then(|| address.to_ascii_uppercase())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PairMethod {
    Code,
    Qr,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PairSpake {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub method: Option<PairMethod>,
    #[serde(with = "serde_bytes")]
    pub msg: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PairConfirm {
    #[serde(with = "serde_bytes")]
    pub mac: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ShareKind {
    Text,
    Link,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Share {
    pub kind: ShareKind,
    pub text: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ShareAck {
    /// The envelope id of the share this acknowledges.
    pub of: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum ShareRejected {
    #[error("the text is empty")]
    Empty,
    #[error("the text is longer than {MAX_SHARE_LEN} bytes")]
    TooLong,
    #[error("a link must be an http or https url")]
    NotWebLink,
}

impl ShareKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Text => "text",
            Self::Link => "link",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "text" => Some(Self::Text),
            "link" => Some(Self::Link),
            _ => None,
        }
    }
}

impl Share {
    /// The rules of a share, checked by the sender before sending and by the receiver while decoding.
    pub fn check(&self) -> Result<(), ShareRejected> {
        if self.text.is_empty() {
            return Err(ShareRejected::Empty);
        }
        if self.text.len() > MAX_SHARE_LEN {
            return Err(ShareRejected::TooLong);
        }
        if self.kind == ShareKind::Link && !is_web_link(&self.text) {
            return Err(ShareRejected::NotWebLink);
        }
        Ok(())
    }
}

/// An absolute http or https URL: scheme in any case, something after it, and no whitespace or control characters.
fn is_web_link(text: &str) -> bool {
    let rest = ["https://", "http://"].iter().find_map(|scheme| {
        let head = text.get(..scheme.len())?;
        head.eq_ignore_ascii_case(scheme).then(|| &text[scheme.len()..])
    });
    rest.is_some_and(|rest| !rest.is_empty()) && !text.chars().any(|c| c.is_whitespace() || c.is_control())
}

pub const MAX_NOTIFICATION_ID_LEN: usize = 256;
pub const MAX_APP_LEN: usize = 128;
pub const MAX_TITLE_LEN: usize = 512;
/// Also the longest reply.
pub const MAX_TEXT_LEN: usize = 4096;
pub const MAX_ICON_LEN: usize = 16 * 1024;
pub const MAX_ACTIONS: usize = 3;
pub const MAX_ACTION_LEN: usize = 64;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NotificationButton {
    pub id: String,
    pub label: String,
    /// Whether the action takes reply text (Android `RemoteInput`).
    pub reply: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NotificationPosted {
    pub id: String,
    pub app: String,
    pub title: String,
    pub text: String,
    /// PNG.
    #[serde(default, skip_serializing_if = "Option::is_none", with = "serde_bytes")]
    pub icon: Option<Vec<u8>>,
    pub actions: Vec<NotificationButton>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NotificationRemoved {
    pub id: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NotificationAction {
    pub id: String,
    pub action: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reply_text: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NotificationDismiss {
    pub id: String,
}

/// Byte length within `min..=max`, as CDDL's `.size` counts it.
fn sized(text: &str, min: usize, max: usize) -> bool {
    (min..=max).contains(&text.len())
}

fn notification_id(id: &str) -> bool {
    sized(id, 1, MAX_NOTIFICATION_ID_LEN)
}

impl NotificationPosted {
    fn valid(&self) -> bool {
        notification_id(&self.id)
            && sized(&self.app, 1, MAX_APP_LEN)
            && sized(&self.title, 0, MAX_TITLE_LEN)
            && sized(&self.text, 0, MAX_TEXT_LEN)
            && self.icon.as_ref().is_none_or(|icon| (1..=MAX_ICON_LEN).contains(&icon.len()))
            && self.actions.len() <= MAX_ACTIONS
            && self
                .actions
                .iter()
                .all(|action| sized(&action.id, 1, MAX_ACTION_LEN) && sized(&action.label, 1, MAX_ACTION_LEN))
    }
}

impl NotificationAction {
    fn valid(&self) -> bool {
        notification_id(&self.id)
            && sized(&self.action, 1, MAX_ACTION_LEN)
            && self.reply_text.as_ref().is_none_or(|reply| sized(reply, 1, MAX_TEXT_LEN))
    }
}

pub const MAX_PLAYER_LEN: usize = 64;
pub const MAX_METADATA_LEN: usize = 512;
pub const MAX_ARTWORK_LEN: usize = 48 * 1024;
pub const MAX_VOLUME: u8 = 100;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PlaybackState {
    Playing,
    Paused,
    Stopped,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum MediaCommandKind {
    Play,
    Pause,
    PlayPause,
    Next,
    Previous,
    /// `value` is the absolute position in ms.
    Seek,
    /// `value` is 0 to 100.
    Volume,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MediaPlayer {
    pub player: String,
    pub name: String,
    pub state: PlaybackState,
    pub title: String,
    pub artist: String,
    pub album: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub length_ms: Option<u64>,
    /// When sent; the receiver advances it while playing.
    pub position_ms: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub volume: Option<u8>,
    /// PNG or JPEG.
    #[serde(default, skip_serializing_if = "Option::is_none", with = "serde_bytes")]
    pub artwork: Option<Vec<u8>>,
    pub can: Vec<MediaCommandKind>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MediaGone {
    pub player: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MediaCommand {
    pub player: String,
    pub command: MediaCommandKind,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub value: Option<u64>,
}

/// Asks the receiver to start or stop ringing.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Ring {
    pub on: bool,
}

/// The sender's own ringing started or stopped.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Ringing {
    pub on: bool,
}

/// The phone's own hotspot, offered over Bluetooth so the desktop can join it for full speed.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Hotspot {
    pub ssid: String,
    pub passphrase: String,
}

/// Where the desktop listens on the phone's hotspot, once it joined.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HotspotJoined {
    pub address: String,
}

/// Either side is done with the hotspot, or could not start or join it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HotspotEnd {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

pub const MAX_SSID_LEN: usize = 32;
pub const MAX_REASON_LEN: usize = 128;

impl Hotspot {
    /// WPA2-PSK rules: an SSID of 1 to 32 bytes, a passphrase of 8 to 63 printable ASCII characters.
    fn valid(&self) -> bool {
        (1..=MAX_SSID_LEN).contains(&self.ssid.len())
            && (8..=63).contains(&self.passphrase.len())
            && self.passphrase.bytes().all(|byte| (0x20..0x7f).contains(&byte))
    }
}

/// Lists a folder on the phone. `path` is `/` for the phone's roots, else `/<root>/<name>/...`; `cursor` continues a
/// listing from its `next`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FsList {
    pub req: u64,
    pub path: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cursor: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FsEntry {
    pub name: String,
    pub dir: bool,
    pub size: u64,
    /// Unix seconds.
    pub mtime: u64,
}

/// One page of a listing; `next` is the cursor for the rest, when there is more.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FsEntries {
    pub req: u64,
    pub entries: Vec<FsEntry>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub next: Option<u32>,
}

/// Reads `len` bytes of a file from `offset`; answered by `fs-data` frames in order, the last one marked.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FsRead {
    pub req: u64,
    pub path: String,
    pub offset: u64,
    pub len: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FsData {
    pub req: u64,
    pub offset: u64,
    #[serde(with = "serde_bytes")]
    pub data: Vec<u8>,
    /// The read is complete: all it asked for, or the end of the file.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub last: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum FsRefusal {
    NotFound,
    /// Android does not let the app read it, or the path leaves its root.
    Denied,
    /// The phone's browse switch for this desktop is off.
    NotAllowed,
    NotAFolder,
    NotAFile,
    Busy,
    Io,
}

impl FsRefusal {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::NotFound => "not-found",
            Self::Denied => "denied",
            Self::NotAllowed => "not-allowed",
            Self::NotAFolder => "not-a-folder",
            Self::NotAFile => "not-a-file",
            Self::Busy => "busy",
            Self::Io => "io",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FsError {
    pub req: u64,
    pub reason: FsRefusal,
}

pub const MAX_FS_PATH: usize = 4096;
pub const MAX_FS_NAME: usize = 255;
/// Entries per `fs-entries`, so a page of the longest names still fits a control frame.
pub const MAX_FS_ENTRIES: usize = 128;
pub const MAX_FS_READ: u32 = 1024 * 1024;
/// Bytes per `fs-data`, so a read never holds the control stream for long.
pub const MAX_FS_CHUNK: usize = 48 * 1024;

/// A browse path: `/`, or `/` then components that are not empty, `.`, or `..`, with no NUL.
pub fn fs_path_valid(path: &str) -> bool {
    path.len() <= MAX_FS_PATH
        && !path.contains('\0')
        && (path == "/"
            || path.strip_prefix('/').is_some_and(|rest| rest.split('/').all(|part| !matches!(part, "" | "." | ".."))))
}

fn fs_name_valid(name: &str) -> bool {
    sized(name, 1, MAX_FS_NAME) && !name.contains('/') && !name.contains('\0') && name != "." && name != ".."
}

pub const MAX_NUMBER_LEN: usize = 64;
pub const MAX_CALLER_LEN: usize = 128;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum CallState {
    Ringing,
    Active,
    Idle,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Call {
    pub state: CallState,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub number: Option<String>,
    /// The contact's name, when the phone may read contacts.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum CallActionKind {
    /// Silences the ringer until the call ends.
    Mute,
    /// Ends the ringing call.
    Decline,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CallAction {
    pub action: CallActionKind,
}

impl CallState {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Ringing => "ringing",
            Self::Active => "active",
            Self::Idle => "idle",
        }
    }
}

impl CallActionKind {
    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "mute" => Some(Self::Mute),
            "decline" => Some(Self::Decline),
            _ => None,
        }
    }
}

fn player_id(player: &str) -> bool {
    sized(player, 1, MAX_PLAYER_LEN)
}

impl MediaPlayer {
    fn valid(&self) -> bool {
        let mut seen = std::collections::HashSet::new();
        player_id(&self.player)
            && sized(&self.name, 1, MAX_PLAYER_LEN)
            && [&self.title, &self.artist, &self.album].iter().all(|text| sized(text, 0, MAX_METADATA_LEN))
            && self.volume.is_none_or(|volume| volume <= MAX_VOLUME)
            && self.artwork.as_ref().is_none_or(|art| (1..=MAX_ARTWORK_LEN).contains(&art.len()))
            && self.can.iter().all(|command| seen.insert(*command))
    }
}

impl MediaCommand {
    fn valid(&self) -> bool {
        let value_ok = match self.command {
            MediaCommandKind::Seek => self.value.is_some(),
            MediaCommandKind::Volume => self.value.is_some_and(|volume| volume <= u64::from(MAX_VOLUME)),
            _ => self.value.is_none(),
        };
        player_id(&self.player) && value_ok
    }
}

/// A transfer's id: 16 random bytes chosen by the sender.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct TransferId(pub [u8; TRANSFER_ID_LEN]);

pub const TRANSFER_ID_LEN: usize = 16;
pub const SHA256_LEN: usize = 32;
/// Longer names are cut to 255 bytes on receipt; this only bounds what a frame may carry.
pub const MAX_WIRE_NAME_LEN: usize = 4096;
pub const MAX_MIME_LEN: usize = 255;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FileMeta {
    pub id: u64,
    pub name: String,
    pub size: u64,
    pub mime: String,
    #[serde(with = "serde_bytes")]
    pub sha256: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Offer {
    pub transfer: TransferId,
    pub files: Vec<FileMeta>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum RefuseReason {
    Declined,
    NoSpace,
    TooLarge,
    Busy,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct OfferReply {
    pub transfer: TransferId,
    pub accepted: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<RefuseReason>,
}

/// The body of `resume` and `cancel`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TransferRef {
    pub transfer: TransferId,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FileOffset {
    pub file: u64,
    pub offset: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ResumeAt {
    pub transfer: TransferId,
    pub offsets: Vec<FileOffset>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FileDone {
    pub transfer: TransferId,
    pub file: u64,
    pub ok: bool,
}

/// The first frame of a file's unidirectional stream; the bytes from `offset` to the file's size follow.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FileData {
    pub transfer: TransferId,
    pub file: u64,
    pub offset: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum NetworkKind {
    Wifi,
    Cellular,
    Ethernet,
    None,
    Other,
}

/// The phone's battery and network, sent on connect and on change.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Status {
    /// Percent, 0 to 100.
    pub battery: u8,
    pub charging: bool,
    pub network: NetworkKind,
}

impl NetworkKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Wifi => "wifi",
            Self::Cellular => "cellular",
            Self::Ethernet => "ethernet",
            Self::None => "none",
            Self::Other => "other",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        Some(match text {
            "wifi" => Self::Wifi,
            "cellular" => Self::Cellular,
            "ethernet" => Self::Ethernet,
            "none" => Self::None,
            "other" => Self::Other,
            _ => return None,
        })
    }
}

pub const MAX_CLIP_MIMES: usize = 16;
pub const MAX_CLIP_SIZE: u64 = 64 << 20;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ClipOffer {
    pub id: u64,
    pub mimes: Vec<String>,
    pub size: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub text: Option<String>,
}

/// The body of `clip-pull`, and of `clip-data`, the first frame of the stream that answers it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ClipPull {
    pub id: u64,
    pub mime: String,
}

impl ClipOffer {
    pub fn is_valid(&self) -> bool {
        let mimes_ok = (1..=MAX_CLIP_MIMES).contains(&self.mimes.len())
            && self.mimes.iter().all(|mime| (1..=MAX_MIME_LEN).contains(&mime.len()));
        let text_ok = self.text.as_ref().is_none_or(|text| {
            (1..=MAX_SHARE_LEN).contains(&text.len()) && self.mimes.iter().any(|mime| is_text_mime(mime))
        });
        mimes_ok && text_ok && self.size <= MAX_CLIP_SIZE
    }
}

/// `text/plain`, with or without parameters.
pub fn is_text_mime(mime: &str) -> bool {
    mime.split(';').next().is_some_and(|base| base.trim().eq_ignore_ascii_case("text/plain"))
}

impl RefuseReason {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Declined => "declined",
            Self::NoSpace => "no-space",
            Self::TooLarge => "too-large",
            Self::Busy => "busy",
        }
    }
}

impl TransferId {
    pub fn to_hex(self) -> String {
        self.0.iter().fold(String::with_capacity(2 * TRANSFER_ID_LEN), |mut hex, byte| {
            let _ = write!(hex, "{byte:02x}");
            hex
        })
    }

    pub fn parse_hex(text: &str) -> Option<Self> {
        if text.len() != 2 * TRANSFER_ID_LEN || !text.is_ascii() {
            return None;
        }
        let mut id = [0; TRANSFER_ID_LEN];
        for (index, byte) in id.iter_mut().enumerate() {
            *byte = u8::from_str_radix(&text[2 * index..2 * index + 2], 16).ok()?;
        }
        Some(Self(id))
    }
}

impl fmt::Debug for TransferId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_hex())
    }
}

impl fmt::Display for TransferId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_hex())
    }
}

impl Serialize for TransferId {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_bytes(&self.0)
    }
}

impl<'de> Deserialize<'de> for TransferId {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let bytes = serde_bytes::ByteBuf::deserialize(deserializer)?;
        let id = <[u8; TRANSFER_ID_LEN]>::try_from(bytes.as_slice())
            .map_err(|_| serde::de::Error::invalid_length(bytes.len(), &"16 bytes"))?;
        Ok(Self(id))
    }
}

impl Offer {
    /// The rules of an offer, checked by the sender before sending and by the receiver while decoding.
    pub fn is_valid(&self) -> bool {
        !self.files.is_empty()
            && unique(self.files.iter().map(|file| file.id))
            && self.files.iter().all(FileMeta::is_valid)
    }

    pub fn total_size(&self) -> u64 {
        self.files.iter().map(|file| file.size).fold(0, u64::saturating_add)
    }
}

impl FileMeta {
    fn is_valid(&self) -> bool {
        (1..=MAX_WIRE_NAME_LEN).contains(&self.name.len())
            && (1..=MAX_MIME_LEN).contains(&self.mime.len())
            && self.sha256.len() == SHA256_LEN
    }
}

fn unique(ids: impl Iterator<Item = u64>) -> bool {
    let mut ids: Vec<u64> = ids.collect();
    let count = ids.len();
    ids.sort_unstable();
    ids.dedup();
    ids.len() == count
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Empty {}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Message {
    Hello(Hello),
    PairSpake(PairSpake),
    PairConfirm(PairConfirm),
    Unpair,
    Share(Share),
    ShareAck(ShareAck),
    NotificationPosted(NotificationPosted),
    NotificationRemoved(NotificationRemoved),
    NotificationAction(NotificationAction),
    NotificationDismiss(NotificationDismiss),
    MediaPlayer(MediaPlayer),
    MediaGone(MediaGone),
    MediaCommand(MediaCommand),
    Ring(Ring),
    Ringing(Ringing),
    Call(Call),
    CallAction(CallAction),
    Offer(Offer),
    OfferReply(OfferReply),
    Resume(TransferRef),
    ResumeAt(ResumeAt),
    Cancel(TransferRef),
    FileDone(FileDone),
    FileData(FileData),
    ClipOffer(ClipOffer),
    ClipPull(ClipPull),
    ClipData(ClipPull),
    Status(Status),
    HotspotRequest,
    Hotspot(Hotspot),
    HotspotJoined(HotspotJoined),
    HotspotEnd(HotspotEnd),
    FsList(FsList),
    FsEntries(FsEntries),
    FsRead(FsRead),
    FsData(FsData),
    FsError(FsError),
}

impl Message {
    pub fn kind(&self) -> &'static str {
        match self {
            Self::Hello(_) => "hello",
            Self::PairSpake(_) => "pair-spake",
            Self::PairConfirm(_) => "pair-confirm",
            Self::Unpair => "unpair",
            Self::Share(_) => "share",
            Self::ShareAck(_) => "share-ack",
            Self::NotificationPosted(_) => "notification-posted",
            Self::NotificationRemoved(_) => "notification-removed",
            Self::NotificationAction(_) => "notification-action",
            Self::NotificationDismiss(_) => "notification-dismiss",
            Self::MediaPlayer(_) => "media-player",
            Self::MediaGone(_) => "media-gone",
            Self::MediaCommand(_) => "media-command",
            Self::Ring(_) => "ring",
            Self::Ringing(_) => "ringing",
            Self::Call(_) => "call",
            Self::CallAction(_) => "call-action",
            Self::Offer(_) => "offer",
            Self::OfferReply(_) => "offer-reply",
            Self::Resume(_) => "resume",
            Self::ResumeAt(_) => "resume-at",
            Self::Cancel(_) => "cancel",
            Self::FileDone(_) => "file-done",
            Self::FileData(_) => "file-data",
            Self::ClipOffer(_) => "clip-offer",
            Self::ClipPull(_) => "clip-pull",
            Self::ClipData(_) => "clip-data",
            Self::Status(_) => "status",
            Self::HotspotRequest => "hotspot-request",
            Self::Hotspot(_) => "hotspot",
            Self::HotspotJoined(_) => "hotspot-joined",
            Self::HotspotEnd(_) => "hotspot-end",
            Self::FsList(_) => "fs-list",
            Self::FsEntries(_) => "fs-entries",
            Self::FsRead(_) => "fs-read",
            Self::FsData(_) => "fs-data",
            Self::FsError(_) => "fs-error",
        }
    }

    fn body(&self) -> Result<Value, ciborium::value::Error> {
        match self {
            Self::Hello(body) => Value::serialized(body),
            Self::PairSpake(body) => Value::serialized(body),
            Self::PairConfirm(body) => Value::serialized(body),
            Self::Unpair | Self::HotspotRequest => Value::serialized(&Empty {}),
            Self::Share(body) => Value::serialized(body),
            Self::ShareAck(body) => Value::serialized(body),
            Self::NotificationPosted(body) => Value::serialized(body),
            Self::NotificationRemoved(body) => Value::serialized(body),
            Self::NotificationAction(body) => Value::serialized(body),
            Self::NotificationDismiss(body) => Value::serialized(body),
            Self::MediaPlayer(body) => Value::serialized(body),
            Self::MediaGone(body) => Value::serialized(body),
            Self::MediaCommand(body) => Value::serialized(body),
            Self::Ring(body) => Value::serialized(body),
            Self::Ringing(body) => Value::serialized(body),
            Self::Call(body) => Value::serialized(body),
            Self::CallAction(body) => Value::serialized(body),
            Self::Offer(body) => Value::serialized(body),
            Self::OfferReply(body) => Value::serialized(body),
            Self::Resume(body) | Self::Cancel(body) => Value::serialized(body),
            Self::ResumeAt(body) => Value::serialized(body),
            Self::FileDone(body) => Value::serialized(body),
            Self::FileData(body) => Value::serialized(body),
            Self::ClipOffer(body) => Value::serialized(body),
            Self::ClipPull(body) | Self::ClipData(body) => Value::serialized(body),
            Self::Status(body) => Value::serialized(body),
            Self::Hotspot(body) => Value::serialized(body),
            Self::HotspotJoined(body) => Value::serialized(body),
            Self::HotspotEnd(body) => Value::serialized(body),
            Self::FsList(body) => Value::serialized(body),
            Self::FsEntries(body) => Value::serialized(body),
            Self::FsRead(body) => Value::serialized(body),
            Self::FsData(body) => Value::serialized(body),
            Self::FsError(body) => Value::serialized(body),
        }
    }

    fn from_parts(kind: &str, body: &Value) -> Result<Self, DecodeError> {
        let message = match kind {
            "hello" => Self::Hello(body.deserialized()?),
            "pair-spake" => Self::PairSpake(body.deserialized()?),
            "pair-confirm" => Self::PairConfirm(body.deserialized()?),
            "unpair" => {
                body.deserialized::<Empty>()?;
                Self::Unpair
            }
            "share" => Self::Share(body.deserialized()?),
            "share-ack" => Self::ShareAck(body.deserialized()?),
            "notification-posted" => Self::NotificationPosted(body.deserialized()?),
            "notification-removed" => Self::NotificationRemoved(body.deserialized()?),
            "notification-action" => Self::NotificationAction(body.deserialized()?),
            "notification-dismiss" => Self::NotificationDismiss(body.deserialized()?),
            "media-player" => Self::MediaPlayer(body.deserialized()?),
            "media-gone" => Self::MediaGone(body.deserialized()?),
            "media-command" => Self::MediaCommand(body.deserialized()?),
            "ring" => Self::Ring(body.deserialized()?),
            "ringing" => Self::Ringing(body.deserialized()?),
            "call" => Self::Call(body.deserialized()?),
            "call-action" => Self::CallAction(body.deserialized()?),
            "offer" => Self::Offer(body.deserialized()?),
            "offer-reply" => Self::OfferReply(body.deserialized()?),
            "resume" => Self::Resume(body.deserialized()?),
            "resume-at" => Self::ResumeAt(body.deserialized()?),
            "cancel" => Self::Cancel(body.deserialized()?),
            "file-done" => Self::FileDone(body.deserialized()?),
            "file-data" => Self::FileData(body.deserialized()?),
            "clip-offer" => Self::ClipOffer(body.deserialized()?),
            "clip-pull" => Self::ClipPull(body.deserialized()?),
            "clip-data" => Self::ClipData(body.deserialized()?),
            "status" => Self::Status(body.deserialized()?),
            "hotspot-request" => {
                body.deserialized::<Empty>()?;
                Self::HotspotRequest
            }
            "hotspot" => Self::Hotspot(body.deserialized()?),
            "hotspot-joined" => Self::HotspotJoined(body.deserialized()?),
            "hotspot-end" => Self::HotspotEnd(body.deserialized()?),
            "fs-list" => Self::FsList(body.deserialized()?),
            "fs-entries" => Self::FsEntries(body.deserialized()?),
            "fs-read" => Self::FsRead(body.deserialized()?),
            "fs-data" => Self::FsData(body.deserialized()?),
            "fs-error" => Self::FsError(body.deserialized()?),
            other => return Err(DecodeError::UnknownType(other.to_owned())),
        };
        message.validate()?;
        Ok(message)
    }

    /// The schema's rules, checked by the receiver while decoding and by senders before sending.
    pub fn validate(&self) -> Result<(), DecodeError> {
        let valid = match self {
            Self::Hello(hello) => (1..=MAX_NAME_LEN).contains(&hello.name.chars().count()),
            Self::PairSpake(spake) => spake.msg.len() == SPAKE_MSG_LEN,
            Self::PairConfirm(confirm) => confirm.mac.len() == MAC_LEN,
            Self::Unpair
            | Self::HotspotRequest
            | Self::ShareAck(_)
            | Self::Ring(_)
            | Self::Ringing(_)
            | Self::CallAction(_)
            | Self::Resume(_)
            | Self::Cancel(_)
            | Self::FileDone(_)
            | Self::FileData(_)
            | Self::FsError(_) => true,
            Self::Call(call) => {
                call.number.as_ref().is_none_or(|number| sized(number, 1, MAX_NUMBER_LEN))
                    && call.name.as_ref().is_none_or(|name| sized(name, 1, MAX_CALLER_LEN))
            }
            Self::Share(share) => share.check().is_ok(),
            Self::NotificationPosted(posted) => posted.valid(),
            Self::NotificationRemoved(NotificationRemoved { id })
            | Self::NotificationDismiss(NotificationDismiss { id }) => notification_id(id),
            Self::NotificationAction(action) => action.valid(),
            Self::MediaPlayer(player) => player.valid(),
            Self::MediaGone(gone) => player_id(&gone.player),
            Self::MediaCommand(command) => command.valid(),
            Self::ClipPull(pull) | Self::ClipData(pull) => (1..=MAX_MIME_LEN).contains(&pull.mime.len()),
            Self::ClipOffer(offer) => offer.is_valid(),
            Self::Status(status) => status.battery <= 100,
            Self::Offer(offer) => offer.is_valid(),
            Self::OfferReply(reply) => reply.accepted == reply.reason.is_none(),
            Self::ResumeAt(at) => unique(at.offsets.iter().map(|offset| offset.file)),
            Self::Hotspot(hotspot) => hotspot.valid(),
            Self::HotspotJoined(joined) => joined.address.parse::<std::net::SocketAddr>().is_ok(),
            Self::HotspotEnd(end) => end.reason.as_ref().is_none_or(|reason| sized(reason, 1, MAX_REASON_LEN)),
            Self::FsList(list) => fs_path_valid(&list.path),
            Self::FsEntries(page) => {
                page.entries.len() <= MAX_FS_ENTRIES && page.entries.iter().all(|entry| fs_name_valid(&entry.name))
            }
            Self::FsRead(read) => fs_path_valid(&read.path) && (1..=MAX_FS_READ).contains(&read.len),
            Self::FsData(data) => data.data.len() <= MAX_FS_CHUNK,
        };
        if valid { Ok(()) } else { Err(DecodeError::Invalid(self.kind())) }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Envelope {
    pub id: u64,
    pub message: Message,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct RawEnvelope {
    #[serde(rename = "type")]
    kind: String,
    id: u64,
    body: Value,
}

#[derive(Debug, thiserror::Error)]
pub enum DecodeError {
    #[error("malformed cbor: {0}")]
    Cbor(String),
    #[error("unknown message type {0:?}")]
    UnknownType(String),
    #[error("{0} message violates the schema")]
    Invalid(&'static str),
}

impl From<ciborium::value::Error> for DecodeError {
    fn from(error: ciborium::value::Error) -> Self {
        Self::Cbor(error.to_string())
    }
}

impl Envelope {
    pub fn new(id: u64, message: Message) -> Self {
        Self { id, message }
    }

    #[expect(clippy::expect_used, reason = "our own message types always serialize into memory")]
    pub fn to_cbor(&self) -> Vec<u8> {
        let raw = RawEnvelope {
            kind: self.message.kind().to_owned(),
            id: self.id,
            body: self.message.body().expect("message body serializes"),
        };
        let mut out = Vec::new();
        ciborium::into_writer(&raw, &mut out).expect("envelope serializes");
        out
    }

    pub fn from_cbor(bytes: &[u8]) -> Result<Self, DecodeError> {
        let mut reader = std::io::Cursor::new(bytes);
        let raw: RawEnvelope =
            ciborium::from_reader(&mut reader).map_err(|error| DecodeError::Cbor(error.to_string()))?;
        if usize::try_from(reader.position()).ok() != Some(bytes.len()) {
            return Err(DecodeError::Cbor("trailing bytes after the envelope".to_owned()));
        }
        Ok(Self { id: raw.id, message: Message::from_parts(&raw.kind, &raw.body)? })
    }
}
