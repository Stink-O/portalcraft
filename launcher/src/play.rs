//! Play: start (or reuse) the hidden Minecraft, start Portal 2 with the plugin, wait for the link.
//! The same job as bridge/tools/play_clip.sh, but through Prism and with a temporary autoexec.cfg that never stays behind.
use crate::autoexec::{self, Guard, InstallError, Script};
use crate::engine::*;
use crate::paths::MC_VERSION;
use crate::portal2;
use crate::prism::{self, McProc};
use crate::util::{self, log, portal2_pids};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Clone, Debug, PartialEq)]
pub enum PlayKind { Campaign, PitFlings }

#[derive(Clone, Debug)]
pub struct PlayOpts {
    pub kind: PlayKind,
    /// Overrides the campaign start map.
    pub map: Option<String>,
    pub start_as_mc: bool,
    /// Continue from this Portal 2 save (name without .sav).
    pub load: Option<String>,
    /// A new game that also starts a fresh Minecraft world: the old one is kept as a backup folder.
    pub reset_world: bool,
}

pub const LINK_WAIT_S: u64 = 480;

pub fn map_for(opts: &PlayOpts, start_map: &str) -> String {
    match opts.kind {
        PlayKind::PitFlings => "pc_pit_flings".into(),
        PlayKind::Campaign => opts.map.clone().filter(|m| !m.trim().is_empty()).or_else(|| Some(start_map.trim().to_string()).filter(|m| !m.is_empty())).unwrap_or_else(|| "sp_a1_intro1".into()),
    }
}

/// The worlds "New game" put aside (Portalcraft-old-<time>), with their sizes in bytes, newest first.
pub fn old_worlds(l: &crate::paths::Layout) -> Vec<(PathBuf, u64)> {
    fn size(p: &Path) -> u64 {
        std::fs::read_dir(p).map(|rd| rd.flatten().map(|e| {
            let m = e.metadata().ok();
            if m.as_ref().is_some_and(|m| m.is_dir()) { size(&e.path()) } else { m.map(|m| m.len()).unwrap_or(0) }
        }).sum()).unwrap_or(0)
    }
    let Some(dir) = l.world_dir().parent().map(Path::to_path_buf) else { return vec![] };
    let prefix = format!("{}-old-", crate::paths::INSTANCE_ID);
    let mut v: Vec<(PathBuf, u64)> = std::fs::read_dir(&dir).map(|rd| rd.flatten()
        .filter(|e| e.file_name().to_string_lossy().starts_with(&prefix) && e.path().is_dir())
        .map(|e| { let p = e.path(); let s = size(&p); (p, s) }).collect()).unwrap_or_default();
    v.sort_by(|a, b| b.0.cmp(&a.0));
    v
}

/// Moves the Minecraft world aside (never deletes it), so the mod makes a fresh one with a new random seed.
pub fn backup_world(l: &crate::paths::Layout) -> std::io::Result<Option<PathBuf>> {
    let w = l.world_dir();
    if !w.exists() { return Ok(None); }
    let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let b = w.with_file_name(format!("{}-old-{stamp}", crate::paths::INSTANCE_ID));
    std::fs::rename(&w, &b)?;
    // the old world's spot report must not be read for the new one
    let _ = std::fs::remove_file(l.bridge_dir().join("portalcraft_world.env"));
    Ok(Some(b))
}

/// Puts the old Minecraft world back if the launch failed before a new Minecraft could create its fresh one (so a
/// failed "New game" does not silently cost the player their world).
struct WorldRestore { world: PathBuf, backup: PathBuf, armed: bool }
impl Drop for WorldRestore {
    fn drop(&mut self) {
        if self.armed && !self.world.exists() && self.backup.exists() {
            let _ = std::fs::rename(&self.backup, &self.world);
            log("the new-world launch failed early: your old Minecraft world was put back");
        }
    }
}

/// Valve's map names are letters, digits and underscores; anything else would end up in a console line.
pub fn valid_map_name(m: &str) -> bool {
    !m.is_empty() && m.len() < 64 && m.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')
}

/// How long to wait for Minecraft to appear: Prism downloads Java and the game assets on the first start, which can
/// take a while on a slow line.
fn mc_start_wait_s(l: &crate::paths::Layout) -> u64 {
    if l.prism_dir().join("java").is_dir() { 300 } else { 1200 }
}

#[cfg(unix)]
fn cpu_ticks(pid: u32) -> u64 {
    let Ok(s) = std::fs::read_to_string(format!("/proc/{pid}/stat")) else { return 0 };
    let Some(rest) = s.rsplit_once(')').map(|x| x.1.to_string()) else { return 0 };
    let f: Vec<&str> = rest.split_whitespace().collect();
    f.get(11).and_then(|x| x.parse::<u64>().ok()).unwrap_or(0) + f.get(12).and_then(|x| x.parse::<u64>().ok()).unwrap_or(0)
}

#[cfg(unix)]
fn proc_state(pid: u32) -> char {
    std::fs::read_to_string(format!("/proc/{pid}/stat")).ok().and_then(|s| s.rsplit_once(')').and_then(|x| x.1.trim().chars().next())).unwrap_or('?')
}

#[cfg(windows)]
fn mc_progressing(pid: u32) -> bool {
    if !util::pid_alive(pid) { return false; }
    let a = crate::winsys::cpu_ms(pid);
    std::thread::sleep(Duration::from_secs(3));
    crate::winsys::cpu_ms(pid) > a
}

#[cfg(unix)]
fn mc_progressing(pid: u32) -> bool {
    if matches!(proc_state(pid), 'T' | 't' | 'Z' | 'X' | '?') { return false; }
    let a = cpu_ticks(pid);
    std::thread::sleep(Duration::from_secs(3));
    cpu_ticks(pid) > a
}

/// The bridge log from byte offset `from` on. The plugin only ever appends to it and it can be hundreds of MB, so
/// this reads just the new part (a file that got shorter than `from` was replaced: read it all).
fn log_since(p: &Path, from: u64) -> String {
    use std::io::{Read, Seek, SeekFrom};
    let Ok(mut f) = std::fs::File::open(p) else { return String::new() };
    let len = f.metadata().map(|m| m.len()).unwrap_or(0);
    let start = if from > len { 0 } else { from };
    if f.seek(SeekFrom::Start(start)).is_err() { return String::new(); }
    let mut b = Vec::new();
    let _ = f.read_to_end(&mut b);
    String::from_utf8_lossy(&b).into_owned()
}

/// Size of the bridge log now: where "this session" starts.
fn log_len(p: &Path) -> u64 { std::fs::metadata(p).map(|m| m.len()).unwrap_or(0) }

/// For the launcher's window closing (call it from on_exit, after autoexec::recover): if this launcher started a
/// Minecraft and no Portal 2 with the plugin loaded exists, that Minecraft would wait for a host that never comes (an
/// invisible java eating RAM), so it is closed. Only the process recorded in state.json (pid + start time) is touched.
/// Returns what was done, for the log.
pub fn abandon_unlinked_minecraft(l: &crate::paths::Layout) -> Option<String> {
    let st = crate::settings::State::load(l);
    if st.mc_pid == 0 { return None; }
    let p = prism::minecraft_procs(l).into_iter().find(|p| p.pid == st.mc_pid && p.starttime == st.mc_starttime)?;
    let linked_p2 = portal2_pids().iter().any(|pid| attach_start(&l.bridge_log(), *pid).is_some());
    if linked_p2 { return None; }
    let ok = prism::stop_pid(p.pid, 10);
    Some(format!("closed the Minecraft this launcher started (pid {}) because no Portal 2 with the plugin is running: {}", p.pid, if ok { "done" } else { "it did not quit" }))
}

/// If the running Portal 2 has our plugin loaded: the line number its session log starts at.
fn attach_start(bridge_log: &Path, p2pid: u32) -> Option<u64> {
    // only the tail matters (the newest session); the file can be huge
    let len = log_len(bridge_log);
    let from = len.saturating_sub(8 * 1024 * 1024);
    let s = log_since(bridge_log, from);
    let mut off = from;
    let mut last: Option<(u64, bool, u32)> = None; // (byte offset of the line, loaded, host pid)
    for l in s.split_inclusive('\n') {
        let i = off;
        off += l.len() as u64;
        // Linux: "link: /dev/shm/Portalcraft_v1 created, ..."; a Windows plugin logs "link: Local\\Portalcraft_v1 created, ..."
        if let Some(pos) = l.find("link: ").filter(|_| l.contains("Portalcraft_v1 created")) {
            let pid = l[pos..].rsplit(' ').next().and_then(|x| x.trim().parse::<u32>().ok()).unwrap_or(0);
            last = Some((i, true, pid));
        } else if l.contains("[bridge] unloading") {
            if let Some(x) = last.as_mut() { x.1 = false; }
        }
    }
    last.and_then(|(i, loaded, pid)| (loaded && pid == p2pid).then_some(i))
}

/// A Minecraft that already started closing, or one linked to a Portal 2 that is gone (it quits 5 s after its host does).
fn mc_quitting(latest_log: &Path) -> bool {
    let Ok(t) = std::fs::read_to_string(latest_log) else { return false };
    if t.lines().rev().take(8).any(|x| x.contains("Stopping!") || x.contains("has closed; saving and quitting")) { return true; }
    let last_host = t.lines().rev().find_map(|x| {
        let i = x.find("linked to the host game (pid ")?;
        x[i + "linked to the host game (pid ".len()..].split(')').next()?.trim().parse::<u32>().ok()
    });
    last_host.map(|pid| !util::pid_alive(pid)).unwrap_or(false)
}

#[cfg(windows)]
fn steam_launch_command() -> Option<Command> {
    // Development only: PORTALCRAFT_P2_DIRECT=<folder with portal2.exe> starts that copy itself (a SteamCMD copy outside Steam's
    // library, steam_appid.txt = 620 next to it; set PORTAL2_DIR to the same folder) instead of Steam's own Portal 2. Players never set it.
    if let Some(dir) = std::env::var_os("PORTALCRAFT_P2_DIRECT").map(std::path::PathBuf::from) {
        let exe = dir.join("portal2.exe");
        if exe.is_file() {
            let mut c = Command::new(exe);
            c.args(["-game", "portal2", "-steam"]).current_dir(&dir);
            return Some(c);
        }
    }
    let url = "steam://rungameid/620";
    if let Some(exe) = crate::winsys::steam_exe() { let mut c = Command::new(exe); c.arg(url); return Some(c); }
    let mut c = Command::new("explorer.exe"); c.arg(url); Some(c)
}

#[cfg(unix)]
fn steam_launch_command() -> Option<Command> {
    let has = |n: &str| std::env::var_os("PATH").map(|p| std::env::split_paths(&p).any(|d| d.join(n).is_file())).unwrap_or(false);
    let url = "steam://rungameid/620";
    if has("steam") { let mut c = Command::new("steam"); c.arg(url); return Some(c); }
    let flat = std::path::PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".var/app/com.valvesoftware.Steam");
    if flat.is_dir() && has("flatpak") { let mut c = Command::new("flatpak"); c.args(["run", "com.valvesoftware.Steam", url]); return Some(c); }
    if has("xdg-open") { let mut c = Command::new("xdg-open"); c.arg(url); return Some(c); }
    None
}

fn sleep_or_cancel(rep: &Reporter, s: f32) -> bool {
    let end = Instant::now() + Duration::from_secs_f32(s);
    while Instant::now() < end {
        if rep.cancelled() { return true; }
        std::thread::sleep(Duration::from_millis(250));
    }
    rep.cancelled()
}

fn stopped(step: StepId) -> UserError {
    UserError::new("Stopped", "Nothing was closed. Press PLAY again whenever you like.").at(step)
}

pub fn run_play(cx: &Ctx, opts: &PlayOpts) -> R<()> {
    let r = &cx.rep;
    let l = &cx.layout;
    let s = &cx.settings;

    // ---------------------------------------------------------- prepare
    let id = StepId::PlayPrepare;
    r.step(id, StepState::Running, "");
    let fail = |e: UserError| { r.step(e.step.unwrap_or(id), StepState::Failed, &e.title); e };
    if !fully_installed(l) {
        let missing: Vec<&str> = installed_report(l).into_iter().filter(|(_, ok)| !ok).map(|(n, _)| n).collect();
        let body = if missing == ["Signed in"] {
            "You are not signed in to Minecraft yet. Press 'Continue the setup', then sign in with your Microsoft account in the Prism window (it must be the account that owns Minecraft: Java Edition).".to_string()
        } else {
            format!("Still missing: {}. Press 'Continue the setup' to finish it.", missing.join(", "))
        };
        return Err(fail(UserError::new("Setup is not finished", &body).hint(Hint::Setup).at(id)));
    }
    if plugin_missing(l) {
        return Err(fail(no_plugin_error().at(id)));
    }
    let p2 = portal2::find(&s.portal2_dir).map_err(|e| fail(portal2_error(&e).at(id)))?;
    if l.plugin_so().to_string_lossy().contains('"') {
        return Err(fail(UserError::new("The data folder's name has a \" in it", &format!("Portal 2 cannot load a plugin from a path containing a double quote: {}. Move Portalcraft's data folder somewhere without one (set PORTALCRAFT_DATA), then run the setup again.", l.data.display())).at(id)));
    }
    let map = map_for(opts, &s.start_map);
    if opts.load.as_deref().is_some_and(|n| !valid_map_name(n)) {
        return Err(fail(UserError::new("That save name is not valid", "Portalcraft only loads saves named with letters, numbers and underscores.").at(id)));
    }
    if !valid_map_name(&map) {
        return Err(fail(UserError::new("That map name is not valid", "Map names are letters, numbers and underscores only, for example sp_a2_bridge_intro. Fix it in Settings.").hint(Hint::Settings).at(id)));
    }
    if let Some(msg) = autoexec::recover(&p2) { r.line(&format!("autoexec: {msg}")); }
    let p2pids = portal2_pids();
    let attach = p2pids.first().and_then(|pid| attach_start(&l.bridge_log(), *pid).map(|line| (*pid, line)));
    if opts.reset_world && attach.is_some() {
        return Err(fail(UserError::new("Close the games first", "A new Minecraft world can only be made while Portal 2 and Minecraft are closed. Close them, then press RETRY.").at(id)));
    }
    if !p2pids.is_empty() && attach.is_none() {
        return Err(fail(UserError::new(
            "Portal 2 is already running",
            "Portal 2 is open without Portalcraft's plugin. Portalcraft never closes a game it did not start: close Portal 2 yourself, then press RETRY.",
        ).at(id)));
    }
    if attach.is_none() {
        if let Some(f) = util::shm_free_bytes() {
            // a leftover link from a crashed session takes ~220 MB; with no game running it is garbage
            let stale = Path::new("/dev/shm/Portalcraft_v1");
            if cfg!(unix) && stale.exists() && prism::minecraft_procs(l).is_empty() {
                let _ = std::fs::remove_file(stale);
                r.line("removed a leftover /dev/shm/Portalcraft_v1 from an earlier crash");
            }
            let f = util::shm_free_bytes().unwrap_or(f);
            if f < SHM_NEED { return Err(fail(shm_full_error(f).at(id))); }
        }
        let home = util::home_dir();
        if let Some(tool) = portal2::forced_compat_tool(&home) {
            return Err(fail(UserError::new(
                "Steam runs Portal 2 through Proton",
                &format!("Steam is set to use '{tool}' (Steam Play) for Portal 2, so it would start the Windows version, which Portalcraft cannot use. In Steam: right-click Portal 2, Properties, Compatibility, switch OFF 'Force the use of a specific Steam Play compatibility tool'. Let Steam finish downloading, then press RETRY."),
            ).hint(Hint::Steam).at(id)));
        }
        if map.starts_with("pc_") && !portal2::maps_dir(&p2).join(format!("{map}.bsp")).exists() {
            return Err(fail(UserError::new(
                "The demo map is not built yet",
                "The pit_flings demo is made from your own copy of Portal 2 (we cannot hand out Valve's files). Build it once from Settings, 'Build the demo map', with Portal 2 closed. Or play the campaign now: it needs no extra files.",
            ).hint(Hint::Settings).at(id)));
        }
    }
    r.step(id, StepState::Done, &format!("{}; map {map}", if attach.is_some() { "Portal 2 already running with the plugin" } else { "ready" }));

    // ---------------------------------------------------------- Minecraft
    let id = StepId::PlayMinecraft;
    r.step(id, StepState::Running, "");
    let fail = |e: UserError| { r.step(e.step.unwrap_or(id), StepState::Failed, &e.title); e };
    let mut procs = prism::minecraft_procs(l);
    if procs.len() > 1 {
        let ids: Vec<String> = procs.iter().map(|p| p.pid.to_string()).collect();
        return Err(fail(UserError::new("Several Minecraft windows are running", &format!("Portalcraft found more than one Minecraft of its own (process {}) and will not guess which to use. Close them (or log out and in) and press RETRY.", ids.join(", "))).at(id)));
    }
    // a Minecraft that belonged to the last Portal 2 quits with it: let it finish
    if let Some(p) = procs.first() {
        let quitting = mc_quitting(&l.mc_latest_log());
        if quitting && attach.is_none() {
            r.note(id, "the last Minecraft is still closing; waiting for it");
            for _ in 0..60 { if !util::pid_alive(p.pid) { break; } if sleep_or_cancel(r, 1.0) { return Err(fail(stopped(id))); } }
            procs = prism::minecraft_procs(l);
        }
    }
    let mut world_restore: Option<WorldRestore> = None;
    if opts.reset_world {
        if let Some(p) = procs.first().cloned() {
            let st = cx.state();
            if !(st.mc_pid == p.pid && st.mc_starttime == p.starttime) {
                return Err(fail(UserError::new("Close Minecraft first", &format!("A new world can only be made while Minecraft is closed, and this launcher did not start the one that is running (process {}). Close it, then press RETRY.", p.pid)).at(id)));
            }
            r.note(id, "closing Minecraft to start a fresh world");
            if !prism::stop_pid(p.pid, 30) { return Err(fail(UserError::new("Minecraft would not close", &format!("Process {} ignored the request to quit. Close it yourself and press RETRY.", p.pid)).at(id))); }
            procs.clear();
        }
        match backup_world(l) {
            Ok(Some(b)) => {
                r.note(id, &format!("your old Minecraft world is kept in {}", b.display()));
                world_restore = Some(WorldRestore { world: l.world_dir(), backup: b, armed: true });
            }
            Ok(None) => {}
            Err(e) => return Err(fail(io_err("putting the old Minecraft world aside", e).at(id))),
        }
    }
    let mut mc: Option<McProc> = None;
    if let Some(p) = procs.first().cloned() {
        let st = cx.state();
        let ours = st.mc_pid == p.pid && st.mc_starttime == p.starttime;
        let mismatch = prism::proc_matches_settings(l, s, &p).err();
        let healthy = mismatch.is_none() && mc_progressing(p.pid);
        if healthy {
            r.step(id, StepState::Done, &format!("reusing the running Minecraft (pid {})", p.pid));
            mc = Some(p);
        } else if attach.is_some() {
            return Err(fail(UserError::new("Minecraft is not healthy", "Portal 2 is running but its Minecraft is frozen or was started with other settings. Close both games and press PLAY again.").hint(Hint::Logs).at(id)));
        } else if ours || mismatch.is_some() && ours {
            let why = mismatch.map(|t| format!("it was started with different settings ({t})")).unwrap_or_else(|| "it is frozen".into());
            r.note(id, &format!("restarting the Minecraft this launcher started: {why}"));
            if !prism::stop_pid(p.pid, 30) { return Err(fail(UserError::new("Minecraft would not close", &format!("Process {} ignored the request to quit. Close it yourself (kill {}) and press RETRY.", p.pid, p.pid)).at(id))); }
        } else {
            return Err(fail(UserError::new("A Minecraft is already running", &format!("A Portalcraft Minecraft (process {}) is running with other settings or is stuck, and this launcher did not start it, so it will not close it. Close it yourself, then press RETRY.", p.pid)).at(id)));
        }
    }
    if mc.is_none() {
        if let Err(e) = prism::write_instance_cfg_only(l, s) { return Err(fail(io_err("updating the Minecraft profile", e).at(id))); }
        r.note(id, "starting Prism, which starts Minecraft (the first time this takes a few minutes)");
        prism::launch_instance(l).map_err(|e| fail(UserError::new("Prism would not start", &format!("{e}. Press RETRY.")).hint(Hint::Logs).at(id)))?;
        let t0 = Instant::now();
        let p = loop {
            if let Some(p) = prism::minecraft_procs(l).into_iter().next() { break p; }
            if r.cancelled() { return Err(fail(stopped(id))); }
            if t0.elapsed() > Duration::from_secs(mc_start_wait_s(l)) {
                return Err(fail(UserError::new(
                    "Minecraft did not start",
                    &format!("Prism was asked to start Minecraft {MC_VERSION} but no game appeared after {} minutes. Open Prism ('Open Prism' in Settings) to see what it says; it may want you to sign in again or to confirm something. Then press RETRY.", mc_start_wait_s(l) / 60),
                ).hint(Hint::Prism).at(id)));
            }
            std::thread::sleep(Duration::from_secs(1));
        };
        let mut st = cx.state();
        st.mc_pid = p.pid;
        st.mc_starttime = p.starttime;
        st.save(l);
        r.step(id, StepState::Done, &format!("Minecraft started (pid {}), hidden", p.pid));
        mc = Some(p);
    }
    let mc = mc.unwrap();
    if let Some(w) = world_restore.as_mut() { w.armed = false; } // Minecraft is up: the fresh world exists now

    // ---------------------------------------------------------- Portal 2
    let id = StepId::PlayPortal2;
    let fail = |e: UserError| { r.step(e.step.unwrap_or(id), StepState::Failed, &e.title); e };
    let (p2pid, log_start) = if let Some((pid, line)) = attach {
        r.step(id, StepState::Skipped, &format!("already running (pid {pid})"));
        (pid, line)
    } else {
        r.step(id, StepState::Running, "writing the temporary autoexec.cfg");
        let so = l.plugin_so();
        let text = autoexec::lines(&Script { so: &so, start_as_mc: opts.start_as_mc, unified_world: autoexec::plugin_knows_world(&so), map: &map, load: opts.load.as_deref() });
        let log_start = log_len(&l.bridge_log());
        let mut guard = Guard::install(&p2, &text).map_err(|e| fail(match e {
            InstallError::UpdateCfgInTheWay(p) => UserError::new("A leftover Portal 2 file is in the way", &format!("{} exists and would stop Portalcraft from loading its plugin. If you did not put it there on purpose, delete it; then press RETRY.", p.display())).at(id),
            InstallError::Io(m) => UserError::new("Could not write into Portal 2's folder", &format!("{m}\n{}", if cfg!(windows) { "Portal 2's folder looks read-only or full, or a program (an antivirus, for example) is holding it. Check free space and permissions of that folder, then press RETRY." } else { "Portal 2's folder looks read-only or full. If Portal 2 is on a Windows (NTFS) drive, Windows may have left it locked by 'Fast startup' or hibernation: shut Windows down fully (or run 'shutdown /s /t 0'), boot Linux, and try again. Otherwise check free space and permissions of that folder, then press RETRY." })).hint(Hint::Logs).at(id),
        }))?;
        log(&format!("autoexec written for map {map}"));
        r.note(id, "starting Portal 2 through Steam (if Steam asks you to sign in or shows a pop-up, handle that there)");
        let Some(mut cmd) = steam_launch_command() else {
            guard.remove();
            return Err(fail(UserError::new("Steam could not be started", "Portalcraft found no way to start Steam or open steam:// links. Start Steam yourself, then press RETRY.").hint(Hint::Steam).at(id)));
        };
        cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
        if let Err(e) = cmd.spawn() {
            guard.remove();
            return Err(fail(UserError::new("Steam could not be started", &format!("{e}. Start Steam yourself, then press RETRY.")).hint(Hint::Steam).at(id)));
        }
        let t0 = Instant::now();
        let pid = loop {
            if let Some(p) = portal2_pids().first() { break *p; }
            if r.cancelled() { guard.remove(); return Err(fail(stopped(id))); }
            if t0.elapsed() > Duration::from_secs(240) {
                guard.remove();
                return Err(fail(UserError::new("Portal 2 did not start", "Steam did not start Portal 2 within 4 minutes. A Steam pop-up (an update, a login, a 'launch options' question) is the usual reason: click it away in Steam, then press RETRY.").hint(Hint::Steam).at(id)));
            }
            std::thread::sleep(Duration::from_secs(1));
        };
        r.note(id, &format!("Portal 2 is running (pid {pid}); waiting for the level to load"));
        let t1 = Instant::now();
        let marker = format!("[bridge] level init {map}");
        let ok = loop {
            if log_since(&l.bridge_log(), log_start).contains(&marker) { break true; }
            if !util::pid_alive(pid) { break false; }
            if r.cancelled() { guard.remove(); return Err(fail(stopped(id))); }
            if t1.elapsed() > Duration::from_secs(180) { break false; }
            std::thread::sleep(Duration::from_secs(1));
        };
        guard.remove();
        log("temporary autoexec removed");
        if ok {
            if let Some(name) = opts.load.as_deref() {
                // the plugin stamps progress.txt a moment after logging 'level init'
                std::thread::sleep(Duration::from_secs(2));
                if let Some(t) = portal2::save_modified(&p2, name) {
                    if portal2::backdate_progress(&l.progress_file(), &map, t) { log("progress.txt: visit start moved back to the loaded save"); }
                }
            }
        }
        if !ok {
            return Err(fail(UserError::new(
                "Portal 2 started but the plugin did not load the level",
                &format!("The plugin never reported '{map}' within 3 minutes (or Portal 2 closed). Portal 2 may be sitting at its main menu. Close Portal 2 and press RETRY; if it happens again, send a bug report.{}", if cfg!(unix) && p2.to_string_lossy().contains("/.var/app/com.valvesoftware.Steam/") { "\nYou use the Flatpak version of Steam: its sandbox cannot see Portalcraft's folder, so it cannot load the plugin. Install Steam from your distribution's package (or the Steam website) instead and move Portal 2 into that Steam." } else { "" }),
            ).hint(Hint::Logs).at(id)));
        }
        r.step(id, StepState::Done, &format!("Portal 2 is running (pid {pid}), level {map} loaded"));
        (pid, log_start)
    };

    // ---------------------------------------------------------- link
    let id = StepId::PlayLink;
    let fail = |e: UserError| { r.step(e.step.unwrap_or(id), StepState::Failed, &e.title); e };
    r.step(id, StepState::Running, "waiting for the two games to find each other");
    let t0 = Instant::now();
    let need_mode = opts.start_as_mc;
    let need_cliff = opts.kind == PlayKind::PitFlings;
    loop {
        let log_text = log_since(&l.bridge_log(), log_start);
        let linked = log_text.contains("[bridge] link up (Minecraft pid");
        let mode = !need_mode || log_text.contains("[bridge] mode: Minecraft player at");
        let cliff = !need_cliff || log_text.contains("[bridge] clip: cliff exit (");
        if linked && mode && cliff { break; }
        if !util::pid_alive(p2pid) { return Err(fail(UserError::new("Portal 2 closed", "Portal 2 exited before the games linked up. Press PLAY to try again.").hint(Hint::Logs).at(id))); }
        if !util::pid_alive(mc.pid) { return Err(fail(UserError::new("Minecraft closed", "Minecraft exited before the games linked up. Its log is in the bug report. Press PLAY to try again.").hint(Hint::Logs).at(id))); }
        if r.cancelled() { return Err(fail(stopped(id))); }
        if t0.elapsed() > Duration::from_secs(LINK_WAIT_S) {
            return Err(fail(UserError::new(
                "The two games did not link up",
                &format!("Minecraft and Portal 2 did not find each other within {} minutes (state: link {}). Nothing was closed. The very first start creates your Minecraft world and can be slow: press RETRY to keep waiting. If it fails again, {}, then send a bug report.", LINK_WAIT_S / 60, if linked { "up" } else { "down" }, if cfg!(windows) { "restart your computer" } else { "restart Steam (it can leak shared memory)" }),
            ).hint(Hint::Logs).at(id)));
        }
        r.note(id, &format!("waiting ({}s): link {}", t0.elapsed().as_secs(), if linked { "up" } else { "not yet" }));
        std::thread::sleep(Duration::from_secs(2));
    }
    r.step(id, StepState::Done, "linked");
    Ok(())
}

pub fn keys_help() -> &'static [(&'static str, &'static str)] {
    &[
        ("J", "switch between the Minecraft player and Chell"),
        ("G", "hold while Chell fires a portal: its partner opens onto the Minecraft world. As the Minecraft player, G uses things (buttons, cubes)"),
        ("H", "hold half a second in Minecraft: back to your last spot in the chamber"),
        ("E", "Minecraft inventory (Esc closes it)"),
        ("1-9, wheel", "hotbar"),
        ("F5", "third-person view"),
        ("F8", "mark a bug: saves a note and a replay (the bug report button includes it)"),
        ("` (backtick)", "Portal 2's console"),
    ]
}

pub fn _unused(_: PathBuf) {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps() {
        let o = PlayOpts { kind: PlayKind::Campaign, map: None, start_as_mc: true, load: None, reset_world: false };
        assert_eq!(map_for(&o, ""), "sp_a1_intro1");
        assert_eq!(map_for(&o, "sp_a2_laser_intro"), "sp_a2_laser_intro");
        let p = PlayOpts { kind: PlayKind::PitFlings, map: Some("x".into()), start_as_mc: false, load: None, reset_world: false };
        assert_eq!(map_for(&p, ""), "pc_pit_flings");
        assert!(valid_map_name("sp_a2_bridge_intro"));
        assert!(!valid_map_name("a;quit"));
        assert!(!valid_map_name("a b"));
    }

    #[test]
    fn attach_detection() {
        let t = std::env::temp_dir().join(format!("pc-bl-{}.log", std::process::id()));
        std::fs::write(&t, "old\n[bridge] link: /dev/shm/Portalcraft_v1 created, 220 MB, host pid 111\n[bridge] unloading\n[bridge] link: /dev/shm/Portalcraft_v1 created, 220 MB, host pid 222\n[bridge] x\n").unwrap();
        let txt = std::fs::read_to_string(&t).unwrap();
        assert_eq!(attach_start(&t, 222), Some(txt.find("[bridge] link: /dev/shm/Portalcraft_v1 created, 220 MB, host pid 222").unwrap() as u64));
        assert_eq!(attach_start(&t, 111), None);
        let _ = std::fs::remove_file(&t);
    }
}

pub const PLAY_STEPS_LIST: [StepId; 4] = PLAY_STEPS;
