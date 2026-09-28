//! The filesystem at `~/Phone`: one folder per connected phone, and under it the phone's roots, read-only. Listings
//! and attributes are kept for [`FRESH`]; reads go through 1 MiB blocks, so sequential reads fetch ahead.

use std::collections::{HashMap, VecDeque};
use std::ffi::OsStr;
use std::sync::{Arc, Mutex, PoisonError};
use std::time::{Duration, Instant, UNIX_EPOCH};

use fuser::{
    Errno, FileAttr, FileHandle, FileType, Filesystem, Generation, INodeNo, LockOwner, OpenFlags, ReplyAttr, ReplyData,
    ReplyDirectory, ReplyEntry, Request,
};
use tokio::runtime::Handle;

use crate::link::Link;

/// How long a listing, and the attributes it gave, stand before they are asked for again.
const FRESH: Duration = Duration::from_secs(5);
const BLOCK: u32 = 1024 * 1024;
/// Blocks kept across all files: 32 MiB.
const BLOCKS: usize = 32;
const ROOT: INodeNo = INodeNo(1);

#[derive(Clone)]
struct Node {
    /// The phone this node is on; None for the mount's root.
    device: Option<String>,
    /// The browse path on that phone: `/` for its roots.
    path: String,
    dir: bool,
    size: u64,
    mtime: u64,
}

#[derive(Default)]
struct State {
    nodes: HashMap<u64, Node>,
    by_path: HashMap<(Option<String>, String), u64>,
    next: u64,
    /// A folder's children by name, and when they were listed.
    listed: HashMap<u64, (Instant, Vec<(String, u64)>)>,
    blocks: HashMap<(u64, u64), Arc<Vec<u8>>>,
    block_order: VecDeque<(u64, u64)>,
}

impl State {
    /// The inode for a node, the same one each time the path is seen, with the node's latest attributes.
    fn inode(&mut self, node: Node) -> u64 {
        let key = (node.device.clone(), node.path.clone());
        let ino = *self.by_path.entry(key).or_insert_with(|| {
            self.next += 1;
            self.next
        });
        if let Some(old) = self.nodes.get(&ino)
            && (old.size != node.size || old.mtime != node.mtime)
        {
            self.blocks.retain(|(file, _), _| *file != ino);
        }
        self.nodes.insert(ino, node);
        ino
    }

    fn keep_block(&mut self, key: (u64, u64), block: Arc<Vec<u8>>) {
        if self.blocks.insert(key, block).is_none() {
            self.block_order.push_back(key);
        }
        while self.block_order.len() > BLOCKS {
            if let Some(old) = self.block_order.pop_front() {
                self.blocks.remove(&old);
            }
        }
    }
}

pub struct PhoneFs {
    link: Link,
    runtime: Handle,
    uid: u32,
    gid: u32,
    state: Mutex<State>,
}

impl PhoneFs {
    pub fn new(link: Link, runtime: Handle, uid: u32, gid: u32) -> Self {
        let mut state = State { next: ROOT.0, ..State::default() };
        let root = Node { device: None, path: "/".to_owned(), dir: true, size: 0, mtime: 0 };
        state.nodes.insert(ROOT.0, root.clone());
        state.by_path.insert((None, root.path), ROOT.0);
        Self { link, runtime, uid, gid, state: Mutex::new(state) }
    }

    fn state(&self) -> std::sync::MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn node(&self, ino: INodeNo) -> Option<Node> {
        self.state().nodes.get(&ino.0).cloned()
    }

    /// A folder's children, from its last listing while fresh, else asked for now.
    fn children(&self, ino: INodeNo) -> Result<Vec<(String, u64)>, Errno> {
        if let Some((at, children)) = self.state().listed.get(&ino.0)
            && at.elapsed() < FRESH
        {
            return Ok(children.clone());
        }
        let node = self.node(ino).ok_or(Errno::ENOENT)?;
        if !node.dir {
            return Err(Errno::ENOTDIR);
        }
        let listed: Vec<(String, Node)> = match &node.device {
            None => self
                .runtime
                .block_on(self.link.devices())?
                .into_iter()
                .filter(|device| device.connected)
                .map(|device| {
                    let root = Node { device: Some(device.id), path: "/".to_owned(), dir: true, size: 0, mtime: 0 };
                    (device.name.replace('/', "∕"), root)
                })
                .collect(),
            Some(device) => self
                .runtime
                .block_on(self.link.list(device, &node.path))?
                .into_iter()
                .map(|entry| {
                    let child = Node {
                        device: Some(device.clone()),
                        path: join(&node.path, &entry.name),
                        dir: entry.dir,
                        size: entry.size,
                        mtime: entry.mtime,
                    };
                    (entry.name, child)
                })
                .collect(),
        };
        let mut state = self.state();
        let children: Vec<(String, u64)> = listed.into_iter().map(|(name, child)| (name, state.inode(child))).collect();
        let children = unique(children);
        state.listed.insert(ino.0, (Instant::now(), children.clone()));
        Ok(children)
    }

    fn attr(&self, ino: u64, node: &Node) -> FileAttr {
        let mtime = UNIX_EPOCH + Duration::from_secs(node.mtime);
        FileAttr {
            ino: INodeNo(ino),
            size: node.size,
            blocks: node.size.div_ceil(512),
            atime: mtime,
            mtime,
            ctime: mtime,
            crtime: mtime,
            kind: if node.dir { FileType::Directory } else { FileType::RegularFile },
            perm: if node.dir { 0o555 } else { 0o444 },
            nlink: if node.dir { 2 } else { 1 },
            uid: self.uid,
            gid: self.gid,
            rdev: 0,
            blksize: BLOCK,
            flags: 0,
        }
    }

    fn block(&self, ino: u64, node: &Node, index: u64) -> Result<Arc<Vec<u8>>, Errno> {
        if let Some(block) = self.state().blocks.get(&(ino, index)) {
            return Ok(block.clone());
        }
        let device = node.device.as_deref().ok_or(Errno::EISDIR)?;
        let bytes = self.runtime.block_on(self.link.read(device, &node.path, index * u64::from(BLOCK), BLOCK))?;
        let block = Arc::new(bytes);
        self.state().keep_block((ino, index), block.clone());
        Ok(block)
    }
}

/// Two phones with one name get the second one's id after it, so neither hides the other.
fn unique(children: Vec<(String, u64)>) -> Vec<(String, u64)> {
    let mut seen = HashMap::new();
    children
        .into_iter()
        .map(|(name, ino)| {
            let count = seen.entry(name.clone()).or_insert(0);
            *count += 1;
            if *count == 1 { (name, ino) } else { (format!("{name} ({count})"), ino) }
        })
        .collect()
}

fn join(dir: &str, name: &str) -> String {
    if dir == "/" { format!("/{name}") } else { format!("{dir}/{name}") }
}

impl Filesystem for PhoneFs {
    fn lookup(&self, _req: &Request, parent: INodeNo, name: &OsStr, reply: ReplyEntry) {
        let found = self.children(parent).and_then(|children| {
            let name = name.to_str().ok_or(Errno::ENOENT)?;
            children.iter().find(|(child, _)| child == name).map(|(_, ino)| *ino).ok_or(Errno::ENOENT)
        });
        match found.and_then(|ino| self.node(INodeNo(ino)).map(|node| (ino, node)).ok_or(Errno::ENOENT)) {
            Ok((ino, node)) => reply.entry(&FRESH, &self.attr(ino, &node), Generation(0)),
            Err(errno) => reply.error(errno),
        }
    }

    fn getattr(&self, _req: &Request, ino: INodeNo, _fh: Option<FileHandle>, reply: ReplyAttr) {
        match self.node(ino) {
            Some(node) => reply.attr(&FRESH, &self.attr(ino.0, &node)),
            None => reply.error(Errno::ENOENT),
        }
    }

    fn readdir(&self, _req: &Request, ino: INodeNo, _fh: FileHandle, offset: u64, mut reply: ReplyDirectory) {
        let children = match self.children(ino) {
            Ok(children) => children,
            Err(errno) => return reply.error(errno),
        };
        let kinds: Vec<(u64, FileType, String)> = {
            let state = self.state();
            let kind = |ino: u64| {
                if state.nodes.get(&ino).is_some_and(|node| node.dir) {
                    FileType::Directory
                } else {
                    FileType::RegularFile
                }
            };
            [(ino.0, FileType::Directory, ".".to_owned()), (ROOT.0, FileType::Directory, "..".to_owned())]
                .into_iter()
                .chain(children.into_iter().map(|(name, child)| (child, kind(child), name)))
                .collect()
        };
        for (index, (child, kind, name)) in
            kinds.into_iter().enumerate().skip(usize::try_from(offset).unwrap_or(usize::MAX))
        {
            if reply.add(INodeNo(child), index as u64 + 1, kind, name) {
                break;
            }
        }
        reply.ok();
    }

    fn read(
        &self,
        _req: &Request,
        ino: INodeNo,
        _fh: FileHandle,
        offset: u64,
        size: u32,
        _flags: OpenFlags,
        _lock_owner: Option<LockOwner>,
        reply: ReplyData,
    ) {
        let Some(node) = self.node(ino).filter(|node| !node.dir) else { return reply.error(Errno::EISDIR) };
        let end = (offset + u64::from(size)).min(node.size);
        let mut out = Vec::with_capacity(usize::try_from(end.saturating_sub(offset)).unwrap_or(0));
        let mut at = offset;
        while at < end {
            let index = at / u64::from(BLOCK);
            let block = match self.block(ino.0, &node, index) {
                Ok(block) => block,
                Err(errno) => return reply.error(errno),
            };
            let start = usize::try_from(at - index * u64::from(BLOCK)).unwrap_or(usize::MAX);
            let stop =
                usize::try_from((end - index * u64::from(BLOCK)).min(u64::from(BLOCK))).unwrap_or(0).min(block.len());
            if start >= stop {
                break;
            }
            out.extend_from_slice(&block[start..stop]);
            at += (stop - start) as u64;
        }
        reply.data(&out);
    }
}
