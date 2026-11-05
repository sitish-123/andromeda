//! Confines the helper after it opened its one file: resource limits, Landlock (read-only system and font
//! paths, no TCP, no signals or abstract sockets outside the sandbox) and a seccomp deny list.

use landlock::{
    ABI, Access, AccessFs, AccessNet, CompatLevel, Compatible, Ruleset, RulesetAttr, RulesetCreatedAttr,
    RulesetStatus, Scope, path_beneath_rules,
};
use rustix::process::{Resource, Rlimit, setrlimit};
use seccompiler::{
    BpfProgram, SeccompAction, SeccompCmpArgLen, SeccompCmpOp, SeccompCondition, SeccompFilter, SeccompRule, TargetArch,
};
use std::collections::BTreeMap;
use std::path::PathBuf;

/// Address space cap: a 8192 x 16384 ARGB page is 512 MiB, plus poppler and cairo working memory.
const ADDRESS_SPACE: u64 = 2 << 30;
const OPEN_FILES: u64 = 64;

#[derive(Debug)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

fn err(what: &str, e: impl std::fmt::Display) -> Error {
    Error(format!("{what}: {e}"))
}

/// Read-only paths the decoders need after start: shared data, fonts and the font cache, GPU nodes (S7).
fn readable_paths() -> Vec<PathBuf> {
    let mut paths: Vec<PathBuf> =
        ["/usr", "/etc/fonts", "/var/cache/fontconfig", "/dev/dri"].iter().map(PathBuf::from).collect();
    if let Some(home) = std::env::var_os("HOME").map(PathBuf::from) {
        paths.push(home.join(".local/share/fonts"));
        paths.push(home.join(".fonts"));
        let cache = std::env::var_os("XDG_CACHE_HOME").map_or_else(|| home.join(".cache"), PathBuf::from);
        paths.push(cache.join("fontconfig"));
    }
    paths.retain(|p| p.exists());
    paths
}

fn limit(resource: Resource, value: u64) -> Result<(), Error> {
    setrlimit(resource, Rlimit { current: Some(value), maximum: Some(value) }).map_err(|e| err("setrlimit", e))
}

fn landlock() -> Result<(), Error> {
    let abi = ABI::V6;
    let status = Ruleset::default()
        .set_compatibility(CompatLevel::BestEffort)
        .handle_access(AccessFs::from_all(abi))
        .and_then(|r| r.handle_access(AccessNet::from_all(abi)))
        .and_then(|r| r.scope(Scope::from_all(abi)))
        .and_then(Ruleset::create)
        .and_then(|r| r.add_rules(path_beneath_rules(readable_paths(), AccessFs::from_read(abi))))
        .and_then(landlock::RulesetCreated::restrict_self)
        .map_err(|e| err("landlock", e))?;
    if status.ruleset == RulesetStatus::NotEnforced {
        return Err(Error("landlock: not supported by this kernel".into()));
    }
    Ok(())
}

/// Syscalls a decoder never needs once its file is open: new sockets, exec, other processes, namespaces,
/// mounts, kernel modules and keyrings. Each returns EPERM.
const DENIED: &[libc::c_long] = &[
    libc::SYS_socket,
    libc::SYS_socketpair,
    libc::SYS_connect,
    libc::SYS_bind,
    libc::SYS_listen,
    libc::SYS_accept,
    libc::SYS_accept4,
    libc::SYS_execve,
    libc::SYS_execveat,
    libc::SYS_ptrace,
    libc::SYS_process_vm_readv,
    libc::SYS_process_vm_writev,
    libc::SYS_pidfd_open,
    libc::SYS_pidfd_getfd,
    libc::SYS_pidfd_send_signal,
    libc::SYS_kcmp,
    libc::SYS_mount,
    libc::SYS_umount2,
    libc::SYS_fsopen,
    libc::SYS_fsconfig,
    libc::SYS_fsmount,
    libc::SYS_fspick,
    libc::SYS_open_tree,
    libc::SYS_move_mount,
    libc::SYS_mount_setattr,
    libc::SYS_pivot_root,
    libc::SYS_chroot,
    libc::SYS_unshare,
    libc::SYS_setns,
    libc::SYS_kexec_load,
    libc::SYS_init_module,
    libc::SYS_finit_module,
    libc::SYS_delete_module,
    libc::SYS_bpf,
    libc::SYS_perf_event_open,
    libc::SYS_keyctl,
    libc::SYS_add_key,
    libc::SYS_request_key,
    libc::SYS_userfaultfd,
    libc::SYS_io_uring_setup,
    libc::SYS_open_by_handle_at,
    libc::SYS_name_to_handle_at,
    libc::SYS_personality,
];

const CLONE_NAMESPACES: &[libc::c_int] = &[
    libc::CLONE_NEWNS,
    libc::CLONE_NEWUSER,
    libc::CLONE_NEWPID,
    libc::CLONE_NEWNET,
    libc::CLONE_NEWIPC,
    libc::CLONE_NEWUTS,
    libc::CLONE_NEWCGROUP,
    libc::CLONE_NEWTIME,
];

/// x32 syscall numbers have this bit set; they reach the same handlers, so they must not bypass the list.
const X32_SYSCALL_BIT: u32 = 0x4000_0000;

fn cond(arg: u8, op: SeccompCmpOp, value: u64) -> Result<SeccompRule, Error> {
    let c = SeccompCondition::new(arg, SeccompCmpArgLen::Qword, op, value).map_err(|e| err("seccomp", e))?;
    SeccompRule::new(vec![c]).map_err(|e| err("seccomp", e))
}

fn compile(rules: BTreeMap<i64, Vec<SeccompRule>>, errno: i32) -> Result<BpfProgram, Error> {
    let arch = TargetArch::try_from(std::env::consts::ARCH).map_err(|e| err("seccomp arch", e))?;
    let filter =
        SeccompFilter::new(rules, SeccompAction::Allow, SeccompAction::Errno(errno.cast_unsigned()), arch)
            .map_err(|e| err("seccomp", e))?;
    filter.try_into().map_err(|e: seccompiler::BackendError| err("seccomp", e))
}

/// `ld [nr]; jge X32 -> errno; allow`.
fn x32_filter() -> BpfProgram {
    const BPF_LD_W_ABS: u16 = 0x20;
    const BPF_JGE_K: u16 = 0x35;
    const BPF_RET_K: u16 = 0x06;
    const SECCOMP_RET_ALLOW: u32 = 0x7fff_0000;
    const SECCOMP_RET_ERRNO: u32 = 0x0005_0000;
    let insn = |code, jt, jf, k| seccompiler::sock_filter { code, jt, jf, k };
    vec![
        insn(BPF_LD_W_ABS, 0, 0, 0),
        insn(BPF_JGE_K, 0, 1, X32_SYSCALL_BIT),
        insn(BPF_RET_K, 0, 0, SECCOMP_RET_ERRNO | libc::EPERM.cast_unsigned()),
        insn(BPF_RET_K, 0, 0, SECCOMP_RET_ALLOW),
    ]
}

fn seccomp() -> Result<(), Error> {
    let own_pid = u64::from(std::process::id());
    let mut denied: BTreeMap<i64, Vec<SeccompRule>> = DENIED.iter().map(|&nr| (nr, vec![])).collect();
    // Signals only to this process (abort() raises SIGABRT on itself).
    denied.insert(libc::SYS_kill, vec![cond(0, SeccompCmpOp::Ne, own_pid)?]);
    denied.insert(libc::SYS_tkill, vec![cond(0, SeccompCmpOp::Ne, own_pid)?]);
    denied.insert(libc::SYS_tgkill, vec![cond(0, SeccompCmpOp::Ne, own_pid)?]);
    denied.insert(
        libc::SYS_clone,
        CLONE_NAMESPACES
            .iter()
            .map(|&flag| {
                let flag = u64::from(flag.cast_unsigned());
                cond(0, SeccompCmpOp::MaskedEq(flag), flag)
            })
            .collect::<Result<_, _>>()?,
    );
    // clone3 passes flags in memory, which seccomp cannot read; ENOSYS makes glibc fall back to clone.
    let clone3 = BTreeMap::from([(libc::SYS_clone3, vec![])]);

    for program in [x32_filter(), compile(clone3, libc::ENOSYS)?, compile(denied, libc::EPERM)?] {
        seccompiler::apply_filter_all_threads(&program).map_err(|e| err("seccomp", e))?;
    }
    Ok(())
}

/// Applies every limit to the whole process. Call it after the file is open and before any decoding.
///
/// # Errors
/// Any layer that cannot be applied: the helper must then exit and decode nothing.
pub fn enter() -> Result<(), Error> {
    limit(Resource::As, ADDRESS_SPACE)?;
    limit(Resource::Nofile, OPEN_FILES)?;
    limit(Resource::Core, 0)?;
    landlock()?;
    seccomp()
}
