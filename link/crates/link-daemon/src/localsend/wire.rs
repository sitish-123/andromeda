//! LocalSend v2 JSON (<https://github.com/localsend/protocol>). Unknown fields are allowed, since every LocalSend app
//! adds its own; the fields used are checked where they are used.

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

pub const PORT: u16 = 53317;
pub const MULTICAST: [u8; 4] = [224, 0, 0, 167];
pub const VERSION: &str = "2.1";

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Info {
    pub alias: String,
    #[serde(default)]
    pub version: String,
    #[serde(default)]
    pub device_model: Option<String>,
    #[serde(default)]
    pub device_type: Option<String>,
    pub fingerprint: String,
    #[serde(default = "default_port")]
    pub port: u16,
    #[serde(default = "default_protocol")]
    pub protocol: String,
    #[serde(default)]
    pub download: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Announcement {
    #[serde(flatten)]
    pub info: Info,
    #[serde(default)]
    pub announce: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FileInfo {
    pub id: String,
    pub file_name: String,
    pub size: u64,
    #[serde(default)]
    pub file_type: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sha256: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PrepareUpload {
    pub info: Info,
    pub files: BTreeMap<String, FileInfo>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Prepared {
    pub session_id: String,
    pub files: BTreeMap<String, String>,
}

fn default_port() -> u16 {
    PORT
}

fn default_protocol() -> String {
    "https".to_owned()
}
