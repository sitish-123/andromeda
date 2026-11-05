//! Confines the helper after it opened its one file: resource limits, Landlock (read-only system and font
//! paths, no TCP, no signals or abstract sockets outside the sandbox) and a seccomp allowlist.

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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Profile {
    /// PDF and text: CPU only.
    Document,
    /// Video: the calls GStreamer threads make and DRM ioctls on the VA display, which opens before the sandbox;
    /// no /dev/dri path is reachable.
    Media,
}

/// Read-only paths the decoders need after start: shared data, fonts and the font cache.
fn readable_paths() -> Vec<PathBuf> {
    let mut paths: Vec<PathBuf> = ["/usr", "/etc/fonts", "/var/cache/fontconfig"].iter().map(PathBuf::from).collect();
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

fn landlock(profile: Profile) -> Result<(), Error> {
    let abi = ABI::V6;
    // The VA display is open before the sandbox (media::Media::prepare), so no udev or sysfs access is needed.
    let devices: Vec<PathBuf> = if profile == Profile::Media { vec![PathBuf::from("/dev/urandom")] } else { vec![] };
    let status = Ruleset::default()
        .set_compatibility(CompatLevel::BestEffort)
        .handle_access(AccessFs::from_all(abi))
        .and_then(|r| r.handle_access(AccessNet::from_all(abi)))
        .and_then(|r| r.scope(Scope::from_all(abi)))
        .and_then(Ruleset::create)
        .and_then(|r| r.add_rules(path_beneath_rules(readable_paths(), AccessFs::from_read(abi))))
        .and_then(|r| r.add_rules(path_beneath_rules(devices, AccessFs::from_read(abi))))

        .and_then(landlock::RulesetCreated::restrict_self)
        .map_err(|e| err("landlock", e))?;
    if status.ruleset == RulesetStatus::NotEnforced {
        return Err(Error("landlock: not supported by this kernel".into()));
    }
    Ok(())
}

/// The only syscalls a decoder may make once its file is open: what `strace -f` showed for poppler, cairo,
/// fontconfig and the text path over real documents, plus the Rust and glibc runtime (memory, threads,
/// time, random, signals to itself, abort). Anything else returns EPERM. stat, access and readlink are left
/// out: Landlock does not check them, so they would reveal paths outside the sandbox. mprotect stays without
/// a W^X limit, because glibc and glib load gconv and GIO modules after the sandbox starts.
const ALLOWED: &[libc::c_long] = &[
    libc::SYS_read,
    libc::SYS_pread64,
    libc::SYS_readv,
    libc::SYS_write,
    libc::SYS_writev,
    libc::SYS_openat,
    libc::SYS_close,
    libc::SYS_lseek,
    libc::SYS_fstat,
    libc::SYS_getdents64,
    libc::SYS_memfd_create,
    libc::SYS_brk,
    libc::SYS_mmap,
    libc::SYS_mremap,
    libc::SYS_munmap,
    libc::SYS_mprotect,
    libc::SYS_madvise,
    libc::SYS_futex,
    libc::SYS_set_robust_list,
    libc::SYS_rseq,
    libc::SYS_sched_yield,
    libc::SYS_sched_getaffinity,
    libc::SYS_getpid,
    libc::SYS_gettid,
    libc::SYS_getuid,
    libc::SYS_geteuid,
    libc::SYS_getgid,
    libc::SYS_getegid,
    libc::SYS_clock_gettime,
    libc::SYS_clock_getres,
    libc::SYS_clock_nanosleep,
    libc::SYS_nanosleep,
    libc::SYS_getrandom,
    libc::SYS_rt_sigaction,
    libc::SYS_rt_sigprocmask,
    libc::SYS_rt_sigreturn,
    libc::SYS_sigaltstack,
    libc::SYS_restart_syscall,
    libc::SYS_recvmsg,
    libc::SYS_sendmsg,
    libc::SYS_exit,
    libc::SYS_exit_group,
];

/// Added for video: GStreamer's poll and wakeup fds, its threads' scheduling calls, and stat on plugin files
/// before it loads them (a path-existence leak accepted for media only). ioctl and socketpair get argument
/// rules in `seccomp`.
const MEDIA_EXTRA: &[libc::c_long] = &[
    libc::SYS_newfstatat,
    libc::SYS_statx,
    libc::SYS_poll,
    libc::SYS_ppoll,
    libc::SYS_eventfd2,
    libc::SYS_pipe2,
    libc::SYS_epoll_create1,
    libc::SYS_epoll_ctl,
    libc::SYS_epoll_wait,
    libc::SYS_epoll_pwait,
    libc::SYS_sched_get_priority_max,
    libc::SYS_sched_get_priority_min,
    libc::SYS_getrusage,
    libc::SYS_sysinfo,
    libc::SYS_uname,
    libc::SYS_dup,
    libc::SYS_dup3,
];

/// x32 syscall numbers have this bit set; they reach the same handlers, so they must not bypass the list.
const X32_SYSCALL_BIT: u32 = 0x4000_0000;

fn cond(arg: u8, op: SeccompCmpOp, value: u64) -> Result<SeccompRule, Error> {
    let c = SeccompCondition::new(arg, SeccompCmpArgLen::Qword, op, value).map_err(|e| err("seccomp", e))?;
    SeccompRule::new(vec![c]).map_err(|e| err("seccomp", e))
}

fn cond32(arg: u8, op: SeccompCmpOp, value: u64) -> Result<SeccompRule, Error> {
    let c = SeccompCondition::new(arg, SeccompCmpArgLen::Dword, op, value).map_err(|e| err("seccomp", e))?;
    SeccompRule::new(vec![c]).map_err(|e| err("seccomp", e))
}

fn compile(
    rules: BTreeMap<i64, Vec<SeccompRule>>,
    mismatch: SeccompAction,
    matched: SeccompAction,
) -> Result<BpfProgram, Error> {
    let arch = TargetArch::try_from(std::env::consts::ARCH).map_err(|e| err("seccomp arch", e))?;
    let filter = SeccompFilter::new(rules, mismatch, matched, arch).map_err(|e| err("seccomp", e))?;
    filter.try_into().map_err(|e: seccompiler::BackendError| err("seccomp", e))
}

fn errno(code: i32) -> SeccompAction {
    SeccompAction::Errno(code.cast_unsigned())
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

fn seccomp(profile: Profile) -> Result<(), Error> {
    let own_pid = u64::from(std::process::id());
    let extra: &[libc::c_long] = if profile == Profile::Media { MEDIA_EXTRA } else { &[] };
    let mut allowed: BTreeMap<i64, Vec<SeccompRule>> = ALLOWED.iter().chain(extra).map(|&nr| (nr, vec![])).collect();
    // Signals only to this process (abort() raises SIGABRT on itself).
    for nr in [libc::SYS_kill, libc::SYS_tgkill] {
        allowed.insert(nr, vec![cond(0, SeccompCmpOp::Eq, own_pid)?]);
    }
    // Threads only: a clone without CLONE_THREAD would be a new process or namespace.
    let thread = u64::from(libc::CLONE_THREAD.cast_unsigned());
    allowed.insert(libc::SYS_clone, vec![cond(0, SeccompCmpOp::MaskedEq(thread), thread)?]);
    // clone3 passes flags in memory, which seccomp cannot read; ENOSYS makes glibc fall back to clone. The
    // kernel keeps the first of equal-rank results from the newest filter, so the allowlist must allow clone3
    // for the older filter's ENOSYS to win over its EPERM.
    allowed.insert(libc::SYS_clone3, vec![]);
    let clone3 = BTreeMap::from([(libc::SYS_clone3, vec![])]);
    if profile == Profile::Media {
        // ioctls of type 'd' (DRM, on the VA display opened before the sandbox) and 'b' (dma-buf) only; no
        // terminal or socket ioctls (SIOCGIFCONF would list network interfaces).
        allowed.insert(
            libc::SYS_ioctl,
            vec![cond32(1, SeccompCmpOp::MaskedEq(0xff00), 0x6400)?, cond32(1, SeccompCmpOp::MaskedEq(0xff00), 0x6200)?],
        );
        // Scheduling calls on this process only (pid 0): never another process of the user.
        for nr in [libc::SYS_sched_setscheduler, libc::SYS_sched_getparam, libc::SYS_sched_getscheduler] {
            allowed.insert(nr, vec![cond32(0, SeccompCmpOp::Eq, 0)?]);
        }
        // GstPoll wakeups: a connected pair reaches nothing outside the process.
        allowed.insert(libc::SYS_socketpair, vec![cond32(0, SeccompCmpOp::Eq, u64::from(libc::AF_UNIX.cast_unsigned()))?]);
    }
    // fcntl without F_SETOWN or F_SETSIG: those would let the socket signal another process.
    allowed.insert(
        libc::SYS_fcntl,
        [libc::F_GETFD, libc::F_SETFD, libc::F_GETFL, libc::F_SETFL, libc::F_DUPFD_CLOEXEC, libc::F_ADD_SEALS, libc::F_GET_SEALS]
            .iter()
            .map(|&cmd| cond(1, SeccompCmpOp::Eq, u64::from(cmd.cast_unsigned())))
            .collect::<Result<_, _>>()?,
    );
    // prctl: thread names and anonymous memory names (glibc malloc) only.
    allowed.insert(
        libc::SYS_prctl,
        [libc::PR_SET_NAME, libc::PR_GET_NAME, libc::PR_SET_VMA]
            .iter()
            .map(|&op| cond(0, SeccompCmpOp::Eq, u64::from(op.cast_unsigned())))
            .collect::<Result<_, _>>()?,
    );

    for program in [
        x32_filter(),
        compile(clone3, SeccompAction::Allow, errno(libc::ENOSYS))?,
        compile(allowed, errno(libc::EPERM), SeccompAction::Allow)?,
    ] {
        seccompiler::apply_filter_all_threads(&program).map_err(|e| err("seccomp", e))?;
    }
    Ok(())
}

/// Applies every limit to the whole process. Call it after the file is open and before any decoding.
///
/// # Errors
/// Any layer that cannot be applied: the helper must then exit and decode nothing.
pub fn enter(profile: Profile) -> Result<(), Error> {
    limit(Resource::As, ADDRESS_SPACE)?;
    limit(Resource::Nofile, OPEN_FILES)?;
    limit(Resource::Core, 0)?;
    landlock(profile)?;
    seccomp(profile)
}
