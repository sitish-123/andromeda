use std::io;
use std::sync::Arc;
use std::time::Duration;

use serde_json::{Map, Value, json};
use tokio::io::{AsyncWriteExt, BufReader};
use tokio::net::{UnixListener, UnixStream};

use crate::idl::SCHEMA_VERSION;
use crate::proto::{Code, Line, MAX_LINE, err, error_object, failed, read_line};
use crate::registry::Shared;

pub const CALL_TIMEOUT: Duration = Duration::from_secs(10);

const OPS_HINT: &str =
    r#"send one JSON object per line: {"op":"act","action":…,"args":{…}}, {"op":"state","path":…} or {"op":"help"}"#;

pub async fn serve(shared: Arc<Shared>, listener: UnixListener) -> io::Result<()> {
    loop {
        let (stream, _) = listener.accept().await?;
        tokio::spawn(session(Arc::clone(&shared), stream));
    }
}

async fn session(shared: Arc<Shared>, stream: UnixStream) {
    let (reader, mut writer) = stream.into_split();
    let mut reader = BufReader::new(reader);
    let mut buf = Vec::new();
    loop {
        let reply = match read_line(&mut reader, &mut buf).await {
            Ok(Line::Complete) => handle(&shared, &buf).await,
            Ok(Line::TooLong) => invalid(&format!("request line is longer than {MAX_LINE} bytes")),
            Ok(Line::Eof) | Err(_) => return,
        };
        let mut line = reply.to_string();
        line.push('\n');
        if writer.write_all(line.as_bytes()).await.is_err() {
            return;
        }
    }
}

fn invalid(message: &str) -> Value {
    err(Code::InvalidArgument, message, OPS_HINT, "andromeda help --json")
}

async fn handle(shared: &Shared, line: &[u8]) -> Value {
    let request: Value = match serde_json::from_slice(line) {
        Ok(request) => request,
        Err(e) => return invalid(&format!("request is not JSON: {e}")),
    };
    match request.get("op").and_then(Value::as_str) {
        Some("act") => act(shared, &request).await,
        Some("state") => match request.get("path") {
            None | Some(Value::Null) => shared.state(""),
            Some(Value::String(path)) => shared.state(path),
            Some(_) => invalid("path must be a string"),
        },
        Some("help") => shared.help(),
        _ => invalid("op must be act, state or help"),
    }
}

async fn act(shared: &Shared, request: &Value) -> Value {
    let Some(action) = request.get("action").and_then(Value::as_str) else {
        return invalid("act needs an action name");
    };
    let empty = Map::new();
    let args = match request.get("args") {
        None | Some(Value::Null) => &empty,
        Some(Value::Object(args)) => args,
        Some(_) => return invalid("args must be an object"),
    };
    let mut call = match shared.route(action, args) {
        Ok(call) => call,
        Err(reply) => return reply,
    };
    let unavailable = |message: &str| {
        err(Code::Unavailable, message, "retry when the provider is back", &format!("andromeda act {action}"))
    };
    match tokio::time::timeout(CALL_TIMEOUT, &mut call.reply).await {
        Ok(Ok(result)) => reply_from(result),
        Ok(Err(_)) => unavailable(&format!("provider {} closed during the call", call.provider)),
        Err(_) => {
            shared.cancel(&call);
            unavailable(&format!("provider {} sent no result in {} s", call.provider, CALL_TIMEOUT.as_secs()))
        }
    }
}

fn reply_from(result: Value) -> Value {
    let mut fields = match result {
        Value::Object(fields) => fields,
        _ => Map::new(),
    };
    if fields.get("ok") == Some(&Value::Bool(true)) {
        let value = fields.remove("value").unwrap_or(Value::Null);
        let changed = fields.remove("changed").unwrap_or_else(|| json!({}));
        return json!({ "ok": true, "value": value, "changed": changed, "schema_version": SCHEMA_VERSION });
    }
    match fields.remove("error") {
        Some(error @ Value::Object(_)) => failed(error),
        _ => failed(error_object(Code::Unavailable, "the provider failed without an error object", "", "")),
    }
}
