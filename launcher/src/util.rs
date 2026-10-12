//! Small shared helpers: hashing, downloads with progress, /proc scanning, logging.
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::Duration;

static LOG_FILE: Mutex<Option<PathBuf>> = Mutex::new(None);

pub fn init_log(path: PathBuf) {
    if let Some(p) = path.parent() {
        let _ = fs::create_dir_all(p);
    }
    // keep one rotation
    if let Ok(md) = fs::metadata(&path) {
        if md.len() > 2_000_000 {
            let _ = fs::rename(&path, path.with_extension("log.old"));
        }
    }
    *LOG_FILE.lock().unwrap() = Some(path);
}

pub fn log(msg: &str) {
    let line = format!("[{}] {}\n", timestamp(), msg);
    eprint!("{line}");
    if let Some(p) = LOG_FILE.lock().unwrap().as_ref() {
        if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(p) {
            let _ = f.write_all(line.as_bytes());
        }
    }
}

pub fn timestamp() -> String {
    let (y, mo, d, h, mi, s) = local_time();
    format!("{y:04}-{mo:02}-{d:02} {h:02}:{mi:02}:{s:02}")
}

pub fn date_stamp() -> String {
    let (y, mo, d, ..) = local_time();
    format!("{y:04}-{mo:02}-{d:02}")
}

#[cfg(windows)]
fn local_time() -> (i32, u32, u32, u32, u32, u32) { crate::winsys::local_time() }

#[cfg(unix)]
fn local_time() -> (i32, u32, u32, u32, u32, u32) {
    unsafe {
        let t = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&t, &mut tm);
        (tm.tm_year + 1900, (tm.tm_mon + 1) as u32, tm.tm_mday as u32, tm.tm_hour as u32, tm.tm_min as u32, tm.tm_sec as u32)
    }
}

pub fn hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

pub fn sha256_file(path: &Path) -> std::io::Result<String> {
    let mut f = File::open(path)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(hex(&h.finalize()))
}

pub fn human_bytes(n: u64) -> String {
    let f = n as f64;
    if f >= 1e9 { format!("{:.1} GB", f / 1e9) } else if f >= 1e6 { format!("{:.1} MB", f / 1e6) } else if f >= 1e3 { format!("{:.0} KB", f / 1e3) } else { format!("{n} B") }
}

/// Numeric dotted-version comparison ("0.2.10" > "0.2.9"). Anything after '-' or '+' is ignored.
pub fn version_newer(candidate: &str, current: &str) -> bool {
    fn parts(v: &str) -> Vec<u64> {
        v.split(|c| c == '-' || c == '+').next().unwrap_or("").split('.').map(|p| p.trim().parse::<u64>().unwrap_or(0)).collect()
    }
    let (a, b) = (parts(candidate), parts(current));
    for i in 0..a.len().max(b.len()) {
        let (x, y) = (a.get(i).copied().unwrap_or(0), b.get(i).copied().unwrap_or(0));
        if x != y {
            return x > y;
        }
    }
    false
}

#[derive(Debug, Clone)]
pub enum NetError {
    /// No connection (DNS, connect, timeout).
    Offline(String),
    /// The server answered with an error status.
    Http(u16, String),
    /// A download did not match its published hash/size.
    Mismatch(String),
    Io(String),
}

impl std::fmt::Display for NetError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            NetError::Offline(s) => write!(f, "no connection: {s}"),
            NetError::Http(c, u) => write!(f, "HTTP {c} for {u}"),
            NetError::Mismatch(s) => write!(f, "{s}"),
            NetError::Io(s) => write!(f, "{s}"),
        }
    }
}

fn agent() -> ureq::Agent {
    let cfg = ureq::Agent::config_builder()
        .timeout_connect(Some(Duration::from_secs(15)))
        .timeout_resolve(Some(Duration::from_secs(15)))
        .timeout_recv_response(Some(Duration::from_secs(30)))
        .user_agent(concat!("portalcraft-launcher/", env!("CARGO_PKG_VERSION")))
        .build();
    cfg.into()
}

fn local_path(url: &str) -> Option<PathBuf> {
    if let Some(p) = url.strip_prefix("file://") {
        // file:///C:/dir/x on Windows
        let b = p.as_bytes();
        if b.len() > 2 && b[0] == b'/' && b[2] == b':' { return Some(PathBuf::from(&p[1..])); }
        return Some(PathBuf::from(p));
    }
    if url.starts_with('/') {
        return Some(PathBuf::from(url));
    }
    // C:\dir\x or C:/dir/x
    let b = url.as_bytes();
    if b.len() > 2 && b[0].is_ascii_alphabetic() && b[1] == b':' && (b[2] == b'\\' || b[2] == b'/') {
        return Some(PathBuf::from(url));
    }
    None
}

/// Fetches a small text document (the manifest). Accepts http(s)://, file:// and plain paths.
pub fn fetch_text(url: &str) -> Result<String, NetError> {
    if let Some(p) = local_path(url) {
        return fs::read_to_string(&p).map_err(|e| NetError::Io(format!("{}: {e}", p.display())));
    }
    let cfg = ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(60)))
        .user_agent(concat!("portalcraft-launcher/", env!("CARGO_PKG_VERSION")))
        .build();
    let mut resp = ureq::Agent::from(cfg).get(url).call().map_err(|e| map_ureq(e, url))?;
    resp.body_mut().with_config().limit(8 * 1024 * 1024).read_to_string().map_err(|e| map_ureq(e, url))
}

fn map_ureq(e: ureq::Error, url: &str) -> NetError {
    match e {
        ureq::Error::StatusCode(c) => NetError::Http(c, url.to_string()),
        ureq::Error::Io(io) => NetError::Offline(io.to_string()),
        other => NetError::Offline(other.to_string()),
    }
}

/// Downloads `url` to `dest` (via dest.part), checks sha256 (and size when non-zero), reports (done, total) bytes.
/// An already complete, matching `dest` is kept and not downloaded again. The .part file never outlives a failure
/// (a cut line or a full disk must not leave hundreds of MB behind).
pub fn download_verified(
    url: &str,
    dest: &Path,
    sha256: &str,
    size: u64,
    cancel: &std::sync::atomic::AtomicBool,
    progress: &mut dyn FnMut(u64, u64),
) -> Result<(), NetError> {
    if let Some(p) = dest.parent() {
        fs::create_dir_all(p).map_err(|e| NetError::Io(e.to_string()))?;
    }
    if dest.exists() && !sha256.is_empty() && sha256_file(dest).map(|h| h.eq_ignore_ascii_case(sha256)).unwrap_or(false) {
        progress(1, 1);
        return Ok(());
    }
    let part = PathBuf::from(format!("{}.part", dest.display()));
    let r = download_inner(url, dest, &part, sha256, size, cancel, progress);
    if r.is_err() { let _ = fs::remove_file(&part); }
    r
}

/// No byte for this long: the connection is dead (Wi-Fi dropped, cable pulled); give up instead of hanging forever.
const STALL_SECS: u64 = 60;

fn download_inner(
    url: &str,
    dest: &Path,
    part: &Path,
    sha256: &str,
    size: u64,
    cancel: &std::sync::atomic::AtomicBool,
    progress: &mut dyn FnMut(u64, u64),
) -> Result<(), NetError> {
    use std::sync::atomic::Ordering::Relaxed;
    let mut hasher = Sha256::new();
    let mut done = 0u64;
    let mut out = File::create(part).map_err(|e| NetError::Io(format!("{}: {e}", part.display())))?;
    let io_err = |e: std::io::Error| NetError::Io(e.to_string());
    if let Some(p) = local_path(url) {
        let mut src = File::open(&p).map_err(|e| NetError::Io(format!("{}: {e}", p.display())))?;
        let total = src.metadata().map(|m| m.len()).unwrap_or(0);
        let mut buf = vec![0u8; 1 << 20];
        loop {
            if cancel.load(Relaxed) { return Err(NetError::Io("cancelled".into())); }
            let n = src.read(&mut buf).map_err(io_err)?;
            if n == 0 { break; }
            hasher.update(&buf[..n]);
            out.write_all(&buf[..n]).map_err(io_err)?;
            done += n as u64;
            progress(done, total);
        }
    } else {
        let resp = agent().get(url).call().map_err(|e| map_ureq(e, url))?;
        let total = resp.body().content_length().unwrap_or(size);
        // the body is read on a helper thread so that Stop and the stall timeout work while a read is blocked
        let (tx, rx) = std::sync::mpsc::sync_channel::<Result<Vec<u8>, String>>(8);
        std::thread::spawn(move || {
            let mut body = resp.into_body();
            let mut reader = body.as_reader();
            let mut buf = vec![0u8; 256 * 1024];
            loop {
                match reader.read(&mut buf) {
                    Ok(0) => { let _ = tx.send(Ok(Vec::new())); return; }
                    Ok(n) => { if tx.send(Ok(buf[..n].to_vec())).is_err() { return; } }
                    Err(e) => { let _ = tx.send(Err(e.to_string())); return; }
                }
            }
        });
        let mut last = std::time::Instant::now();
        loop {
            if cancel.load(Relaxed) { return Err(NetError::Io("cancelled".into())); }
            match rx.recv_timeout(Duration::from_millis(500)) {
                Ok(Ok(chunk)) if chunk.is_empty() => break,
                Ok(Ok(chunk)) => {
                    last = std::time::Instant::now();
                    hasher.update(&chunk);
                    out.write_all(&chunk).map_err(io_err)?;
                    done += chunk.len() as u64;
                    progress(done, total);
                }
                Ok(Err(e)) => return Err(NetError::Offline(e)),
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
                    if last.elapsed() > Duration::from_secs(STALL_SECS) {
                        return Err(NetError::Offline(format!("no data for {STALL_SECS} seconds; the connection seems to be gone")));
                    }
                }
                Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return Err(NetError::Offline("the connection closed early".into())),
            }
        }
    }
    out.flush().map_err(io_err)?;
    drop(out);
    let got = hex(&hasher.finalize());
    if size != 0 && done != size {
        return Err(NetError::Mismatch(format!("size {done} bytes, expected {size}")));
    }
    if !sha256.is_empty() && !got.eq_ignore_ascii_case(sha256) {
        return Err(NetError::Mismatch(format!("sha256 {got}, expected {sha256}")));
    }
    fs::rename(part, dest).map_err(io_err)?;
    Ok(())
}

// ---------------------------------------------------------------- one launcher at a time

/// Holds the data folder's lock for as long as it lives (released by the kernel when the process dies, however it dies).
pub struct InstanceLock(#[allow(dead_code)] File);

/// Takes `<data>/launcher.lock`. Err(()) means another Portalcraft launcher is running on this data folder. Retries for
/// a few seconds so that a launcher restarted by the self-update can take over from the one that is exiting. A data
/// folder that cannot be written to gets no lock (the real error shows up where it matters).
#[cfg(windows)]
pub fn lock_instance(data: &Path, wait: Duration) -> Result<Option<InstanceLock>, ()> {
    use std::os::windows::fs::OpenOptionsExt;
    let _ = fs::create_dir_all(data);
    let t0 = std::time::Instant::now();
    loop {
        // share_mode(0): nobody else can open the file while we hold it; Windows frees it however the program ends
        match fs::OpenOptions::new().create(true).write(true).truncate(false).share_mode(0).open(data.join("launcher.lock")) {
            Ok(f) => return Ok(Some(InstanceLock(f))),
            Err(e) if e.raw_os_error() == Some(32) => {} // ERROR_SHARING_VIOLATION: another launcher has it
            Err(_) => return Ok(None),
        }
        if t0.elapsed() >= wait { return Err(()); }
        std::thread::sleep(Duration::from_millis(200));
    }
}

#[cfg(unix)]
pub fn lock_instance(data: &Path, wait: Duration) -> Result<Option<InstanceLock>, ()> {
    use std::os::unix::io::AsRawFd;
    let _ = fs::create_dir_all(data);
    let Ok(f) = fs::OpenOptions::new().create(true).write(true).truncate(false).open(data.join("launcher.lock")) else { return Ok(None) };
    let t0 = std::time::Instant::now();
    loop {
        if unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } == 0 { return Ok(Some(InstanceLock(f))); }
        if unsafe { *libc::__errno_location() } != libc::EWOULDBLOCK { return Ok(None); }
        if t0.elapsed() >= wait { return Err(()); }
        std::thread::sleep(Duration::from_millis(200));
    }
}

/// The running program's path; after a self-update the old inode is gone and Linux appends " (deleted)".
#[cfg(windows)]
pub fn exe_path() -> std::io::Result<PathBuf> { std::env::current_exe() }

#[cfg(unix)]
pub fn exe_path() -> std::io::Result<PathBuf> {
    let p = std::env::current_exe()?;
    let s = p.to_string_lossy();
    Ok(match s.strip_suffix(" (deleted)") { Some(t) => PathBuf::from(t), None => p })
}

/// Leftovers of an earlier run that was killed: half-downloaded files and half-copied installs. Needs the instance
/// lock (nothing else may be writing them). `keep` are the file names in downloads/ that are still wanted.
pub fn clean_leftovers(l: &crate::paths::Layout, keep: Option<&[String]>) -> Vec<String> {
    let mut gone = Vec::new();
    let mut rm = |p: PathBuf| { if fs::remove_file(&p).is_ok() { gone.push(p.display().to_string()); } };
    if let Ok(rd) = fs::read_dir(l.downloads_dir()) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().to_string();
            if n.ends_with(".part") || n.ends_with(".tmp") || keep.is_some_and(|k| !k.contains(&n)) { rm(e.path()); }
        }
    }
    for dir in [l.plugin_dir(), l.mods_dir()] {
        if let Ok(rd) = fs::read_dir(&dir) {
            for e in rd.flatten() {
                let n = e.file_name().to_string_lossy().to_string();
                if n.starts_with('.') && n.ends_with(".new") { rm(e.path()); }
            }
        }
    }
    gone
}

// ---------------------------------------------------------------- processes (/proc)

#[derive(Debug, Clone)]
pub struct Proc {
    pub pid: u32,
    pub comm: String,
    pub cmdline: Vec<String>,
    /// Linux: clock ticks since boot (/proc stat field 22). Windows: seconds since 1970. Only compared with itself.
    pub starttime: u64,
    /// The program file (Windows only; Linux callers read /proc/<pid>/exe themselves).
    #[allow(dead_code)]
    pub exe: Option<PathBuf>,
}

#[cfg(windows)]
pub fn list_procs() -> Vec<Proc> { crate::winsys::list_procs() }

#[cfg(unix)]
pub fn list_procs() -> Vec<Proc> {
    let mut v = Vec::new();
    let Ok(rd) = fs::read_dir("/proc") else { return v };
    for e in rd.flatten() {
        let Some(pid) = e.file_name().to_str().and_then(|s| s.parse::<u32>().ok()) else { continue };
        let base = e.path();
        let comm = fs::read_to_string(base.join("comm")).unwrap_or_default().trim().to_string();
        let cmd = fs::read(base.join("cmdline")).unwrap_or_default();
        let cmdline: Vec<String> = cmd.split(|b| *b == 0).filter(|s| !s.is_empty()).map(|s| String::from_utf8_lossy(s).to_string()).collect();
        let starttime = proc_starttime(pid).unwrap_or(0);
        v.push(Proc { pid, comm, cmdline, starttime, exe: None });
    }
    v
}

#[cfg(windows)]
#[allow(dead_code)]
pub fn proc_starttime(pid: u32) -> Option<u64> { crate::winsys::proc_starttime(pid) }

#[cfg(unix)]
pub fn proc_starttime(pid: u32) -> Option<u64> {
    let s = fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    let rest = s.rsplit_once(')')?.1;
    rest.split_whitespace().nth(19)?.parse().ok()
}

#[cfg(windows)]
pub fn pid_alive(pid: u32) -> bool { crate::winsys::pid_alive(pid) }

#[cfg(unix)]
pub fn pid_alive(pid: u32) -> bool {
    unsafe { libc::kill(pid as i32, 0) == 0 || *libc::__errno_location() == libc::EPERM }
}

#[cfg(unix)]
pub fn portal2_pids() -> Vec<u32> {
    list_procs().into_iter().filter(|p| p.comm == "portal2_linux").map(|p| p.pid).collect()
}

#[cfg(windows)]
pub fn portal2_pids() -> Vec<u32> {
    list_procs().into_iter().filter(|p| p.comm.eq_ignore_ascii_case("portal2.exe")).map(|p| p.pid).collect()
}

/// Windows has no /dev/shm: the link between the games lives in a named file mapping, which needs no check.
#[cfg(windows)]
pub fn shm_free_bytes() -> Option<u64> { None }

/// Free bytes on /dev/shm, or None if it cannot be read.
#[cfg(unix)]
pub fn shm_free_bytes() -> Option<u64> {
    let c = std::ffi::CString::new("/dev/shm").unwrap();
    unsafe {
        let mut s: libc::statvfs = std::mem::zeroed();
        if libc::statvfs(c.as_ptr(), &mut s) != 0 {
            return None;
        }
        Some(s.f_bavail as u64 * s.f_frsize as u64)
    }
}

/// Free bytes where `p` is (the nearest existing folder above it).
#[cfg(windows)]
pub fn free_bytes(p: &Path) -> Option<u64> { crate::winsys::free_bytes(p) }

#[cfg(unix)]
pub fn free_bytes(p: &Path) -> Option<u64> {
    use std::os::unix::ffi::OsStrExt;
    let c = std::ffi::CString::new(p.as_os_str().as_bytes()).ok()?;
    unsafe {
        let mut s: libc::statvfs = std::mem::zeroed();
        if libc::statvfs(c.as_ptr(), &mut s) != 0 { return None; }
        Some(s.f_bavail as u64 * s.f_frsize as u64)
    }
}

/// The player's home folder ($HOME, %USERPROFILE%).
pub fn home_dir() -> PathBuf {
    #[cfg(unix)]
    { PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| "/tmp".into())) }
    #[cfg(windows)]
    { std::env::var_os("USERPROFILE").filter(|v| !v.is_empty()).map(PathBuf::from).unwrap_or_else(std::env::temp_dir) }
}

/// A short notice outside the window (the launcher refused to start twice).
pub fn notify(title: &str, msg: &str) {
    #[cfg(unix)]
    { let _ = std::process::Command::new("notify-send").args(["-a", "Portalcraft", title, msg]).status(); }
    #[cfg(windows)]
    { crate::winsys::message_box(title, msg); }
}

#[cfg(windows)]
static CLI_OUT: Mutex<Option<PathBuf>> = Mutex::new(None);

/// Where --cli output is also written (Windows: the release program has no console of its own, and cmd.exe does not wait
/// for it, so a text file is the reliable way to read what it said).
#[cfg(windows)]
pub fn set_cli_output(p: PathBuf) { *CLI_OUT.lock().unwrap() = Some(p); }

/// One line of --cli output: to stdout, and on Windows also appended to the file set by set_cli_output().
pub fn cli_out(line: &str) {
    println!("{line}");
    #[cfg(windows)]
    if let Some(p) = CLI_OUT.lock().unwrap().as_ref() {
        if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(p) { let _ = writeln!(f, "{line}"); }
    }
}

#[cfg(windows)]
pub fn open_path(p: &Path) {
    let mut c = std::process::Command::new("explorer.exe");
    c.arg(p.to_string_lossy().replace('/', "\\")).stdin(std::process::Stdio::null()).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null());
    let _ = c.spawn();
}

#[cfg(unix)]
pub fn open_path(p: &Path) {
    let _ = std::process::Command::new("xdg-open")
        .arg(p)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn();
}

/// Atomic text write (temp + rename).
pub fn write_atomic(path: &Path, data: &[u8]) -> std::io::Result<()> {
    if let Some(p) = path.parent() {
        fs::create_dir_all(p)?;
    }
    let tmp = PathBuf::from(format!("{}.tmp", path.display()));
    fs::write(&tmp, data)?;
    fs::rename(&tmp, path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn second_launcher_is_refused_then_allowed_after_release() {
        let t = std::env::temp_dir().join(format!("pc-lock-{}", std::process::id()));
        let _ = fs::remove_dir_all(&t);
        let a = lock_instance(&t, Duration::from_millis(0)).unwrap();
        assert!(a.is_some());
        assert!(lock_instance(&t, Duration::from_millis(300)).is_err());
        drop(a);
        assert!(lock_instance(&t, Duration::from_millis(0)).unwrap().is_some());
        let _ = fs::remove_dir_all(&t);
    }

    #[test]
    fn leftovers_are_cleaned_and_downloads_pruned() {
        let t = std::env::temp_dir().join(format!("pc-left-{}", std::process::id()));
        let _ = fs::remove_dir_all(&t);
        let l = crate::paths::Layout { data: t.clone() };
        for d in [l.downloads_dir(), l.plugin_dir(), l.mods_dir()] { fs::create_dir_all(d).unwrap(); }
        for f in ["downloads/prism.tar.gz.part", "downloads/old-0.0.9.jar", "downloads/keep.jar", "plugin/.portalcraft_bridge.so.new", "prism/instances/Portalcraft/.minecraft/mods/.portalcraft-1.jar.new", "prism/instances/Portalcraft/.minecraft/mods/portalcraft-1.jar"] {
            fs::write(t.join(f), "x").unwrap();
        }
        clean_leftovers(&l, None);
        assert!(!t.join("downloads/prism.tar.gz.part").exists() && t.join("downloads/old-0.0.9.jar").exists());
        assert!(!t.join("plugin/.portalcraft_bridge.so.new").exists());
        assert!(!l.mods_dir().join(".portalcraft-1.jar.new").exists() && l.mods_dir().join("portalcraft-1.jar").exists());
        clean_leftovers(&l, Some(&["keep.jar".to_string()]));
        assert!(!t.join("downloads/old-0.0.9.jar").exists() && t.join("downloads/keep.jar").exists());
        let _ = fs::remove_dir_all(&t);
    }

    #[test]
    fn failed_copy_download_leaves_no_part_file() {
        let t = std::env::temp_dir().join(format!("pc-dl-{}", std::process::id()));
        let _ = fs::remove_dir_all(&t);
        fs::create_dir_all(&t).unwrap();
        let src = t.join("src.bin");
        fs::write(&src, "hello").unwrap();
        let cancel = std::sync::atomic::AtomicBool::new(false);
        let dest = t.join("d/out.bin");
        let e = download_verified(src.to_str().unwrap(), &dest, &"0".repeat(64), 0, &cancel, &mut |_, _| {});
        assert!(matches!(e, Err(NetError::Mismatch(_))));
        assert!(!t.join("d/out.bin.part").exists() && !dest.exists());
        let _ = fs::remove_dir_all(&t);
    }
}
