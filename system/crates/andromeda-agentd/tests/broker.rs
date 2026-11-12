#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::future::Future;
use std::path::{Path, PathBuf};
use std::time::Duration;

use andromeda_agentd::{Broker, MAX_LINE};
use serde_json::{Value, json};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::unix::{OwnedReadHalf, OwnedWriteHalf};
use tokio::net::{UnixListener, UnixStream};
use tokio::sync::watch;

const WAIT: Duration = Duration::from_secs(5);

async fn within<T>(what: &str, f: impl Future<Output = T>) -> T {
    tokio::time::timeout(WAIT, f).await.unwrap_or_else(|_| panic!("timed out: {what}"))
}

struct Lines {
    reader: BufReader<OwnedReadHalf>,
    writer: OwnedWriteHalf,
}

impl Lines {
    fn new(stream: UnixStream) -> Self {
        let (reader, writer) = stream.into_split();
        Self { reader: BufReader::new(reader), writer }
    }

    async fn send_raw(&mut self, bytes: &[u8]) {
        self.writer.write_all(bytes).await.unwrap();
    }

    async fn send(&mut self, value: &Value) {
        self.send_raw(format!("{value}\n").as_bytes()).await;
    }

    async fn recv(&mut self) -> Value {
        let mut line = String::new();
        within("a line", self.reader.read_line(&mut line)).await.unwrap();
        serde_json::from_str(&line).unwrap_or_else(|e| panic!("bad line {line:?}: {e}"))
    }

    async fn request(&mut self, value: &Value) -> Value {
        self.send(value).await;
        self.recv().await
    }
}

struct Env {
    dir: tempfile::TempDir,
    changes: watch::Receiver<u64>,
}

impl Env {
    fn start() -> Self {
        let dir = tempfile::tempdir().unwrap();
        let listener = UnixListener::bind(dir.path().join("agentd.sock")).unwrap();
        let broker = Broker::default();
        let changes = broker.changes();
        tokio::spawn(broker.run(listener, dir.path().to_owned()));
        Self { dir, changes }
    }

    fn provider_path(&self) -> PathBuf {
        self.dir.path().join("andromeda-provider-test.sock")
    }

    async fn client(&self) -> Lines {
        Lines::new(UnixStream::connect(self.dir.path().join("agentd.sock")).await.unwrap())
    }

    /// Repeats `request` after each broker change until `done` holds.
    async fn until(&mut self, client: &mut Lines, request: Value, done: impl Fn(&Value) -> bool) -> Value {
        within(&format!("{request} to match"), async {
            loop {
                self.changes.borrow_and_update();
                let reply = client.request(&request).await;
                if done(&reply) {
                    return reply;
                }
                self.changes.changed().await.unwrap();
            }
        })
        .await
    }
}

async fn accept(listener: &UnixListener) -> Lines {
    Lines::new(within("broker connect", listener.accept()).await.unwrap().0)
}

fn hello(hash: &str) -> Value {
    json!({ "op": "hello", "provider": "test", "schema_version": 1, "manifest_hash": hash,
            "actions": [action("workspace.switch", &json!([{ "name": "index", "type": "u32", "required": true,
                                                             "description": "1-based position", "example": "2" }])),
                        action("workspace.list", &json!([])),
                        action("window.list", &json!([{ "name": "app", "type": "optional<string>", "required": false,
                                                       "description": "app id", "example": "foot" }]))] })
}

fn action(name: &str, args: &Value) -> Value {
    json!({ "name": name, "description": "test action", "args": args, "result": "string", "events": [],
            "permission": "windows", "reversible": true, "sensitive": false, "binds": name })
}

fn switch_request() -> Value {
    json!({ "op": "act", "action": "workspace.switch", "args": { "index": 2 } })
}

fn workspaces(n: u32) -> Value {
    (1..=n).map(|i| json!({ "id": format!("ws{i}"), "index": i, "focused": i == 1 })).collect()
}

fn snapshot(n: u32) -> Value {
    json!({ "op": "snapshot", "seq": 0, "state": { "workspaces": workspaces(n), "outputs": [] } })
}

fn ok(reply: &Value) -> bool {
    reply["ok"] == true
}

async fn connected(env: &mut Env, path: &Path, hash: &str) -> (UnixListener, Lines, Lines) {
    let listener = UnixListener::bind(path).unwrap();
    let mut provider = accept(&listener).await;
    provider.send(&hello(hash)).await;
    provider.send(&snapshot(2)).await;
    let mut client = env.client().await;
    env.until(&mut client, json!({ "op": "state", "path": "workspaces" }), ok).await;
    (listener, provider, client)
}

async fn switch(client: &mut Lines, provider: &mut Lines) -> Value {
    let request = json!({ "op": "act", "action": "workspace.switch", "args": { "index": 2 } });
    let (reply, ()) = tokio::join!(client.request(&request), async {
        let call = provider.recv().await;
        assert_eq!(call["op"], "call");
        assert_eq!(call["action"], "workspace.switch");
        assert_eq!(call["args"], json!({ "index": 2 }));
        assert_eq!(call["actor"], "cli");
        provider
            .send(&json!({ "op": "result", "id": call["id"], "ok": true, "value": { "index": 2 },
                           "changed": { "workspaces": [{ "id": "ws2", "focused": true }] } }))
            .await;
    });
    reply
}

#[tokio::test]
async fn snapshot_answers_state_queries() {
    let mut env = Env::start();
    let path = env.provider_path();
    let (_listener, _provider, mut client) = connected(&mut env, &path, "h1").await;
    let all = client.request(&json!({ "op": "state", "path": "workspaces" })).await;
    assert_eq!(all, json!({ "ok": true, "value": workspaces(2), "schema_version": 1 }));
    let one = client.request(&json!({ "op": "state", "path": "workspaces.ws2" })).await;
    assert_eq!(one["value"]["index"], 2);
    let missing = client.request(&json!({ "op": "state", "path": "workspaces.ws9" })).await;
    assert_eq!(missing["error"]["code"], "not_found");
    assert!(missing["error"]["hint"].as_str().unwrap().contains("ws1"), "{missing}");
}

#[tokio::test]
async fn act_is_routed_and_answers_the_changed_slice() {
    let mut env = Env::start();
    let path = env.provider_path();
    let (_listener, mut provider, mut client) = connected(&mut env, &path, "h1").await;
    let reply = switch(&mut client, &mut provider).await;
    assert_eq!(
        reply,
        json!({ "ok": true, "value": { "index": 2 }, "changed": { "workspaces": [{ "id": "ws2", "focused": true }] },
                "schema_version": 1 })
    );
}

#[tokio::test]
async fn unknown_action_suggests_the_closest_names() {
    let mut env = Env::start();
    let path = env.provider_path();
    let (_listener, _provider, mut client) = connected(&mut env, &path, "h1").await;
    let reply = client.request(&json!({ "op": "act", "action": "workspace.swich" })).await;
    assert_eq!(reply["schema_version"], 1);
    assert_eq!(reply["error"]["code"], "not_found");
    assert!(reply["error"]["hint"].as_str().unwrap().starts_with("closest: workspace.switch"), "{reply}");
    assert_eq!(reply["error"]["example"], "andromeda act workspace.switch");
}

#[tokio::test]
async fn args_are_checked_against_the_hello_entry() {
    let mut env = Env::start();
    let path = env.provider_path();
    let (_listener, mut provider, mut client) = connected(&mut env, &path, "h1").await;
    for args in [json!({}), json!({ "index": "two" }), json!({ "index": -1 }), json!({ "index": 2, "speed": 1 })] {
        let reply = client.request(&json!({ "op": "act", "action": "workspace.switch", "args": args })).await;
        assert_eq!(reply["error"]["code"], "invalid_argument", "{args}: {reply}");
        assert_eq!(reply["error"]["example"], "andromeda act workspace.switch --index 2", "{reply}");
    }
    let help = client.request(&json!({ "op": "help" })).await;
    let switch_entry = &help["value"]["actions"][0];
    assert_eq!(switch_entry["args"][0]["type"], "u32", "{help}");
    assert_eq!(switch_entry["permission"], "windows", "{help}");
    assert!(ok(&switch(&mut client, &mut provider).await));
}

#[tokio::test]
async fn provider_restart_is_unavailable_until_the_next_hello() {
    let mut env = Env::start();
    let path = env.provider_path();
    let (listener, mut provider, mut client) = connected(&mut env, &path, "h1").await;

    let request: Value = switch_request();
    let (reply, ()) = tokio::join!(client.request(&request), async move {
        provider.recv().await;
    });
    assert_eq!(reply["error"]["code"], "unavailable", "{reply}");
    drop(listener);
    std::fs::remove_file(&path).unwrap();

    let reply = client.request(&switch_request()).await;
    assert_eq!(reply["error"]["code"], "unavailable", "{reply}");
    assert!(reply["error"]["hint"].as_str().unwrap().contains("test"), "{reply}");
    let state = client.request(&json!({ "op": "state", "path": "workspaces" })).await;
    assert_eq!(state["error"]["code"], "unavailable", "{state}");

    let (_listener, mut provider, mut client) = connected(&mut env, &path, "h2").await;
    let help = client.request(&json!({ "op": "help" })).await;
    assert_eq!(help["value"]["providers"][0]["manifest_hash"], "h2");
    assert!(ok(&switch(&mut client, &mut provider).await));
}

#[tokio::test]
async fn seq_gap_reads_a_new_snapshot() {
    let mut env = Env::start();
    let path = env.provider_path();
    let (listener, mut provider, mut client) = connected(&mut env, &path, "h1").await;
    let count = |n: usize| move |r: &Value| r["value"].as_array().is_some_and(|a| a.len() == n);

    provider.send(&json!({ "op": "event", "seq": 1, "name": "workspace.changed", "delta": { "workspaces": workspaces(3) }, "cause": null })).await;
    env.until(&mut client, json!({ "op": "state", "path": "workspaces" }), count(3)).await;

    provider.send(&json!({ "op": "event", "seq": 3, "name": "workspace.changed", "delta": { "workspaces": workspaces(9) }, "cause": null })).await;
    let mut again = accept(&listener).await;
    again.send(&hello("h1")).await;
    again.send(&snapshot(4)).await;
    env.until(&mut client, json!({ "op": "state", "path": "workspaces" }), count(4)).await;
}

#[tokio::test]
async fn oversize_and_malformed_lines_are_rejected() {
    let env = Env::start();
    let mut client = env.client().await;
    let big = format!("{}\n", json!({ "op": "help", "pad": "x".repeat(MAX_LINE) }));
    let Lines { reader, writer } = &mut client;
    let mut reply = String::new();
    let (read, ()) = tokio::join!(within("oversize reply", reader.read_line(&mut reply)), async {
        writer.write_all(big.as_bytes()).await.unwrap();
    });
    read.unwrap();
    assert!(reply.contains(r#""code":"invalid_argument""#), "{reply}");
    let reply = client.request(&json!("{nope")).await;
    assert_eq!(reply["error"]["code"], "invalid_argument", "{reply}");
    client.send_raw(b"{nope\n").await;
    assert_eq!(client.recv().await["error"]["code"], "invalid_argument");
    assert!(ok(&client.request(&json!({ "op": "help" })).await));
}
