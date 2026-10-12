//! Steps, progress reporting and plain-English errors shared by every task (setup, play, repair), plus the setup steps.
use crate::manifest::{self, Asset, FetchError, Manifest};
use crate::paths::Layout;
use crate::portal2::{self, Portal2Error};
use crate::prism;
use crate::settings::{Settings, State};
use crate::util::{self, human_bytes, log, NetError};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum StepId {
    Portal2, Shm, Manifest, Prism, Plugin, Instance, SignIn, Minecraft, Textures,
    PlayPrepare, PlayMinecraft, PlayPortal2, PlayLink,
    Maps, Verify,
}

impl StepId {
    pub fn title(self) -> &'static str {
        match self {
            StepId::Portal2 => "Find Portal 2",
            StepId::Shm => "Check shared memory (/dev/shm)",
            StepId::Manifest => "Check for the newest version",
            StepId::Prism => "Download Prism Launcher",
            StepId::Plugin => "Install the Portal 2 plugin",
            StepId::Instance => "Set up the Minecraft profile",
            StepId::SignIn => "Sign in with your Microsoft account",
            StepId::Minecraft => "Let Prism download Minecraft",
            StepId::Textures => "Unpack Minecraft's textures",
            StepId::PlayPrepare => "Get ready",
            StepId::PlayMinecraft => "Start Minecraft (hidden)",
            StepId::PlayPortal2 => "Start Portal 2",
            StepId::PlayLink => "Link the two games",
            StepId::Maps => "Build the pit_flings demo map",
            StepId::Verify => "Check the installed files",
        }
    }
}

/// Windows has no /dev/shm to check, so its setup has one step less.
#[cfg(unix)]
pub const SETUP_STEPS: &[StepId] = &[
    StepId::Portal2, StepId::Shm, StepId::Manifest, StepId::Prism, StepId::Plugin, StepId::Instance, StepId::SignIn, StepId::Minecraft, StepId::Textures,
];
#[cfg(windows)]
pub const SETUP_STEPS: &[StepId] = &[
    StepId::Portal2, StepId::Manifest, StepId::Prism, StepId::Plugin, StepId::Instance, StepId::SignIn, StepId::Minecraft, StepId::Textures,
];
pub const PLAY_STEPS: [StepId; 4] = [StepId::PlayPrepare, StepId::PlayMinecraft, StepId::PlayPortal2, StepId::PlayLink];

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum StepState { Pending, Running, Waiting, Done, Skipped, Failed }

#[derive(Clone, Debug)]
pub enum Prompt {
    /// Waiting for the player to sign in inside Prism.
    SignIn,
    /// Prism is downloading Minecraft; nothing to do.
    PrismWorking,
    None,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Hint { None, Settings, Prism, Logs, Steam, Setup }

#[derive(Clone, Debug)]
pub struct UserError {
    pub step: Option<StepId>,
    pub title: String,
    pub body: String,
    pub hint: Hint,
}

impl UserError {
    pub fn new(title: &str, body: &str) -> Self { UserError { step: None, title: title.into(), body: body.into(), hint: Hint::None } }
    pub fn hint(mut self, h: Hint) -> Self { self.hint = h; self }
    pub fn at(mut self, s: StepId) -> Self { self.step = Some(s); self }
}

pub type R<T> = Result<T, UserError>;

#[derive(Clone, Debug)]
pub enum Event {
    Step { id: StepId, state: StepState, detail: String },
    Progress { id: StepId, done: u64, total: u64 },
    Log(String),
    Prompt(Prompt),
}

#[derive(Clone)]
pub struct Reporter {
    sink: Arc<dyn Fn(Event) + Send + Sync>,
    pub cancel: Arc<AtomicBool>,
}

impl Reporter {
    pub fn new(sink: Arc<dyn Fn(Event) + Send + Sync>, cancel: Arc<AtomicBool>) -> Self { Reporter { sink, cancel } }
    pub fn emit(&self, e: Event) { (self.sink)(e) }
    pub fn step(&self, id: StepId, state: StepState, detail: &str) {
        log(&format!("step {:?} {:?} {}", id, state, detail));
        self.emit(Event::Step { id, state, detail: detail.into() });
    }
    pub fn note(&self, id: StepId, detail: &str) { self.emit(Event::Step { id, state: StepState::Running, detail: detail.into() }) }
    pub fn progress(&self, id: StepId, done: u64, total: u64) { self.emit(Event::Progress { id, done, total }) }
    pub fn line(&self, s: &str) { log(s); self.emit(Event::Log(s.into())) }
    pub fn cancelled(&self) -> bool { self.cancel.load(Ordering::Relaxed) }
}

pub struct Ctx {
    pub layout: Layout,
    pub settings: Settings,
    pub rep: Reporter,
}

impl Ctx {
    pub fn state(&self) -> State { State::load(&self.layout) }
}

pub fn net_error(e: NetError, what: &str) -> UserError {
    match e {
        NetError::Offline(d) => UserError::new(
            "Can't reach the internet",
            &format!("Portalcraft could not download {what}. Check your internet connection and press RETRY.\nIf you only want to play with what is already installed, turn on Offline mode in Settings.\n(technical detail: {d})"),
        ).hint(Hint::Settings),
        NetError::Http(404, u) => UserError::new(
            "The download is not there",
            &format!("The server has no file for {what} ({u}, HTTP 404). This usually means a release is half uploaded. Wait a few minutes and press RETRY; if it keeps happening, report it with the bug report button."),
        ),
        NetError::Http(c, u) => UserError::new(
            "The download server had a problem",
            &format!("Downloading {what} failed (HTTP {c} from {u}). Try again in a minute: press RETRY."),
        ),
        NetError::Mismatch(d) => UserError::new(
            "A download was damaged",
            &format!("The file for {what} did not match its checksum ({d}) and was thrown away. Press RETRY to download it again."),
        ),
        NetError::Io(d) => UserError::new("Could not save a download", &format!("{what}: {d}. Check that your disk has free space and press RETRY.")),
    }
}

pub fn io_err(what: &str, e: impl std::fmt::Display) -> UserError {
    UserError::new("Could not write a file", &format!("{what}: {e}. Check free disk space and the permissions of the Portalcraft data folder, then press RETRY.")).hint(Hint::Logs)
}

// ---------------------------------------------------------------- state helpers

pub fn tag(a: &Asset) -> String { format!("{} {}", a.version, a.sha256) }

fn file_name_of(url: &str) -> String {
    url.rsplit('/').next().unwrap_or("download").split('?').next().unwrap_or("download").replace("%2B", "+")
}

/// The `files` folder next to the launcher in a release zip: the plugin, the mod, Prism, Fabric API and the release list,
/// so setup works out of the box (and without the internet for everything but Minecraft and Java). None when absent.
pub fn bundle_dir() -> Option<PathBuf> {
    let d = util::exe_path().ok()?.parent()?.join("files");
    d.join("launcher.json").is_file().then_some(d)
}

/// The bundled copy of an asset, if the release zip has one with the right checksum.
fn bundled(a: &Asset) -> Option<PathBuf> {
    let f = bundle_dir()?.join(file_name_of(&a.url));
    (!a.sha256.is_empty() && f.is_file() && util::sha256_file(&f).map(|h| h.eq_ignore_ascii_case(&a.sha256)).unwrap_or(false)).then_some(f)
}

/// Downloads an asset into downloads/ (verified), with progress shown on `id`. A matching file in the release zip's
/// `files` folder is copied instead of downloaded.
pub fn fetch_asset(cx: &Ctx, id: StepId, a: &Asset, what: &str) -> R<PathBuf> {
    let dest = cx.layout.downloads_dir().join(file_name_of(&a.url));
    if let Some(src) = bundled(a) {
        cx.rep.note(id, &format!("{what}: from the files that came with the launcher"));
        if let Some(p) = dest.parent() { std::fs::create_dir_all(p).map_err(|e| io_err("the downloads folder", e).at(id))?; }
        let part = PathBuf::from(format!("{}.part", dest.display()));
        std::fs::copy(&src, &part).and_then(|_| std::fs::rename(&part, &dest)).map_err(|e| io_err(what, e).at(id))?;
        cx.rep.progress(id, 1, 1);
        return Ok(dest);
    }
    let rep = cx.rep.clone();
    let mut last = Instant::now() - Duration::from_secs(1);
    let mut p = |d: u64, t: u64| {
        if last.elapsed() > Duration::from_millis(100) || d == t {
            last = Instant::now();
            rep.progress(id, d, t);
            rep.note(id, &format!("{what}: {} of {}", human_bytes(d), human_bytes(t)));
        }
    };
    util::download_verified(&a.url, &dest, &a.sha256, a.size, &cx.rep.cancel, &mut p).map_err(|e| net_error(e, what).at(id))?;
    Ok(dest)
}

// ---------------------------------------------------------------- setup steps

pub fn step_portal2(cx: &Ctx) -> R<PathBuf> {
    let r = &cx.rep;
    r.step(StepId::Portal2, StepState::Running, "looking through your Steam libraries");
    match portal2::find(&cx.settings.portal2_dir) {
        Ok(p) => {
            let flat = cfg!(unix) && p.to_string_lossy().contains("/.var/app/com.valvesoftware.Steam/");
            r.step(StepId::Portal2, StepState::Done, &format!("{}{}", p.display(), if flat { " (Flatpak Steam: its sandbox has a separate /dev/shm, so the two games may not be able to link; the normal Steam package is recommended)" } else { "" }));
            Ok(p)
        }
        Err(e) => {
            let ue = portal2_error(&e).at(StepId::Portal2);
            r.step(StepId::Portal2, StepState::Failed, &ue.title);
            Err(ue)
        }
    }
}

pub fn portal2_error(e: &Portal2Error) -> UserError {
    match e {
        Portal2Error::NoSteam => UserError::new(
            "Steam was not found",
            if cfg!(windows) {
                "Portalcraft asked Windows where Steam is installed (the registry entry Steam keeps) and also looked in C:\\Program Files (x86)\\Steam, and found nothing.\nInstall Steam, sign in, and install Portal 2 from your library. If Steam is somewhere unusual, set the Portal 2 folder in Settings. Then press RETRY."
            } else {
                "Portalcraft looked for Steam in ~/.steam, ~/.local/share/Steam and the Flatpak folder and found nothing.\nInstall Steam, sign in, and install Portal 2 from your library. If Steam is somewhere unusual, set the Portal 2 folder in Settings. Then press RETRY."
            },
        ).hint(Hint::Steam),
        Portal2Error::NotInstalled(libs) => UserError::new(
            "Portal 2 is not installed",
            &format!("Steam is there, but none of its libraries holds Portal 2 ({} looked at).\nOpen Steam, install Portal 2 (you must own it), let it finish, then press RETRY. If it is on a drive that is not mounted, mount it first. If Steam itself has forgotten that drive (it shows Portal 2 as not installed), add the library back in Steam: Settings, Storage, Add Drive. Do not use Portal RTX.", libs.len()),
        ).hint(Hint::Steam),
        Portal2Error::BadOverride(p) => UserError::new(
            "That Portal 2 folder is not right",
            &format!("The Portal 2 folder in Settings ({}) has no 'portal2' folder inside it. In Steam, right-click Portal 2, choose Manage, then Browse local files, and use that folder.", p.display()),
        ).hint(Hint::Settings),
        Portal2Error::NoProgram(p) if cfg!(windows) => UserError::new(
            "Portal 2's program is missing",
            &format!("Portal 2's files are at {} but 'portal2.exe' is missing. In Steam, right-click Portal 2, choose Properties, Installed Files, 'Verify integrity of game files', let it finish, then press RETRY.", p.display()),
        ).hint(Hint::Steam),
        Portal2Error::NoProgram(p) => UserError::new(
            "Portal 2 has no Linux program",
            &format!("Portal 2's files are at {} but 'portal2_linux' is missing. Portalcraft needs the native Linux version: in Steam open Portal 2's Properties, Compatibility, make sure 'Force the use of a specific Steam Play compatibility tool' is OFF, let it re-download, then press RETRY.", p.display()),
        ).hint(Hint::Steam),
    }
}

pub const SHM_NEED: u64 = 300 * 1024 * 1024;

pub fn step_shm(cx: &Ctx) -> R<()> {
    let r = &cx.rep;
    if cfg!(windows) { return Ok(()); } // not a step on Windows (SETUP_STEPS has none)
    r.step(StepId::Shm, StepState::Running, "");
    match util::shm_free_bytes() {
        Some(f) if f >= SHM_NEED => {
            r.step(StepId::Shm, StepState::Done, &format!("{} free", human_bytes(f)));
            Ok(())
        }
        Some(f) => {
            let ue = shm_full_error(f).at(StepId::Shm);
            r.step(StepId::Shm, StepState::Failed, &ue.title);
            Err(ue)
        }
        None => {
            r.step(StepId::Shm, StepState::Skipped, "could not read /dev/shm");
            Ok(())
        }
    }
}

pub fn shm_full_error(free: u64) -> UserError {
    UserError::new(
        "Shared memory is full",
        &format!("The two games talk through /dev/shm and it has only {} free (about 300 MB are needed). Steam sometimes leaks shared memory over a long session.\nFully quit Steam (Steam menu, Exit), start it again, then press RETRY. Restarting your computer also clears it.", human_bytes(free)),
    ).hint(Hint::Steam)
}

/// The manifest: fresh from the server (online), else the saved copy.
pub fn step_manifest(cx: &Ctx) -> R<Manifest> {
    let r = &cx.rep;
    r.step(StepId::Manifest, StepState::Running, "");
    if cx.settings.offline {
        return match manifest::cached(&cx.layout) {
            Some(m) => {
                r.step(StepId::Manifest, StepState::Skipped, "Offline mode: using the saved list");
                Ok(m)
            }
            None if manifest::bundled().is_some() => {
                r.step(StepId::Manifest, StepState::Skipped, "Offline mode: using the files that came with the launcher");
                Ok(manifest::bundled().unwrap())
            }
            None => {
                let ue = UserError::new("Offline mode is on", "Portalcraft has never downloaded its file list, so it cannot set up without the internet. Turn Offline mode off in Settings, then press RETRY.").hint(Hint::Settings).at(StepId::Manifest);
                r.step(StepId::Manifest, StepState::Failed, &ue.title);
                Err(ue)
            }
        };
    }
    match manifest::fetch(&cx.layout, &cx.settings.manifest_url()) {
        Ok(m) => {
            r.step(StepId::Manifest, StepState::Done, &format!("release {}", if m.release.is_empty() { &m.r#mod.version } else { &m.release }));
            Ok(m)
        }
        Err(e) => {
            if let Some(m) = manifest::cached(&cx.layout) {
                r.step(StepId::Manifest, StepState::Skipped, "could not reach the server; using the saved list");
                return Ok(m);
            }
            if let Some(m) = manifest::bundled() {
                r.step(StepId::Manifest, StepState::Skipped, "could not reach the server; using the files that came with the launcher");
                return Ok(m);
            }
            let ue = match e {
                FetchError::Net(n) => net_error(n, "the list of files (launcher.json)"),
                FetchError::Bad(s) => UserError::new("The file list is not readable", &format!("{s}\nPress RETRY in a minute; if it keeps happening please report it.")),
                FetchError::NeedsNewerLauncher(a) => UserError::new("This release needs a newer launcher", &format!("Portalcraft {} is out and this launcher is too old to install it. Use 'Update the launcher' on the home screen, then start it again.", a.version)),
            }.at(StepId::Manifest);
            r.step(StepId::Manifest, StepState::Failed, &ue.title);
            Err(ue)
        }
    }
}

fn installed_ok(cx: &Ctx, key: &str, a: &Asset, exists: bool) -> bool {
    exists && cx.state().installed.get(key).map(|s| *s == tag(a)).unwrap_or(false)
}

fn mark_installed(cx: &Ctx, key: &str, a: &Asset) {
    let mut s = cx.state();
    s.installed.insert(key.into(), tag(a));
    s.save(&cx.layout);
}

fn need_space(cx: &Ctx, id: StepId, need: u64, what: &str) -> R<()> {
    match util::free_bytes(&cx.layout.data) {
        Some(f) if f < need => {
            let ue = UserError::new("Not enough free disk space", &format!("{what} needs about {} free where Portalcraft keeps its files ({}), and only {} is free. Free some space (or move the folder: set PORTALCRAFT_DATA), then press RETRY.", human_bytes(need), cx.layout.data.display(), human_bytes(f))).at(id);
            cx.rep.step(id, StepState::Failed, &ue.title);
            Err(ue)
        }
        _ => Ok(()),
    }
}

pub fn step_prism(cx: &Ctx, m: &Manifest) -> R<()> {
    let r = &cx.rep;
    let id = StepId::Prism;
    let Some(prism_asset) = m.prism_asset() else {
        let ue = no_platform_error("Prism Launcher").at(id);
        r.step(id, StepState::Failed, &ue.title);
        return Err(ue);
    };
    if installed_ok(cx, "prism", prism_asset, cx.layout.prism_bin().exists() && !cx.layout.prism_dir().join(prism::UNPACKING_MARK).exists()) {
        r.step(id, StepState::Done, &format!("Prism {} is installed", prism_asset.version));
        return Ok(());
    }
    if cx.layout.prism_bin().exists() && prism::prism_running(&cx.layout) {
        let ue = UserError::new("Close Prism first", "A new version of Prism is ready, but the Prism window is still open and its program cannot be replaced while it runs. Close the Prism window, then press RETRY.").at(id);
        r.step(id, StepState::Failed, &ue.title);
        return Err(ue);
    }
    need_space(cx, id, 400 << 20, "Prism Launcher")?;
    r.step(id, StepState::Running, &format!("Prism Launcher {}", prism_asset.version));
    let file = fetch_asset(cx, id, prism_asset, "Prism Launcher").map_err(|e| { r.step(id, StepState::Failed, &e.title); e })?;
    r.step(id, StepState::Running, "unpacking");
    r.progress(id, 0, 0);
    // a launcher killed in the middle must not leave a half-unpacked Prism that looks installed
    let mark = cx.layout.prism_dir().join(prism::UNPACKING_MARK);
    let _ = std::fs::create_dir_all(cx.layout.prism_dir());
    let _ = std::fs::write(&mark, "unpacking\n");
    let n = prism::extract_prism(&file, &cx.layout.prism_dir()).map_err(|e| {
        let _ = std::fs::remove_file(&file);
        let ue = io_err("unpacking Prism", e).at(id);
        r.step(id, StepState::Failed, &ue.title);
        ue
    })?;
    let _ = std::fs::remove_file(&mark);
    prism::write_prism_defaults(&cx.layout);
    if !cx.layout.prism_bin().exists() {
        let ue = UserError::new("Prism did not unpack properly", "The Prism program is missing after unpacking. Press RETRY to download it again.").at(id);
        let _ = std::fs::remove_file(&file);
        r.step(id, StepState::Failed, &ue.title);
        return Err(ue);
    }
    mark_installed(cx, "prism", prism_asset);
    r.step(id, StepState::Done, &format!("Prism {} ({n} files)", prism_asset.version));
    Ok(())
}

/// The release list has no file of this kind for this operating system (an old release, or a half-made one).
fn no_platform_error(what: &str) -> UserError {
    UserError::new(
        "This release has no files for your system",
        &format!("The release list (launcher.json) has no {what} for {}. Releases made before Windows support do not. Wait for a newer release, or press RETRY once one is out.", if cfg!(windows) { "Windows" } else { "Linux" }),
    )
}

/// Windows today: the release has no Portal 2 plugin for it. Setup does not fail on that (everything else is set up and
/// can be tested); Play says so.
pub const NO_PLUGIN_TITLE: &str = "The Portal 2 plugin for Windows is missing";

pub fn no_plugin_error() -> UserError {
    UserError::new(
        NO_PLUGIN_TITLE,
        "The release list this launcher read has no Windows version of the Portal 2 plugin, so Portal 2 and Minecraft cannot be linked. Usually the release is still being uploaded: wait a few minutes and press RETRY, or use Settings, \"Check and repair installed files\". Everything else is set up, nothing was started and nothing was changed in Portal 2. If it keeps happening, send a bug report.",
    )
}

pub fn step_plugin(cx: &Ctx, m: &Manifest) -> R<()> {
    let r = &cx.rep;
    let id = StepId::Plugin;
    let so = cx.layout.plugin_so();
    let Some(plugin) = m.plugin_asset() else {
        if cfg!(windows) {
            r.step(id, StepState::Skipped, "this release has no Windows plugin yet");
            return Ok(());
        }
        let ue = no_platform_error("Portal 2 plugin").at(id);
        r.step(id, StepState::Failed, &ue.title);
        return Err(ue);
    };
    if installed_ok(cx, "plugin", plugin, so.exists()) {
        r.step(id, StepState::Done, &format!("plugin {} is installed", plugin.version));
        return Ok(());
    }
    if !util::portal2_pids().is_empty() && so.exists() {
        let ue = UserError::new("Close Portal 2 first", "A new version of the plugin is ready, but Portal 2 is running and has the old one loaded. Close Portal 2, then press RETRY.").at(id);
        r.step(id, StepState::Failed, &ue.title);
        return Err(ue);
    }
    r.step(id, StepState::Running, &format!("plugin {}", plugin.version));
    let file = fetch_asset(cx, id, plugin, "the Portal 2 plugin").map_err(|e| { r.step(id, StepState::Failed, &e.title); e })?;
    let fail = |e: std::io::Error| { let ue = io_err("installing the plugin", e).at(id); r.step(id, StepState::Failed, &ue.title); ue };
    std::fs::create_dir_all(cx.layout.plugin_dir()).map_err(fail)?;
    // a new file next to the old one, then renamed over it: a running game keeps the old inode
    let tmp = cx.layout.plugin_dir().join(format!(".{}.new", crate::paths::PLUGIN_FILE));
    std::fs::copy(&file, &tmp).map_err(fail)?;
    std::fs::rename(&tmp, &so).map_err(fail)?;
    cx.layout.ensure_dirs().map_err(fail)?;
    mark_installed(cx, "plugin", plugin);
    r.step(id, StepState::Done, &format!("plugin {} installed", plugin.version));
    Ok(())
}

/// The built-in instance template, or the manifest's newer one.
fn instance_template(cx: &Ctx, m: &Manifest) -> (String, String) {
    if let Some(a) = &m.instance {
        if let Ok(f) = fetch_asset(cx, StepId::Instance, a, "the Minecraft profile template") {
            if let Ok(fh) = std::fs::File::open(&f) {
                let mut tar = tar::Archive::new(flate2::read::GzDecoder::new(fh));
                let (mut cfg, mut pack) = (None, None);
                if let Ok(es) = tar.entries() {
                    for e in es.flatten() {
                        let mut e = e;
                        let name = e.path().ok().map(|p| p.to_string_lossy().to_string()).unwrap_or_default();
                        let mut s = String::new();
                        use std::io::Read;
                        if name.ends_with("instance.cfg.in") && e.read_to_string(&mut s).is_ok() { cfg = Some(s); }
                        else if name.ends_with("mmc-pack.json") && e.read_to_string(&mut s).is_ok() { pack = Some(s); }
                    }
                }
                if let (Some(c), Some(p)) = (cfg, pack) {
                    return (c, p);
                }
            }
        }
        log("the manifest's instance template could not be used; using the built-in one");
    }
    (prism::TEMPLATE_CFG.to_string(), prism::TEMPLATE_PACK.to_string())
}

pub fn step_instance(cx: &Ctx, m: &Manifest) -> R<()> {
    let r = &cx.rep;
    let id = StepId::Instance;
    r.step(id, StepState::Running, "");
    let modname = format!("portalcraft-{}.jar", m.r#mod.version);
    let apiname = format!("fabric-api-{}.jar", m.fabric_api.version);
    let mods = cx.layout.mods_dir();
    let need_mod = !(installed_ok(cx, "mod", &m.r#mod, mods.join(&modname).exists()));
    let need_api = !(installed_ok(cx, "fabric_api", &m.fabric_api, mods.join(&apiname).exists()));
    let fail = |e: std::io::Error| { let ue = io_err("setting up the Minecraft profile", e).at(id); r.step(id, StepState::Failed, &ue.title); ue };
    std::fs::create_dir_all(&mods).map_err(fail)?;
    if need_mod || need_api {
        if !prism::minecraft_procs(&cx.layout).is_empty() {
            let ue = UserError::new("Close Minecraft first", "New mod files are ready but the hidden Minecraft is running with the old ones. It closes by itself a few seconds after Portal 2 exits; close Portal 2 (and wait), then press RETRY.").at(id);
            r.step(id, StepState::Failed, &ue.title);
            return Err(ue);
        }
    }
    let set = |a: &Asset, name: &str, prefix: &str, key: &str, what: &str, need: bool| -> R<()> {
        if !need { return Ok(()); }
        let f = fetch_asset(cx, id, a, what).map_err(|e| { r.step(id, StepState::Failed, &e.title); e })?;
        // old versions out (also anything else called like this component), then the new file in
        if let Ok(rd) = std::fs::read_dir(&mods) {
            for e in rd.flatten() {
                let n = e.file_name().to_string_lossy().to_string();
                if n.starts_with(prefix) && n.ends_with(".jar") && n != name { let _ = std::fs::remove_file(e.path()); }
            }
        }
        std::fs::copy(&f, mods.join(format!(".{name}.new"))).map_err(fail)?;
        std::fs::rename(mods.join(format!(".{name}.new")), mods.join(name)).map_err(fail)?;
        mark_installed(cx, key, a);
        Ok(())
    };
    set(&m.r#mod, &modname, "portalcraft-", "mod", "the Portalcraft Minecraft mod", need_mod)?;
    set(&m.fabric_api, &apiname, "fabric-api", "fabric_api", "Fabric API", need_api)?;
    // e4mc (a development-only convenience of old builds) must not be in the player's profile
    if let Ok(rd) = std::fs::read_dir(&mods) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().to_string();
            if n.starts_with("e4mc") && n.ends_with(".jar") { let _ = std::fs::remove_file(e.path()); }
        }
    }
    let (cfg, pack) = instance_template(cx, m);
    prism::write_instance(&cx.layout, &cx.settings, &cfg, &pack).map_err(fail)?;
    r.step(id, StepState::Done, &format!("Minecraft {}, Fabric, mod {}", crate::paths::MC_VERSION, m.r#mod.version));
    Ok(())
}

/// Waits for the player to sign in inside Prism (opens the Prism window on request from the UI).
pub fn step_signin(cx: &Ctx) -> R<()> {
    let r = &cx.rep;
    let id = StepId::SignIn;
    if prism::signed_in(&cx.layout) {
        prism::release_url_handler(&cx.layout);
        r.step(id, StepState::Done, "signed in");
        return Ok(());
    }
    r.step(id, StepState::Waiting, "waiting for you to sign in");
    r.emit(Event::Prompt(Prompt::SignIn));
    let t0 = Instant::now();
    loop {
        if prism::signed_in(&cx.layout) {
            r.emit(Event::Prompt(Prompt::None));
            prism::release_url_handler(&cx.layout);
            r.step(id, StepState::Done, "signed in");
            return Ok(());
        }
        if r.cancelled() {
            r.emit(Event::Prompt(Prompt::None));
            let ue = UserError::new("Sign-in not finished", "You can finish it later: press RETRY, then 'Open Prism to sign in'.").at(id);
            r.step(id, StepState::Failed, &ue.title);
            return Err(ue);
        }
        if t0.elapsed() > Duration::from_secs(60 * 60) {
            r.emit(Event::Prompt(Prompt::None));
            let ue = UserError::new("Still waiting for sign-in", "Nothing was signed in for an hour. Press RETRY and use 'Open Prism to sign in'.").at(id);
            r.step(id, StepState::Failed, &ue.title);
            return Err(ue);
        }
        std::thread::sleep(Duration::from_secs(2));
    }
}

fn dir_bytes(p: &Path, depth: u32) -> u64 {
    if depth == 0 { return 0; }
    let Ok(rd) = std::fs::read_dir(p) else { return 0 };
    rd.flatten().map(|e| {
        let p = e.path();
        match e.metadata() {
            Ok(md) if md.is_dir() => dir_bytes(&p, depth - 1),
            Ok(md) => md.len(),
            _ => 0,
        }
    }).sum()
}

/// Prism downloads Minecraft and Java the first time it starts the profile. We start it once (hidden Minecraft) and wait
/// for the 26.3 client jar, which the textures are unpacked from.
pub fn step_minecraft(cx: &Ctx) -> R<(PathBuf, bool)> {
    let r = &cx.rep;
    let id = StepId::Minecraft;
    if let Some(j) = prism::find_client_jar(&cx.layout) {
        r.step(id, StepState::Done, "Minecraft 26.3 is downloaded");
        return Ok((j, false));
    }
    need_space(cx, id, 3 << 30, "Minecraft and Java")?;
    r.step(id, StepState::Running, "starting Prism, which downloads Minecraft and Java");
    prism::launch_instance(&cx.layout).map_err(|e| {
        let ue = UserError::new("Prism would not start", &format!("{e}.\nPress RETRY. If it keeps failing, use the bug report button.")).hint(Hint::Logs).at(id);
        r.step(id, StepState::Failed, &ue.title);
        ue
    })?;
    r.emit(Event::Prompt(Prompt::PrismWorking));
    let t0 = Instant::now();
    let mut last_walk = Instant::now() - Duration::from_secs(10);
    let mut jar: Option<PathBuf> = None;
    let mut jar_at: Option<Instant> = None;
    loop {
        if jar.is_none() {
            jar = prism::find_client_jar(&cx.layout);
            if jar.is_some() { jar_at = Some(Instant::now()); }
        }
        // the jar is there; Prism has everything once the game itself has started (a hidden Minecraft, closed again below)
        if let Some(j) = &jar {
            if !prism::minecraft_procs(&cx.layout).is_empty() || jar_at.map(|t| t.elapsed() > Duration::from_secs(5 * 60)).unwrap_or(false) {
                r.emit(Event::Prompt(Prompt::None));
                r.step(id, StepState::Done, "Minecraft 26.3 downloaded");
                return Ok((j.clone(), true));
            }
        }
        if r.cancelled() || t0.elapsed() > Duration::from_secs(40 * 60) {
            r.emit(Event::Prompt(Prompt::None));
            let ue = if r.cancelled() {
                UserError::new("Stopped", "Press RETRY to continue the Minecraft download where Prism left off.")
            } else {
                UserError::new(
                    "Minecraft did not finish downloading",
                    "Prism has been working for 40 minutes without a complete Minecraft. Look at the Prism window (if it asked something, such as about Java, answer it there) and check your connection, then press RETRY.",
                ).hint(Hint::Prism)
            }.at(id);
            r.step(id, StepState::Failed, &ue.title);
            return Err(ue);
        }
        if last_walk.elapsed() > Duration::from_secs(3) {
            last_walk = Instant::now();
            let b = dir_bytes(&cx.layout.prism_dir().join("libraries"), 8) + dir_bytes(&cx.layout.prism_dir().join("assets"), 6) + dir_bytes(&cx.layout.prism_dir().join("java"), 6);
            r.note(id, &format!("Prism is downloading Minecraft and Java ({} so far, {} min). If Prism asks you something, answer it there.", human_bytes(b), t0.elapsed().as_secs() / 60));
        }
        std::thread::sleep(Duration::from_millis(1000));
    }
}

pub fn step_textures(cx: &Ctx, jar: Option<PathBuf>) -> R<()> {
    let r = &cx.rep;
    let id = StepId::Textures;
    if prism::textures_ready(&cx.layout) {
        r.step(id, StepState::Done, "already unpacked");
        return Ok(());
    }
    r.step(id, StepState::Running, "");
    let jar = jar.or_else(|| prism::find_client_jar(&cx.layout)).ok_or_else(|| {
        let ue = UserError::new("Minecraft's files are not there yet", "Prism has not finished downloading Minecraft 26.3. Press RETRY once the Minecraft step is done.").at(id);
        r.step(id, StepState::Failed, &ue.title);
        ue
    })?;
    let n = prism::extract_assets(&jar, &cx.layout.mc_assets_dir()).map_err(|e| {
        let ue = io_err("unpacking Minecraft's textures", e).at(id);
        r.step(id, StepState::Failed, &ue.title);
        ue
    })?;
    r.step(id, StepState::Done, &format!("{n} files from your own copy of Minecraft"));
    Ok(())
}

/// The Minecraft that setup started only to let Prism download everything: a hidden Minecraft with no Portal 2 renders for
/// nothing, so it is closed again. PLAY starts it when it is needed.
fn close_setup_minecraft(cx: &Ctx) {
    if !util::portal2_pids().is_empty() { return; }
    for p in prism::minecraft_procs(&cx.layout) {
        cx.rep.line(&format!("closing the Minecraft that setup started (pid {}); PLAY starts it again when needed", p.pid));
        prism::stop_pid(p.pid, 30);
    }
}

/// The whole first-run setup (also used to repair and to update: every step skips what is already right).
pub fn run_setup(cx: &Ctx) -> R<Manifest> {
    cx.layout.ensure_dirs().map_err(|e| io_err("creating the Portalcraft data folder", e))?;
    step_portal2(cx)?;
    step_shm(cx)?;
    let m = step_manifest(cx)?;
    step_prism(cx, &m)?;
    step_plugin(cx, &m)?;
    step_instance(cx, &m)?;
    step_signin(cx)?;
    let (jar, launched) = step_minecraft(cx)?;
    step_textures(cx, Some(jar))?;
    if launched { close_setup_minecraft(cx); }
    let mut s = cx.state();
    s.setup_done = true;
    // the app-menu entry (and with it the taskbar icon on Wayland): once, the first time setup finishes
    #[cfg(unix)]
    if !s.shortcut_made { s.save(&cx.layout); crate::menu::add_menu_entry(&cx.layout); s = cx.state(); s.setup_done = true; }
    s.save(&cx.layout);
    // old versions and anything half-downloaded do not stay behind in downloads/
    let mut keep: Vec<String> = [m.launcher_asset(), m.plugin_asset(), Some(&m.r#mod), Some(&m.fabric_api), m.prism_asset()].into_iter().flatten().map(|a| file_name_of(&a.url)).collect();
    keep.extend(m.instance.iter().chain(m.map_tools.iter()).map(|a| file_name_of(&a.url)));
    let gone = util::clean_leftovers(&cx.layout, Some(&keep));
    if !gone.is_empty() { log(&format!("removed old downloads: {}", gone.len())); }
    Ok(m)
}

/// Is everything in place for Play (no network needed)?
pub fn installed_report(l: &Layout) -> Vec<(&'static str, bool)> {
    vec![
        ("Prism", l.prism_bin().exists() && !l.prism_dir().join(prism::UNPACKING_MARK).exists()),
        // Windows without a Windows plugin in the release: nothing to install, Play explains (see plugin_missing)
        ("Plugin", l.plugin_so().exists() || (cfg!(windows) && !plugin_in_release(l))),
        ("Minecraft profile", l.instance_dir().join("instance.cfg").exists() && l.mods_dir().is_dir()),
        ("Signed in", prism::signed_in(l)),
        ("Minecraft textures", prism::textures_ready(l)),
    ]
}

/// Does the release the launcher knows about (the saved release list) carry a plugin for this system?
pub fn plugin_in_release(l: &Layout) -> bool {
    manifest::cached(l).map(|m| m.plugin_asset().is_some()).unwrap_or(true)
}

/// Windows: no plugin installed and none in the release.
pub fn plugin_missing(l: &Layout) -> bool { cfg!(windows) && !l.plugin_so().exists() && !plugin_in_release(l) }

pub fn fully_installed(l: &Layout) -> bool { installed_report(l).iter().all(|(_, ok)| *ok) }

pub struct Updates {
    pub launcher: Option<String>,
    pub components: Vec<&'static str>,
}

pub fn check_updates(l: &Layout, m: &Manifest) -> Updates {
    let st = State::load(l);
    let mut components = Vec::new();
    for (key, a, label) in [("prism", m.prism_asset(), "Prism"), ("plugin", m.plugin_asset(), "plugin"), ("mod", Some(&m.r#mod), "Minecraft mod"), ("fabric_api", Some(&m.fabric_api), "Fabric API")] {
        let Some(a) = a else { continue };
        if st.installed.get(key).map(|s| *s != tag(a)).unwrap_or(false) { components.push(label); }
    }
    Updates { launcher: m.launcher_update_available().then(|| m.launcher_asset().map(|a| a.version.clone())).flatten(), components }
}

/// Re-hashes the installed single files against the manifest and removes what does not match so setup fetches it again.
pub fn verify_files(cx: &Ctx, m: &Manifest) -> R<Vec<String>> {
    let r = &cx.rep;
    r.step(StepId::Verify, StepState::Running, "");
    let mut bad = Vec::new();
    let mut st = cx.state();
    let mods = cx.layout.mods_dir();
    let mut checks: Vec<(&str, PathBuf, &Asset, &str)> = vec![
        ("mod", mods.join(format!("portalcraft-{}.jar", m.r#mod.version)), &m.r#mod, "Minecraft mod"),
        ("fabric_api", mods.join(format!("fabric-api-{}.jar", m.fabric_api.version)), &m.fabric_api, "Fabric API"),
    ];
    if let Some(p) = m.plugin_asset() { checks.insert(0, ("plugin", cx.layout.plugin_so(), p, "Portal 2 plugin")); }
    for (key, path, a, label) in checks {
        r.note(StepId::Verify, &format!("checking {label}"));
        match util::sha256_file(&path) {
            Ok(h) if h.eq_ignore_ascii_case(&a.sha256) => {}
            Ok(_) | Err(_) => {
                bad.push(label.to_string());
                let _ = std::fs::remove_file(&path);
                st.installed.remove(key);
            }
        }
    }
    if !cx.layout.prism_bin().exists() { bad.push("Prism".into()); st.installed.remove("prism"); }
    if !prism::textures_ready(&cx.layout) { bad.push("Minecraft textures".into()); }
    st.save(&cx.layout);
    if bad.is_empty() {
        r.step(StepId::Verify, StepState::Done, "all files match");
    } else {
        r.step(StepId::Verify, StepState::Done, &format!("repairing: {}", bad.join(", ")));
    }
    Ok(bad)
}
