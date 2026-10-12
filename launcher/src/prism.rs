//! Prism Launcher: install the pinned portable build, write the Portalcraft instance, check the sign-in, start it.
//! Prism (not this launcher) signs the player in with their Microsoft account and downloads Minecraft and Java.
use crate::paths::{Layout, INSTANCE_ID};
use crate::settings::Settings;
use crate::util::{self, list_procs};
use std::fs;
use std::io::Read;
#[cfg(unix)]
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

pub const TEMPLATE_CFG: &str = include_str!("../prism/instance/instance.cfg.in");
pub const TEMPLATE_PACK: &str = include_str!("../prism/instance/mmc-pack.json");
pub const PRISM_DEFAULTS: &str = include_str!("../prism/prismlauncher.cfg");

/// Unpacks a Prism zip (the Windows portable build) into `dest`, overwriting program files and leaving Prism's own data
/// alone. Entries that would leave `dest` are refused.
pub fn extract_zip(archive: &Path, dest: &Path) -> Result<usize, String> {
    let f = fs::File::open(archive).map_err(|e| e.to_string())?;
    let mut z = zip::ZipArchive::new(f).map_err(|e| e.to_string())?;
    fs::create_dir_all(dest).map_err(|e| e.to_string())?;
    let mut n = 0;
    for i in 0..z.len() {
        let mut e = z.by_index(i).map_err(|e| e.to_string())?;
        let Some(rel) = e.enclosed_name() else { return Err(format!("{}: unsafe path in the archive", e.name())) };
        let out = dest.join(rel);
        if e.is_dir() {
            fs::create_dir_all(&out).map_err(|er| format!("{}: {er}", out.display()))?;
            continue;
        }
        if let Some(p) = out.parent() { fs::create_dir_all(p).map_err(|er| format!("{}: {er}", p.display()))?; }
        let mut o = fs::File::create(&out).map_err(|er| format!("{}: {er}", out.display()))?;
        std::io::copy(&mut e, &mut o).map_err(|er| format!("{}: {er}", out.display()))?;
        n += 1;
    }
    Ok(n)
}

/// Unpacks a Prism archive (tar.gz on Linux, zip on Windows) into `dest`.
pub fn extract_prism(archive: &Path, dest: &Path) -> Result<usize, String> {
    if archive.extension().map(|e| e.eq_ignore_ascii_case("zip")).unwrap_or(false) { return extract_zip(archive, dest); }
    let f = fs::File::open(archive).map_err(|e| e.to_string())?;
    let mut tar = tar::Archive::new(flate2::read::GzDecoder::new(f));
    fs::create_dir_all(dest).map_err(|e| e.to_string())?;
    tar.set_preserve_permissions(true);
    let mut n = 0;
    for entry in tar.entries().map_err(|e| e.to_string())? {
        let mut e = entry.map_err(|e| e.to_string())?;
        // unpack_in refuses entries that would leave `dest`
        e.unpack_in(dest).map_err(|er| format!("{}: {er}", e.path().map(|p| p.display().to_string()).unwrap_or_default()))?;
        n += 1;
    }
    Ok(n)
}

pub fn write_prism_defaults(l: &Layout) {
    let p = l.prism_dir().join("prismlauncher.cfg");
    if !p.exists() {
        let _ = fs::write(p, PRISM_DEFAULTS);
    }
    disable_prism_self_update(l);
}

/// Prism's portable builds update themselves (prismlauncher_update.cfg, `auto_check`). Portalcraft pins the Prism it
/// tested and updates it through its own release list, so Prism's updater is switched off every time we touch it.
pub fn disable_prism_self_update(l: &Layout) {
    let p = l.prism_dir().join("prismlauncher_update.cfg");
    let old = fs::read_to_string(&p).unwrap_or_default();
    let new = set_ini_key(&old, "General", "auto_check", "false");
    if new != old { let _ = fs::write(&p, new); }
}

/// Sets `key=value` in `[section]` of a Qt-style INI text, keeping everything else.
pub fn set_ini_key(text: &str, section: &str, key: &str, value: &str) -> String {
    let head = format!("[{section}]");
    let mut out: Vec<String> = Vec::new();
    let (mut in_sec, mut seen_sec, mut done) = (false, false, false);
    for line in text.lines() {
        let t = line.trim();
        if t.starts_with('[') {
            if in_sec && !done { out.push(format!("{key}={value}")); done = true; }
            in_sec = t == head;
            seen_sec |= in_sec;
        } else if in_sec && t.split('=').next().map(str::trim) == Some(key) {
            if !done { out.push(format!("{key}={value}")); done = true; }
            continue;
        }
        out.push(line.to_string());
    }
    if !done {
        if !seen_sec { out.push(head); }
        out.push(format!("{key}={value}"));
    }
    out.join("\n") + "\n"
}

/// The JVM arguments the mod needs (the same -D properties bridge/tools/run_mc.sh passes to Loom's runClient).
pub fn jvm_tokens(l: &Layout, s: &Settings) -> Vec<String> {
    let data = crate::paths::slashed(&l.data);
    let cache = crate::paths::slashed(&l.bridge_dir());
    vec![
        "--enable-native-access=ALL-UNNAMED".into(),
        "-Dskycraft.quitWithSkyrim=true".into(),
        "-Dskycraft.startHidden=true".into(),
        "-Dskycraft.world=terrain".into(),
        format!("-Dskycraft.seed={}", s.forced_seed().map(|v| v.to_string()).unwrap_or_default()),
        "-Dskycraft.worldName=".into(),
        format!("-Dskycraft.renderDistance={}", s.render_distance.clamp(6, 32)),
        "-Dskycraft.findCliff=0".into(),
        "-Dskycraft.minSectionY=3".into(),
        format!("-Dskycraft.audio={}", !s.mute),
        format!("-Dportalcraft.root={data}"),
        format!("-Dportalcraft.cacheDir={cache}"),
        format!("-Dportalcraft.nativeDir={cache}/mcnative"),
        "-Dportalcraft.mcFps=260".into(),
        "-Dportalcraft.gamemode=survival".into(),
    ]
}

fn jvm_line(tokens: &[String]) -> String {
    let args = tokens.iter().map(|t| if t.contains(' ') { format!("\"{t}\"") } else { t.clone() }).collect::<Vec<_>>().join(" ");
    if tokens.iter().any(|t| t.contains(' ')) {
        // a value with inner quotes must itself be quoted and escaped the way Prism's INI reader expects
        format!("\"{}\"", args.replace('\\', "\\\\").replace('"', "\\\""))
    } else {
        args
    }
}

/// Renders instance.cfg from the template text, keeping any other keys Prism has added to an existing file.
pub fn render_cfg(template: &str, jvm: &str, max_mem: u32, existing: Option<&str>) -> String {
    let fresh = template.replace("@JVM_ARGS@", jvm).replace("@MAX_MEM@", &max_mem.to_string());
    let Some(old) = existing else { return fresh };
    let ours: Vec<(String, String)> = fresh.lines().filter_map(|l| l.split_once('=')).map(|(k, v)| (k.to_string(), v.to_string())).collect();
    let mut out = Vec::new();
    let mut seen: Vec<String> = Vec::new();
    for line in old.lines() {
        if let Some((k, _)) = line.split_once('=') {
            if let Some((_, v)) = ours.iter().find(|(ok, _)| ok == k) {
                out.push(format!("{k}={v}"));
                seen.push(k.to_string());
                continue;
            }
        }
        out.push(line.to_string());
    }
    for (k, v) in &ours {
        if !seen.contains(k) {
            // new keys go right under [General]
            let at = out.iter().position(|l| l.trim() == "[General]").map(|i| i + 1).unwrap_or(0);
            out.insert(at, format!("{k}={v}"));
        }
    }
    out.join("\n") + "\n"
}

fn components_of(json: &str) -> Vec<(String, String)> {
    let Ok(v) = serde_json::from_str::<serde_json::Value>(json) else { return vec![] };
    let mut c: Vec<(String, String)> = v["components"].as_array().map(|a| {
        a.iter().filter_map(|x| Some((x["uid"].as_str()?.to_string(), x["version"].as_str()?.to_string()))).collect()
    }).unwrap_or_default();
    c.sort();
    c
}

/// Writes the instance (cfg + component list). `pack` is only replaced when its components differ.
pub fn write_instance(l: &Layout, s: &Settings, cfg_template: &str, pack: &str) -> std::io::Result<()> {
    let dir = l.instance_dir();
    fs::create_dir_all(l.mods_dir())?;
    let cfgp = dir.join("instance.cfg");
    let old = fs::read_to_string(&cfgp).ok();
    let cfg = render_cfg(cfg_template, &jvm_line(&jvm_tokens(l, s)), s.memory_mb.clamp(2048, 32768), old.as_deref());
    if old.as_deref() != Some(cfg.as_str()) {
        util::write_atomic(&cfgp, cfg.as_bytes())?;
    }
    let packp = dir.join("mmc-pack.json");
    let old_pack = fs::read_to_string(&packp).unwrap_or_default();
    if components_of(&old_pack) != components_of(pack) {
        util::write_atomic(&packp, pack.as_bytes())?;
    }
    Ok(())
}

/// Rewrites only instance.cfg (Java arguments and memory) from the current settings; used right before each start.
pub fn write_instance_cfg_only(l: &Layout, s: &Settings) -> std::io::Result<()> {
    let dir = l.instance_dir();
    fs::create_dir_all(l.mods_dir())?;
    let cfgp = dir.join("instance.cfg");
    let old = fs::read_to_string(&cfgp).ok();
    let cfg = render_cfg(TEMPLATE_CFG, &jvm_line(&jvm_tokens(l, s)), s.memory_mb.clamp(2048, 32768), old.as_deref());
    if old.as_deref() != Some(cfg.as_str()) {
        util::write_atomic(&cfgp, cfg.as_bytes())?;
    }
    Ok(())
}

/// True when Prism has a Microsoft account (any profile). Only the type and profile id are looked at; tokens are never read.
pub fn signed_in(l: &Layout) -> bool {
    if crate::settings::State::load(l).signin_override { return true; }
    let Ok(t) = fs::read_to_string(l.prism_accounts()) else { return false };
    let Ok(v) = serde_json::from_str::<serde_json::Value>(&t) else { return false };
    v["accounts"].as_array().map(|a| a.iter().any(|acc| {
        acc["type"].as_str().map(|t| t.eq_ignore_ascii_case("MSA")).unwrap_or(false)
            && (acc["profile"]["id"].as_str().map(|s| !s.is_empty()).unwrap_or(false) || acc["entitlement"]["ownsMinecraft"].as_bool() == Some(true))
    })).unwrap_or(false)
}

#[cfg(windows)]
fn spawn_detached(l: &Layout, args: &[&str]) -> Result<u32, String> {
    disable_prism_self_update(l);
    let bin = l.prism_bin();
    if !bin.exists() {
        return Err(format!("Prism is not installed ({} is missing)", bin.display()));
    }
    let log = fs::OpenOptions::new().create(true).append(true).open(l.prism_log()).map_err(|e| e.to_string())?;
    let log2 = log.try_clone().map_err(|e| e.to_string())?;
    let mut c = Command::new(&bin);
    c.args(args).current_dir(l.prism_dir()).stdin(Stdio::null()).stdout(log).stderr(log2);
    crate::winsys::detached(&mut c);
    c.spawn().map(|ch| ch.id()).map_err(|e| format!("could not start Prism: {e}"))
}

#[cfg(unix)]
fn spawn_detached(l: &Layout, args: &[&str]) -> Result<u32, String> {
    disable_prism_self_update(l);
    let bin = l.prism_bin();
    if !bin.exists() {
        return Err(format!("Prism is not installed ({} is missing)", bin.display()));
    }
    let log = fs::OpenOptions::new().create(true).append(true).open(l.prism_log()).map_err(|e| e.to_string())?;
    let log2 = log.try_clone().map_err(|e| e.to_string())?;
    let mut c = Command::new(&bin);
    c.args(args).current_dir(l.prism_dir()).stdin(Stdio::null()).stdout(log).stderr(log2);
    unsafe {
        c.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    c.spawn().map(|ch| ch.id()).map_err(|e| format!("could not start Prism: {e}"))
}

/// The Prism window (for signing in, or looking at the instance).
pub fn open_window(l: &Layout) -> Result<(), String> {
    util::log("starting the Prism window");
    if !signed_in(l) { claim_url_handler(l); }
    spawn_detached(l, &[]).map(|_| ())
}

#[cfg(unix)]
const URL_DESKTOP: &str = "portalcraft-prism.desktop";
#[cfg(unix)]
const URL_MIME: &str = "x-scheme-handler/prismlauncher";

#[cfg(unix)]
fn xdg_default() -> Option<String> {
    let o = Command::new("xdg-mime").args(["query", "default", URL_MIME]).output().ok()?;
    let s = String::from_utf8_lossy(&o.stdout).trim().to_string();
    if s.is_empty() { None } else { Some(s) }
}

/// A path as one argument of a .desktop Exec line: quoted, with the characters the Desktop Entry spec reserves escaped
/// (inside the quotes `"`, backtick, `$` and `\` take a backslash, and in the file each backslash is doubled again;
/// `%` is doubled so it is not read as a field code).
#[allow(dead_code)]
pub fn desktop_exec_arg(p: &str) -> String {
    let mut o = String::from("\"");
    for c in p.chars() {
        match c {
            '"' | '`' | '$' | '\\' => { o.push_str("\\\\"); o.push(c); }
            '%' => o.push_str("%%"),
            '\n' | '\r' => {}
            _ => o.push(c),
        }
    }
    o.push('"');
    o
}

/// The .desktop file that points prismlauncher:// at our Prism. TryExec makes desktops ignore it once the program is
/// gone (the data folder was deleted), so a forgotten handler never points at nothing.
#[allow(dead_code)]
pub fn url_desktop_text(bin: &str) -> String {
    format!("[Desktop Entry]\nType=Application\nName=Prism Launcher (Portalcraft)\nTryExec={}\nExec={} %U\nTerminal=false\nNoDisplay=true\nMimeType=x-scheme-handler/prismlauncher;\n", bin.replace(['\n', '\r'], ""), desktop_exec_arg(bin))
}

#[cfg(unix)]
fn apps_dir() -> Option<PathBuf> {
    let base = match std::env::var("XDG_DATA_HOME") {
        Ok(x) if !x.is_empty() => PathBuf::from(x),
        _ => PathBuf::from(std::env::var("HOME").ok()?).join(".local/share"),
    };
    Some(base.join("applications"))
}

/// Microsoft sign-in in Prism ends with the browser opening a prismlauncher:// link. A portable Prism registers no
/// handler for it, so without this the desktop asks "Choose application" and the sign-in never finishes. Points the
/// link at our Prism until sign-in is done; release_url_handler() puts back whatever was the default before.
#[cfg(windows)]
pub fn claim_url_handler(l: &Layout) {
    // Windows: HKCU\Software\Classes\prismlauncher -> our prismlauncher.exe "%1"; the previous entry is saved in state.json
    match crate::winsys::claim_url_handler(&l.prism_bin().to_string_lossy()) {
        Ok(Some(prev)) => {
            let mut st = crate::settings::State::load(l);
            if st.prev_url_handler.is_none() { st.prev_url_handler = Some(prev); st.save(l); }
            util::log("prismlauncher:// links now open our Prism (for the Microsoft sign-in)");
        }
        Ok(None) => {}
        Err(e) => util::log(&format!("could not register prismlauncher:// ({e}): the Microsoft sign-in may ask which program opens such links; use the code shown in Prism instead")),
    }
}

#[cfg(unix)]
pub fn claim_url_handler(l: &Layout) {
    let Some(dir) = apps_dir() else { return };
    let _ = fs::create_dir_all(&dir);
    if fs::write(dir.join(URL_DESKTOP), url_desktop_text(&l.prism_bin().to_string_lossy())).is_err() { return; }
    let prev = xdg_default();
    if prev.as_deref() != Some(URL_DESKTOP) {
        let ok = Command::new("xdg-mime").args(["default", URL_DESKTOP, URL_MIME]).status().map(|s| s.success()).unwrap_or(false);
        if !ok {
            util::log("xdg-mime is missing or failed: the Microsoft sign-in may ask which program opens prismlauncher:// links; choose Prism Launcher (Portalcraft)");
            return;
        }
        let mut st = crate::settings::State::load(l);
        if st.prev_url_handler.is_none() { st.prev_url_handler = Some(prev.unwrap_or_else(|| "-".into())); st.save(l); }
    }
    let _ = Command::new("update-desktop-database").arg(&dir).stdout(Stdio::null()).stderr(Stdio::null()).status();
    util::log("prismlauncher:// links now open our Prism (for the Microsoft sign-in)");
}

/// After sign-in: hands prismlauncher:// links back to the player's own Prism, if they had one. Safe to call any time
/// (it does nothing when nothing was borrowed): at the end of sign-in, when the launcher starts and when it exits, so a
/// launcher that was killed mid-sign-in does not keep the link forever.
pub fn release_url_handler(l: &Layout) {
    let mut st = crate::settings::State::load(l);
    let Some(prev) = st.prev_url_handler.clone() else { return };
    #[cfg(windows)]
    crate::winsys::restore_url_handler(&prev);
    #[cfg(unix)]
    {
        // the file is the link's record of an earlier player-owned default only if it is still the default
        if prev != "-" && prev != URL_DESKTOP {
            let _ = Command::new("xdg-mime").args(["default", &prev, URL_MIME]).status();
        }
    }
    st.prev_url_handler = None;
    st.save(l);
}

/// Is a Prism started from our data folder running (it holds its program file open: it cannot be replaced)?
#[cfg(windows)]
pub fn prism_running(l: &Layout) -> bool {
    let bin = l.prism_bin();
    list_procs().iter().any(|p| p.exe.as_ref().map(|e| e.to_string_lossy().eq_ignore_ascii_case(&bin.to_string_lossy())).unwrap_or(false))
}

#[cfg(unix)]
pub fn prism_running(l: &Layout) -> bool {
    let bin = fs::canonicalize(l.prism_bin()).unwrap_or_else(|_| l.prism_bin());
    list_procs().iter().any(|p| fs::read_link(format!("/proc/{}/exe", p.pid)).map(|e| e == bin).unwrap_or(false))
}

pub const UNPACKING_MARK: &str = ".portalcraft-unpacking";

/// Starts the Portalcraft instance (Minecraft). If Prism is already open, it hands the request over.
pub fn launch_instance(l: &Layout) -> Result<(), String> {
    util::log(&format!("prism --launch {INSTANCE_ID}"));
    spawn_detached(l, &["--launch", INSTANCE_ID]).map(|_| ())
}

#[derive(Debug, Clone)]
pub struct McProc {
    pub pid: u32,
    pub starttime: u64,
    pub cmdline: Vec<String>,
}

/// Minecraft processes started from this launcher's instance (they carry -Dportalcraft.root=<data folder>).
pub fn minecraft_procs(l: &Layout) -> Vec<McProc> {
    let tag = format!("-Dportalcraft.root={}", crate::paths::slashed(&l.data));
    let prism_dir = l.prism_dir();
    list_procs()
        .into_iter()
        .filter(|p| p.comm.to_ascii_lowercase().starts_with("java") || p.cmdline.first().map(|c| c.to_ascii_lowercase().contains("java")).unwrap_or(false))
        .filter(|p| p.cmdline.iter().any(|a| *a == tag) || unreadable_cmdline_in(p, &prism_dir))
        .map(|p| McProc { pid: p.pid, starttime: p.starttime, cmdline: p.cmdline })
        .collect()
}

/// Windows only: Windows does not let a program read the command line of every process (another session, protected
/// processes, some compatibility layers). Prism's own Java lives in <prism>/java, so a java process whose command line
/// cannot be read but whose program file is inside our Prism folder is ours too.
fn unreadable_cmdline_in(p: &util::Proc, prism_dir: &Path) -> bool {
    p.cmdline.is_empty() && p.exe.as_ref().map(|e| e.to_string_lossy().to_ascii_lowercase().starts_with(&prism_dir.to_string_lossy().to_ascii_lowercase())).unwrap_or(false)
}

/// Does a running Minecraft carry every argument the current settings would give it?
pub fn proc_matches_settings(l: &Layout, s: &Settings, p: &McProc) -> Result<(), String> {
    if p.cmdline.is_empty() { return Ok(()); } // command line unreadable (Windows): cannot tell, so do not restart it
    for t in jvm_tokens(l, s) {
        if t.starts_with("-Dskycraft.audio") || t.starts_with("-Dskycraft.renderDistance") || t.starts_with("-Dskycraft.seed") {
            if !p.cmdline.iter().any(|a| *a == t) {
                return Err(t);
            }
        }
    }
    Ok(())
}

#[cfg(windows)]
pub fn stop_pid(pid: u32, wait_s: u64) -> bool { crate::winsys::stop_pid(pid, wait_s) }

#[cfg(unix)]
pub fn stop_pid(pid: u32, wait_s: u64) -> bool {
    unsafe { libc::kill(pid as i32, libc::SIGTERM) };
    for _ in 0..wait_s {
        if !util::pid_alive(pid) { return true; }
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
    unsafe { libc::kill(pid as i32, libc::SIGKILL) };
    std::thread::sleep(std::time::Duration::from_secs(2));
    !util::pid_alive(pid)
}

// ---------------------------------------------------------------- Minecraft client jar

pub const PROBE: &str = "assets/minecraft/textures/block/dirt.png";
const PREFIX: &str = "assets/minecraft/";
const STAMP: &str = ".portalcraft-extracted";

pub fn is_client_jar(p: &Path) -> bool {
    fs::File::open(p).ok().and_then(|f| zip::ZipArchive::new(f).ok()).map(|mut z| z.by_name(PROBE).is_ok()).unwrap_or(false)
}

fn find_jars(l: &Layout) -> Vec<PathBuf> {
    let mut v = Vec::new();
    if let Ok(e) = std::env::var("MC_CLIENT_JAR") {
        v.push(PathBuf::from(e));
    }
    if let Ok(rd) = fs::read_dir(l.mc_client_jar_dir()) {
        let mut x: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|p| p.extension().map(|e| e == "jar").unwrap_or(false)).collect();
        x.sort();
        v.extend(x);
    }
    if v.is_empty() {
        // Prism changed its folder layout? look a few levels down for the client jar by name
        fn walk(d: &Path, depth: u32, out: &mut Vec<PathBuf>) {
            if depth == 0 { return; }
            let Ok(rd) = fs::read_dir(d) else { return };
            for e in rd.flatten() {
                let p = e.path();
                if p.is_dir() { walk(&p, depth - 1, out); }
                else if p.file_name().and_then(|n| n.to_str()).map(|n| n.contains("26.3") && n.contains("client") && n.ends_with(".jar")).unwrap_or(false) { out.push(p); }
            }
        }
        walk(&l.prism_dir().join("libraries"), 7, &mut v);
    }
    v.into_iter().filter(|p| p.is_file()).collect()
}

/// A complete 26.3 client jar, once Prism has downloaded it.
pub fn find_client_jar(l: &Layout) -> Option<PathBuf> {
    find_jars(l).into_iter().find(|p| is_client_jar(p))
}

pub fn textures_ready(l: &Layout) -> bool {
    let d = l.mc_assets_dir();
    d.join(STAMP).exists() && d.join("textures/block/dirt.png").exists()
}

/// Unpacks assets/minecraft/** from the player's own client jar into the folder the plugin reads (what
/// bridge/tools/extract_mc_assets.py does). Returns the number of files.
pub fn extract_assets(jar: &Path, out: &Path) -> Result<usize, String> {
    let f = fs::File::open(jar).map_err(|e| e.to_string())?;
    let mut z = zip::ZipArchive::new(f).map_err(|e| e.to_string())?;
    let mut n = 0;
    for i in 0..z.len() {
        let mut e = z.by_index(i).map_err(|e| e.to_string())?;
        let name = e.name().to_string();
        if !name.starts_with(PREFIX) || name.ends_with('/') { continue; }
        let rel = &name[PREFIX.len()..];
        if rel.split('/').any(|c| c == ".." || c.is_empty()) { continue; }
        let dest = out.join(rel);
        if let Some(p) = dest.parent() { fs::create_dir_all(p).map_err(|e| e.to_string())?; }
        let part = PathBuf::from(format!("{}.part", dest.display()));
        let mut o = fs::File::create(&part).map_err(|e| e.to_string())?;
        if let Err(er) = std::io::copy(&mut e.by_ref(), &mut o) { drop(o); let _ = fs::remove_file(&part); return Err(er.to_string()); }
        fs::rename(&part, &dest).map_err(|e| e.to_string())?;
        n += 1;
    }
    fs::write(out.join(STAMP), format!("{}\n", jar.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default())).map_err(|e| e.to_string())?;
    Ok(n)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::paths::quote_arg;

    #[test]
    fn desktop_entry_survives_odd_paths() {
        assert_eq!(desktop_exec_arg("/home/a b/p"), "\"/home/a b/p\"");
        assert_eq!(desktop_exec_arg("/h/100%/$x\"y"), "\"/h/100%%/\\\\$x\\\\\"y\"");
        let t = url_desktop_text("/home/Zoë/prism dir/PrismLauncher");
        assert!(t.contains("TryExec=/home/Zoë/prism dir/PrismLauncher\n"));
        assert!(t.contains("Exec=\"/home/Zoë/prism dir/PrismLauncher\" %U\n"));
    }

    #[test]
    fn prism_updater_is_switched_off() {
        assert_eq!(set_ini_key("[General]\nlast_check=2026-10-10T13:39:26\n", "General", "auto_check", "false"), "[General]\nlast_check=2026-10-10T13:39:26\nauto_check=false\n");
        assert_eq!(set_ini_key("[General]\nauto_check=true\n", "General", "auto_check", "false"), "[General]\nauto_check=false\n");
        assert_eq!(set_ini_key("", "General", "auto_check", "false"), "[General]\nauto_check=false\n");
    }

    #[test]
    fn cfg_is_rendered_and_merged() {
        let jvm = "-Dportalcraft.root=/x --enable-native-access=ALL-UNNAMED";
        let fresh = render_cfg(TEMPLATE_CFG, jvm, 6144, None);
        assert!(fresh.contains("JvmArgs=-Dportalcraft.root=/x"));
        assert!(fresh.contains("MaxMemAlloc=6144"));
        // Prism adds its own keys; ours are updated and theirs kept
        let old = format!("{fresh}lastLaunchTime=123\ntotalTimePlayed=9\n").replace("MaxMemAlloc=6144", "MaxMemAlloc=1");
        let merged = render_cfg(TEMPLATE_CFG, jvm, 8192, Some(&old));
        assert!(merged.contains("MaxMemAlloc=8192"));
        assert!(merged.contains("lastLaunchTime=123"));
        assert!(!merged.contains("MaxMemAlloc=1\n"));
    }

    #[test]
    fn jvm_tokens_have_what_the_mod_reads() {
        let l = Layout { data: PathBuf::from("/d/pc") };
        let mut s = Settings::default();
        let t = jvm_tokens(&l, &s);
        for want in ["-Dskycraft.world=terrain", "-Dskycraft.startHidden=true", "-Dportalcraft.root=/d/pc", "-Dportalcraft.cacheDir=/d/pc/cache/bridge", "-Dskycraft.seed="] {
            assert!(t.iter().any(|x| x == want), "missing {want}");
        }
        s.seed_random = false; s.seed = "12345".into();
        assert!(jvm_tokens(&l, &s).iter().any(|x| x == "-Dskycraft.seed=12345"));
        assert_eq!(quote_arg(Path::new("/a b")), "\"/a b\"");
    }

    #[test]
    fn prism_zip_unpacks_and_refuses_escapes() {
        use std::io::Write;
        let t = std::env::temp_dir().join(format!("pc-pz-{}", std::process::id()));
        let _ = fs::remove_dir_all(&t);
        fs::create_dir_all(&t).unwrap();
        let o = zip::write::SimpleFileOptions::default();
        let good = t.join("prism.zip");
        {
            let mut w = zip::ZipWriter::new(fs::File::create(&good).unwrap());
            w.start_file("prismlauncher.exe", o).unwrap(); w.write_all(b"MZ").unwrap();
            w.add_directory("imageformats/", o).unwrap();
            w.start_file("imageformats/qjpeg.dll", o).unwrap(); w.write_all(b"dll").unwrap();
            w.finish().unwrap();
        }
        let dest = t.join("prism");
        assert_eq!(extract_prism(&good, &dest).unwrap(), 2); // by extension: a .zip is a zip
        assert!(dest.join("prismlauncher.exe").is_file() && dest.join("imageformats/qjpeg.dll").is_file());
        let bad = t.join("bad.zip");
        {
            let mut w = zip::ZipWriter::new(fs::File::create(&bad).unwrap());
            w.start_file("../evil.txt", o).unwrap(); w.write_all(b"x").unwrap();
            w.finish().unwrap();
        }
        assert!(extract_prism(&bad, &dest).is_err());
        assert!(!t.join("evil.txt").exists());
        let _ = fs::remove_dir_all(&t);
    }

    #[test]
    fn extracts_only_minecraft_assets() {
        let t = std::env::temp_dir().join(format!("pc-jar-{}", std::process::id()));
        let _ = fs::remove_dir_all(&t);
        fs::create_dir_all(&t).unwrap();
        let jar = t.join("c.jar");
        {
            use std::io::Write;
            let mut w = zip::ZipWriter::new(fs::File::create(&jar).unwrap());
            let o = zip::write::SimpleFileOptions::default();
            w.start_file(PROBE, o).unwrap(); w.write_all(b"png").unwrap();
            w.start_file("net/minecraft/Foo.class", o).unwrap(); w.write_all(b"class").unwrap();
            w.start_file("assets/minecraft/../evil.txt", o).unwrap(); w.write_all(b"x").unwrap();
            w.finish().unwrap();
        }
        assert!(is_client_jar(&jar));
        let out = t.join("out");
        assert_eq!(extract_assets(&jar, &out).unwrap(), 1);
        assert!(out.join("textures/block/dirt.png").exists());
        assert!(!out.join("../evil.txt").exists());
        let _ = fs::remove_dir_all(&t);
    }
}
