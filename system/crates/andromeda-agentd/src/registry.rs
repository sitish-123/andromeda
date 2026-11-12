use std::collections::{BTreeMap, HashMap};
use std::sync::{Mutex, MutexGuard, PoisonError};

use serde_json::{Map, Value, json};
use tokio::sync::{mpsc, oneshot, watch};

use crate::idl::ActionEntry;
use crate::proto::{Code, closest, err, failed, ok};

/// Registry, router table and state cache of all providers.
pub struct Shared {
    inner: Mutex<Inner>,
    changes: watch::Sender<u64>,
}

#[derive(Default)]
struct Inner {
    providers: BTreeMap<String, Provider>,
    next_link: u64,
    next_call: u64,
}

/// The action list stays after a disconnect, so calls to it answer `unavailable` instead of `not_found`.
struct Provider {
    actions: Vec<ActionEntry>,
    link: Link,
}

enum Link {
    Offline,
    Online(Online),
}

struct Online {
    id: u64,
    manifest_hash: String,
    tx: mpsc::UnboundedSender<String>,
    pending: HashMap<String, oneshot::Sender<Value>>,
    cache: Cache,
    _close: oneshot::Sender<()>,
}

enum Cache {
    AwaitingSnapshot,
    Synced { seq: u64, state: Map<String, Value> },
}

/// One provider connection; a newer hello from the same provider makes it stale.
pub struct LinkId {
    provider: String,
    id: u64,
}

pub struct Call {
    pub provider: String,
    id: String,
    link: u64,
    pub reply: oneshot::Receiver<Value>,
}

pub enum Seq {
    InOrder,
    Gap,
}

pub struct Hello {
    pub provider: String,
    pub manifest_hash: String,
    pub actions: Vec<ActionEntry>,
}

impl Inner {
    fn online(&mut self, link: &LinkId) -> Option<&mut Online> {
        match self.providers.get_mut(&link.provider).map(|p| &mut p.link) {
            Some(Link::Online(online)) if online.id == link.id => Some(online),
            _ => None,
        }
    }

    fn action_names(&self) -> impl Iterator<Item = &str> {
        self.providers.values().flat_map(|p| p.actions.iter().map(|a| a.name.as_str()))
    }
}

impl Shared {
    pub fn new() -> Self {
        Self { inner: Mutex::default(), changes: watch::Sender::new(0) }
    }

    pub fn subscribe(&self) -> watch::Receiver<u64> {
        self.changes.subscribe()
    }

    fn lock(&self) -> MutexGuard<'_, Inner> {
        self.inner.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn changed(&self) {
        self.changes.send_modify(|n| *n += 1);
    }

    pub fn register(&self, hello: Hello, tx: mpsc::UnboundedSender<String>, close: oneshot::Sender<()>) -> LinkId {
        let mut inner = self.lock();
        inner.next_link += 1;
        let id = inner.next_link;
        let online = Online {
            id,
            manifest_hash: hello.manifest_hash,
            tx,
            pending: HashMap::new(),
            cache: Cache::AwaitingSnapshot,
            _close: close,
        };
        inner.providers.insert(hello.provider.clone(), Provider { actions: hello.actions, link: Link::Online(online) });
        drop(inner);
        self.changed();
        LinkId { provider: hello.provider, id }
    }

    pub fn snapshot(&self, link: &LinkId, seq: u64, state: Map<String, Value>) {
        if let Some(online) = self.lock().online(link) {
            online.cache = Cache::Synced { seq, state };
        }
        self.changed();
    }

    /// Applies an event as a JSON merge patch (RFC 7386). Anything but the next seq takes the provider offline.
    pub fn event(&self, link: &LinkId, seq: u64, delta: &Value) -> Seq {
        let mut inner = self.lock();
        let Some(online) = inner.online(link) else { return Seq::InOrder };
        let applied = match (&mut online.cache, delta) {
            (Cache::Synced { seq: last, state }, Value::Object(patch)) if seq == *last + 1 => {
                *last = seq;
                merge_object(state, patch);
                true
            }
            _ => false,
        };
        if !applied {
            set_offline(&mut inner, link);
        }
        drop(inner);
        self.changed();
        if applied { Seq::InOrder } else { Seq::Gap }
    }

    pub fn complete(&self, link: &LinkId, result: Value) {
        let mut inner = self.lock();
        let Some(online) = inner.online(link) else { return };
        let Some(waiter) = result.get("id").and_then(Value::as_str).and_then(|id| online.pending.remove(id)) else {
            log::debug!("{}: result for no pending call", link.provider);
            return;
        };
        let _ = waiter.send(result);
    }

    pub fn offline(&self, link: &LinkId) {
        set_offline(&mut self.lock(), link);
        self.changed();
    }

    pub fn route(&self, action: &str, args: &Map<String, Value>) -> Result<Call, Value> {
        let mut inner = self.lock();
        let owner = inner.providers.iter().find_map(|(name, provider)| {
            provider.actions.iter().find(|a| a.name == action).map(|entry| (name.clone(), entry.check_args(args)))
        });
        let Some((name, checked)) = owner else {
            let near = closest(action, inner.action_names());
            let example =
                near.first().map_or_else(|| "andromeda help --json".to_owned(), |a| format!("andromeda act {a}"));
            return Err(err(
                Code::NotFound,
                &format!("no action named {action}"),
                &format!("closest: {}", near.join(", ")),
                &example,
            ));
        };
        checked.map_err(failed)?;
        inner.next_call += 1;
        let id = format!("a{}", inner.next_call);
        let Some(Link::Online(online)) = inner.providers.get_mut(&name).map(|p| &mut p.link) else {
            return Err(err(
                Code::Unavailable,
                &format!("provider {name} is not connected"),
                &format!("{action} works again when {name} restarts and connects"),
                &format!("andromeda act {action}"),
            ));
        };
        let line = json!({ "op": "call", "id": id, "action": action, "args": args, "actor": "cli" });
        let (waiter, reply) = oneshot::channel();
        online.pending.insert(id.clone(), waiter);
        let _ = online.tx.send(format!("{line}\n"));
        Ok(Call { provider: name, id, link: online.id, reply })
    }

    pub fn cancel(&self, call: &Call) {
        let link = LinkId { provider: call.provider.clone(), id: call.link };
        if let Some(online) = self.lock().online(&link) {
            online.pending.remove(&call.id);
        }
    }

    /// Answers from the cache only. Path segments name object keys, or entity ids inside a list.
    pub fn state(&self, path: &str) -> Value {
        let inner = self.lock();
        let mut synced = Vec::new();
        let mut unsynced = Vec::new();
        for (name, provider) in &inner.providers {
            match &provider.link {
                Link::Online(Online { cache: Cache::Synced { state, .. }, .. }) => synced.push(state),
                _ => unsynced.push(name.as_str()),
            }
        }
        let mut segments = path.split('.').filter(|s| !s.is_empty());
        let Some(first) = segments.next() else {
            let mut all = Map::new();
            for state in synced.iter().rev() {
                all.extend(state.iter().map(|(k, v)| (k.clone(), v.clone())));
            }
            return ok(Value::Object(all));
        };
        let Some(mut node) = synced.iter().find_map(|s| s.get(first)) else {
            if !unsynced.is_empty() {
                return err(
                    Code::Unavailable,
                    &format!("no state for {first} while {} is not connected", unsynced.join(", ")),
                    "the state comes back when the provider connects and sends a snapshot",
                    &format!("andromeda state {path}"),
                );
            }
            return missing(path, first, synced.iter().flat_map(|s| s.keys().map(String::as_str)));
        };
        for segment in segments {
            let next = match node {
                Value::Object(map) => {
                    map.get(segment).ok_or_else(|| missing(path, segment, map.keys().map(String::as_str)))
                }
                Value::Array(list) => {
                    list.iter().find(|e| e.get("id").and_then(Value::as_str) == Some(segment)).ok_or_else(|| {
                        missing(path, segment, list.iter().filter_map(|e| e.get("id").and_then(Value::as_str)))
                    })
                }
                _ => Err(missing(path, segment, [])),
            };
            match next {
                Ok(n) => node = n,
                Err(e) => return e,
            }
        }
        ok(node.clone())
    }

    pub fn help(&self) -> Value {
        let inner = self.lock();
        let mut providers = Vec::new();
        let mut actions = Vec::new();
        for (name, provider) in &inner.providers {
            let (online, hash) = match &provider.link {
                Link::Online(o) => (true, Value::from(o.manifest_hash.as_str())),
                Link::Offline => (false, Value::Null),
            };
            providers.push(json!({ "name": name, "online": online, "manifest_hash": hash }));
            for action in &provider.actions {
                let mut entry = action.raw.clone();
                if let Value::Object(fields) = &mut entry {
                    fields.insert("provider".into(), name.as_str().into());
                    fields.insert("available".into(), online.into());
                }
                actions.push(entry);
            }
        }
        ok(json!({ "providers": providers, "actions": actions }))
    }
}

fn set_offline(inner: &mut Inner, link: &LinkId) {
    if inner.online(link).is_some()
        && let Some(provider) = inner.providers.get_mut(&link.provider)
    {
        provider.link = Link::Offline;
    }
}

fn missing<'a>(path: &str, segment: &str, candidates: impl IntoIterator<Item = &'a str>) -> Value {
    err(
        Code::NotFound,
        &format!("{path}: nothing named {segment}"),
        &format!("closest: {}", closest(segment, candidates).join(", ")),
        "andromeda state",
    )
}

fn merge_object(target: &mut Map<String, Value>, patch: &Map<String, Value>) {
    for (key, value) in patch {
        match (target.get_mut(key), value) {
            (_, Value::Null) => {
                target.remove(key);
            }
            (Some(Value::Object(inner)), Value::Object(sub)) => merge_object(inner, sub),
            (_, Value::Object(sub)) => {
                let mut fresh = Map::new();
                merge_object(&mut fresh, sub);
                target.insert(key.clone(), Value::Object(fresh));
            }
            _ => {
                target.insert(key.clone(), value.clone());
            }
        }
    }
}
