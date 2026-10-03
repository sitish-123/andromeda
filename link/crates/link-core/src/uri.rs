//! The pairing URI the desktop shows as a QR code:
//! `umbriel-link:pair?v=1&k=<sha256(spki)>&c=<secret>&a=<host:port>...&b=<bluetooth address>`, binary fields
//! base64url without padding. Unknown parameters are skipped, so later fields do not break this reader.

use std::fmt;
use std::net::SocketAddr;

use base64::Engine;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;

use link_proto::message::{BLUETOOTH_SCHEME, bluetooth_address};

use crate::Error;

const PREFIX: &str = "umbriel-link:pair?";
pub const QR_SECRET_LEN: usize = 16;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PairingUri {
    pub fingerprint: [u8; 32],
    pub secret: [u8; QR_SECRET_LEN],
    pub addresses: Vec<SocketAddr>,
    /// The desktop's Bluetooth adapter, upper-case, for pairing where no address answers.
    pub bluetooth: Option<String>,
}

impl fmt::Display for PairingUri {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{PREFIX}v=1&k={}&c={}",
            URL_SAFE_NO_PAD.encode(self.fingerprint),
            URL_SAFE_NO_PAD.encode(self.secret)
        )?;
        for addr in &self.addresses {
            write!(f, "&a={}", addr.to_string().replace('[', "%5B").replace(']', "%5D"))?;
        }
        if let Some(bluetooth) = &self.bluetooth {
            write!(f, "&b={}", bluetooth.replace(':', "%3A"))?;
        }
        Ok(())
    }
}

impl PairingUri {
    pub fn parse(text: &str) -> Result<Self, Error> {
        let query = text.strip_prefix(PREFIX).ok_or(Error::BadUri("not a pairing uri"))?;
        let (mut version, mut fingerprint, mut secret, mut addresses, mut bluetooth) =
            (None, None, None, Vec::new(), None);
        for pair in query.split('&') {
            let (key, value) = pair.split_once('=').ok_or(Error::BadUri("parameter without a value"))?;
            match key {
                "v" => version = Some(value),
                "k" => fingerprint = Some(decode_fixed(value)?),
                "c" => secret = Some(decode_fixed(value)?),
                "a" => addresses.push(parse_addr(value)?),
                "b" => {
                    let entry = format!("{BLUETOOTH_SCHEME}{}", value.replace("%3A", ":"));
                    bluetooth = Some(bluetooth_address(&entry).ok_or(Error::BadUri("bad Bluetooth address"))?);
                }
                _ => {}
            }
        }
        if version != Some("1") {
            return Err(Error::BadUri("unsupported version"));
        }
        Ok(Self {
            fingerprint: fingerprint.ok_or(Error::BadUri("missing key"))?,
            secret: secret.ok_or(Error::BadUri("missing secret"))?,
            addresses,
            bluetooth,
        })
    }
}

fn decode_fixed<const N: usize>(value: &str) -> Result<[u8; N], Error> {
    let bytes = URL_SAFE_NO_PAD.decode(value).map_err(|_| Error::BadUri("bad base64"))?;
    bytes.try_into().map_err(|_| Error::BadUri("wrong length"))
}

fn parse_addr(value: &str) -> Result<SocketAddr, Error> {
    value.replace("%5B", "[").replace("%5D", "]").parse().map_err(|_| Error::BadUri("bad address"))
}
