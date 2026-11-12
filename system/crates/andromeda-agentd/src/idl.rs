//! The action entry a provider sends in its hello: one `help.json` entry of `tools/idl/gen.py`.

use std::fmt::Write;

use serde::Deserialize;
use serde_json::{Map, Value};

use crate::proto::{Code, closest, error_object};

pub const SCHEMA_VERSION: u64 = 1;

#[derive(Deserialize)]
pub struct ArgEntry {
    pub name: String,
    #[serde(rename = "type")]
    pub ty: String,
    pub required: bool,
    #[serde(default)]
    pub example: Option<String>,
}

/// `raw` keeps the whole entry for `help`, so fields the broker does not read still reach clients.
pub struct ActionEntry {
    pub name: String,
    pub args: Vec<ArgEntry>,
    pub raw: Value,
}

#[derive(Deserialize)]
struct Parsed {
    name: String,
    #[serde(default)]
    args: Vec<ArgEntry>,
}

impl ActionEntry {
    pub fn parse(raw: Value) -> Result<Self, serde_json::Error> {
        let Parsed { name, args } = Parsed::deserialize(&raw)?;
        Ok(Self { name, args, raw })
    }

    /// `andromeda act <name>` with every required arg and its example value.
    pub fn example(&self) -> String {
        let mut line = format!("andromeda act {}", self.name);
        for arg in self.args.iter().filter(|a| a.required) {
            let _ = write!(line, " --{} {}", arg.name, arg.example.as_deref().unwrap_or("…"));
        }
        line
    }

    /// Checks names, required args and builtin types; the provider still checks named types and values.
    pub fn check_args(&self, args: &Map<String, Value>) -> Result<(), Value> {
        let invalid =
            |message: String, hint: String| Err(error_object(Code::InvalidArgument, &message, &hint, &self.example()));
        for name in args.keys() {
            if !self.args.iter().any(|a| &a.name == name) {
                let near = closest(name, self.args.iter().map(|a| a.name.as_str()));
                return invalid(format!("{} has no arg {name}", self.name), format!("args: {}", near.join(", ")));
            }
        }
        for arg in &self.args {
            match args.get(&arg.name) {
                None if arg.required => {
                    return invalid(
                        format!("{} needs --{}", self.name, arg.name),
                        format!("{} is {}", arg.name, arg.ty),
                    );
                }
                Some(value) if !matches_type(&arg.ty, value) => {
                    return invalid(format!("{}: {} must be {}", self.name, arg.name, arg.ty), format!("got {value}"));
                }
                _ => {}
            }
        }
        Ok(())
    }
}

fn matches_type(ty: &str, value: &Value) -> bool {
    if let Some(inner) = ty.strip_prefix("optional<").and_then(|t| t.strip_suffix('>')) {
        return value.is_null() || matches_type(inner, value);
    }
    if let Some(inner) = ty.strip_prefix("list<").and_then(|t| t.strip_suffix('>')) {
        return value.as_array().is_some_and(|items| items.iter().all(|v| matches_type(inner, v)));
    }
    match ty {
        "bool" => value.is_boolean(),
        "i64" => value.is_i64(),
        "u32" => value.as_u64().is_some_and(|n| u32::try_from(n).is_ok()),
        "f64" => value.is_number(),
        "string" => value.is_string(),
        "id" => value.as_str().is_some_and(|s| !s.is_empty()),
        _ => !value.is_null(),
    }
}
