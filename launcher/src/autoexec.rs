//! The temporary portal2/cfg/autoexec.cfg that loads the plugin and the map (the same job play_clip.sh does).
//! It is written right before Portal 2 starts and removed as soon as the level is in, on every error path, when the
//! guard is dropped and (for a crash of the launcher itself) at the next launcher start. A cfg the player already had is
//! moved aside first and put back afterwards.
use std::fs;
use std::path::{Path, PathBuf};

pub const MARKER: &str = "// portalcraft-launcher: temporary file, removed once the level has loaded";
const SAVED: &str = "autoexec.cfg.portalcraft-saved";

pub fn path(p2: &Path) -> PathBuf { p2.join("portal2/cfg/autoexec.cfg") }
fn saved(p2: &Path) -> PathBuf { p2.join("portal2/cfg").join(SAVED) }
fn update_path(p2: &Path) -> PathBuf { p2.join("update/cfg/autoexec.cfg") }

pub fn is_ours(p: &Path) -> bool {
    fs::read_to_string(p).map(|t| t.lines().next().map(|l| l.trim() == MARKER).unwrap_or(false)).unwrap_or(false)
}

pub struct Script<'a> {
    pub so: &'a Path,
    pub start_as_mc: bool,
    pub unified_world: bool,
    pub map: &'a str,
    /// Continue from this Portal 2 save (its name without .sav) instead of starting `map`.
    pub load: Option<&'a str>,
}

pub fn lines(s: &Script) -> String {
    let mut t = String::new();
    t.push_str(MARKER);
    t.push('\n');
    // Windows: forward slashes (Portal 2 reads them) and the whole path quoted, as on Linux
    t.push_str(&format!("plugin_load \"{}\"\n", crate::paths::slashed(s.so)));
    if s.start_as_mc { t.push_str("pc_start_mode mc\n"); }
    if s.unified_world { t.push_str("pc_world unified\n"); }
    match s.load {
        Some(save) => t.push_str(&format!("load {save}\n")),
        None => t.push_str(&format!("map {}\n", s.map)),
    }
    t
}

/// Does this plugin build know the pc_world command? (play_clip.sh greps the .so for it.)
pub fn plugin_knows_world(so: &Path) -> bool {
    fs::read(so).map(|b| b.windows(8).any(|w| w == b"pc_world")).unwrap_or(false)
}

/// Leftovers of a launcher that was killed: our temporary file is removed and a saved cfg of the player is restored.
/// Returns what was done, for the log.
pub fn recover(p2: &Path) -> Option<String> {
    let a = path(p2);
    let mut did = Vec::new();
    if a.exists() && is_ours(&a) {
        if fs::remove_file(&a).is_ok() { did.push("removed a leftover temporary autoexec.cfg"); }
    }
    let s = saved(p2);
    if s.exists() && !a.exists() {
        if fs::rename(&s, &a).is_ok() { did.push("put your own autoexec.cfg back"); }
    } else if s.exists() && a.exists() {
        // the player wrote a new autoexec.cfg after a crash left their old one aside: keep both, never overwrite either
        let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        if fs::rename(&s, s.with_file_name(format!("autoexec-old-{stamp}.cfg"))).is_ok() { did.push("kept your older autoexec.cfg as autoexec-old-<time>.cfg next to the new one"); }
    }
    if did.is_empty() { None } else { Some(did.join(", ")) }
}

// ---- cleanup when the launcher is told to quit (logout, shutdown, `kill`, closing its terminal, Ctrl-C) ----
// Without it the temporary autoexec.cfg would stay and make the player's next ordinary Portal 2 start load a plugin and
// jump into chamber 1. Only async-signal-safe calls (unlink, rename, _exit) run in the handler; the C strings are
// prepared before the signal can arrive.
#[cfg(unix)]
static SIG_AUTOEXEC: std::sync::atomic::AtomicPtr<libc::c_char> = std::sync::atomic::AtomicPtr::new(std::ptr::null_mut());
#[cfg(unix)]
static SIG_SAVED: std::sync::atomic::AtomicPtr<libc::c_char> = std::sync::atomic::AtomicPtr::new(std::ptr::null_mut());
#[cfg(unix)]
static SIG_HAD_SAVED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

#[cfg(unix)]
extern "C" fn on_quit_signal(sig: libc::c_int) {
    use std::sync::atomic::Ordering::SeqCst;
    let a = SIG_AUTOEXEC.load(SeqCst);
    if !a.is_null() {
        unsafe {
            libc::unlink(a);
            let s = SIG_SAVED.load(SeqCst);
            if SIG_HAD_SAVED.load(SeqCst) && !s.is_null() { libc::rename(s, a); }
            libc::_exit(128 + sig);
        }
    }
    unsafe { libc::_exit(128 + sig) }
}

#[cfg(windows)]
fn arm_signal_cleanup(p2: &Path) {
    // Ctrl-C / Ctrl-Break / closing the console of a --cli run (a window launcher is covered by on_exit)
    crate::winsys::arm_cleanup(path(p2), saved(p2), saved(p2).exists());
}

#[cfg(unix)]
fn arm_signal_cleanup(p2: &Path) {
    use std::os::unix::ffi::OsStrExt;
    use std::sync::atomic::Ordering::SeqCst;
    let (Ok(a), Ok(s)) = (std::ffi::CString::new(path(p2).as_os_str().as_bytes()), std::ffi::CString::new(saved(p2).as_os_str().as_bytes())) else { return };
    SIG_HAD_SAVED.store(saved(p2).exists(), SeqCst);
    // leaked on purpose (a few bytes per play); the handler may run at any moment
    SIG_SAVED.store(s.into_raw(), SeqCst);
    SIG_AUTOEXEC.store(a.into_raw(), SeqCst);
    unsafe {
        let mut act: libc::sigaction = std::mem::zeroed();
        act.sa_sigaction = on_quit_signal as *const () as usize;
        libc::sigemptyset(&mut act.sa_mask);
        for sig in [libc::SIGTERM, libc::SIGHUP, libc::SIGINT] { libc::sigaction(sig, &act, std::ptr::null_mut()); }
    }
}

#[cfg(windows)]
fn disarm_signal_cleanup() { crate::winsys::disarm_cleanup(); }

#[cfg(unix)]
fn disarm_signal_cleanup() {
    SIG_AUTOEXEC.store(std::ptr::null_mut(), std::sync::atomic::Ordering::SeqCst);
    unsafe { for sig in [libc::SIGTERM, libc::SIGHUP, libc::SIGINT] { libc::signal(sig, libc::SIG_DFL); } }
}

#[derive(Debug)]
pub enum InstallError {
    UpdateCfgInTheWay(PathBuf),
    Io(String),
}

pub struct Guard {
    p2: PathBuf,
    done: bool,
}

impl Guard {
    pub fn install(p2: &Path, text: &str) -> Result<Guard, InstallError> {
        recover(p2);
        if update_path(p2).exists() {
            return Err(InstallError::UpdateCfgInTheWay(update_path(p2)));
        }
        let a = path(p2);
        if let Some(d) = a.parent() { fs::create_dir_all(d).map_err(|e| InstallError::Io(e.to_string()))?; }
        if a.exists() {
            // the player's own cfg: moved aside, restored later
            fs::rename(&a, saved(p2)).map_err(|e| InstallError::Io(format!("{}: {e}", a.display())))?;
        }
        let text = if text.starts_with(MARKER) { text.to_string() } else { format!("{MARKER}\n{text}") };
        if let Err(e) = fs::write(&a, text) {
            // a half-written file (disk full) may not even carry our marker line: it is ours either way
            let _ = fs::remove_file(&a);
            let _ = recover(p2);
            return Err(InstallError::Io(format!("{}: {e}", a.display())));
        }
        arm_signal_cleanup(p2);
        Ok(Guard { p2: p2.to_path_buf(), done: false })
    }
    /// Removes the temporary file and restores the player's own, now.
    pub fn remove(&mut self) {
        if self.done { return; }
        self.done = true;
        disarm_signal_cleanup();
        let a = path(&self.p2);
        if is_ours(&a) { let _ = fs::remove_file(&a); }
        recover(&self.p2);
    }
}

impl Drop for Guard {
    fn drop(&mut self) { self.remove(); }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fake_p2(tag: &str) -> PathBuf {
        let t = std::env::temp_dir().join(format!("pc-ae-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&t);
        fs::create_dir_all(t.join("portal2/cfg")).unwrap();
        t
    }

    #[test]
    fn written_then_removed_on_drop() {
        let p2 = fake_p2("a");
        let so = Path::new("/d/plugin/portalcraft_bridge.so");
        let txt = lines(&Script { so, start_as_mc: true, unified_world: true, map: "sp_a1_intro1", load: None });
        assert!(txt.contains("plugin_load \"/d/plugin/portalcraft_bridge.so\"\npc_start_mode mc\npc_world unified\nmap sp_a1_intro1\n"));
        {
            let _g = Guard::install(&p2, &txt).unwrap();
            assert!(is_ours(&path(&p2)));
        }
        assert!(!path(&p2).exists());
        let _ = fs::remove_dir_all(&p2);
    }

    #[test]
    fn players_own_cfg_is_kept() {
        let p2 = fake_p2("b");
        fs::write(path(&p2), "sv_cheats 1\n").unwrap();
        let mut g = Guard::install(&p2, "x\n").unwrap();
        assert!(fs::read_to_string(path(&p2)).unwrap().ends_with("x\n"));
        g.remove();
        assert_eq!(fs::read_to_string(path(&p2)).unwrap(), "sv_cheats 1\n");
        assert!(!saved(&p2).exists());
        let _ = fs::remove_dir_all(&p2);
    }

    #[test]
    fn crash_leftovers_are_recovered() {
        let p2 = fake_p2("c");
        fs::write(saved(&p2), "sv_cheats 1\n").unwrap();
        fs::write(path(&p2), format!("{MARKER}\nmap x\n")).unwrap();
        let g = Guard::install(&p2, "y\n"); // recover() runs first, then installs
        assert!(g.is_ok());
        std::mem::forget(g.unwrap()); // simulate the launcher dying with the file in place
        assert!(recover(&p2).is_some());
        assert_eq!(fs::read_to_string(path(&p2)).unwrap(), "sv_cheats 1\n");
        let _ = fs::remove_dir_all(&p2);
    }

    #[test]
    fn update_cfg_blocks() {
        let p2 = fake_p2("d");
        fs::create_dir_all(p2.join("update/cfg")).unwrap();
        fs::write(update_path(&p2), "").unwrap();
        assert!(matches!(Guard::install(&p2, "z"), Err(InstallError::UpdateCfgInTheWay(_))));
        let _ = fs::remove_dir_all(&p2);
    }

    #[test]
    fn windows_paths_are_quoted_with_forward_slashes() {
        let so = PathBuf::from(r"C:\Users\Jo Smith\AppData\Local\Portalcraft\plugin\portalcraft_bridge.dll");
        let txt = lines(&Script { so: &so, start_as_mc: false, unified_world: false, map: "sp_a1_intro1", load: None });
        if cfg!(windows) {
            assert!(txt.contains("plugin_load \"C:/Users/Jo Smith/AppData/Local/Portalcraft/plugin/portalcraft_bridge.dll\"\n"));
        }
    }

    #[test]
    fn continue_loads_the_save_instead_of_a_map() {
        let so = std::path::PathBuf::from("/d/plugin/portalcraft_bridge.so");
        let txt = lines(&Script { so: &so, start_as_mc: true, unified_world: true, map: "sp_a1_intro1", load: Some("autosave") });
        assert!(txt.ends_with("pc_world unified\nload autosave\n"));
        assert!(!txt.contains("map sp_a1_intro1"));
    }
}
