//! One test per file: the sandbox confines the whole test process.

use std::io::Write;

#[allow(unsafe_code)]
fn raw_syscall(nr: libc::c_long, a: libc::c_long, b: libc::c_long) -> (libc::c_long, i32) {
    // SAFETY: probes syscalls the sandbox must refuse; a clone that succeeds exits its child at once.
    unsafe {
        let ret = libc::syscall(nr, a, b, 0, 0, 0);
        if ret == 0 && nr == libc::SYS_clone {
            libc::_exit(0);
        }
        (ret, *libc::__errno_location())
    }
}

#[test]
fn sandbox_confines_files_sockets_signals_and_namespaces() {
    let mut secret = tempfile::NamedTempFile::new_in(std::env::var("HOME").unwrap()).unwrap();
    secret.write_all(b"secret").unwrap();
    // Not reaped: the sandbox forbids killing it, and it ends on its own.
    #[allow(clippy::zombie_processes)]
    let neighbour = std::process::Command::new("sleep").arg("5").spawn().unwrap();
    let neighbour_pid = libc::c_long::from(neighbour.id().cast_signed());

    andromeda_preview_helper::sandbox::enter().unwrap();

    assert!(std::fs::read(secret.path()).is_err());
    assert!(std::fs::write("/tmp/andromeda-preview-helper-probe", b"x").is_err());
    assert!(std::fs::read_dir("/usr/share").is_ok());

    let socket = rustix::net::socket(rustix::net::AddressFamily::UNIX, rustix::net::SocketType::STREAM, None);
    assert_eq!(socket.unwrap_err(), rustix::io::Errno::PERM);
    // x32 numbering of socket(2) must hit the same wall, not ENOSYS.
    assert_eq!(raw_syscall(libc::SYS_socket | 0x4000_0000, libc::AF_UNIX.into(), libc::SOCK_STREAM.into()), (-1, libc::EPERM));
    assert_eq!(raw_syscall(libc::SYS_kill, neighbour_pid, libc::SIGKILL.into()), (-1, libc::EPERM));
    let new_user = libc::c_long::from(libc::CLONE_NEWUSER | libc::SIGCHLD);
    assert_eq!(raw_syscall(libc::SYS_clone, new_user, 0), (-1, libc::EPERM));

}
