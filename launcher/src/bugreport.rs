//! The bug report: one zip in the home folder with the logs a bug needs and nothing secret (never Prism's accounts file).
use crate::paths::Layout;
use crate::settings::{Settings, State};
use crate::util::{self, date_stamp};
use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

const MAX_FILE: u64 = 12 * 1024 * 1024;
const MAX_TOTAL: u64 = 40 * 1024 * 1024;

fn add_bytes(z: &mut zip::ZipWriter<fs::File>, name: &str, data: &[u8], total: &mut u64) -> std::io::Result<()> {
    if *total + data.len() as u64 > MAX_TOTAL { return Ok(()); }
    z.start_file(name, zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated)).map_err(std::io::Error::other)?;
    z.write_all(data)?;
    *total += data.len() as u64;
    Ok(())
}

/// The last `max` bytes of a file (a log), starting at a line boundary.
fn tail_of(p: &Path, max: u64) -> Option<Vec<u8>> {
    let mut f = fs::File::open(p).ok()?;
    let len = f.metadata().ok()?.len();
    let mut buf = Vec::new();
    if len > max {
        use std::io::Seek;
        f.seek(std::io::SeekFrom::Start(len - max)).ok()?;
        f.read_to_end(&mut buf).ok()?;
        if let Some(i) = buf.iter().position(|b| *b == b'\n') { buf.drain(..=i); }
        let mut v = format!("[... first {} bytes of this log cut ...]\n", len - max).into_bytes();
        v.extend(buf);
        return Some(v);
    }
    f.read_to_end(&mut buf).ok()?;
    Some(buf)
}

fn newest_entries(dir: &Path, n: usize) -> Vec<PathBuf> {
    let mut v: Vec<(std::time::SystemTime, PathBuf)> = fs::read_dir(dir).map(|rd| rd.flatten().filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path()))).collect()).unwrap_or_default();
    v.sort_by(|a, b| b.0.cmp(&a.0));
    v.into_iter().take(n).map(|x| x.1).collect()
}

fn add_tree(z: &mut zip::ZipWriter<fs::File>, base: &Path, p: &Path, prefix: &str, total: &mut u64, depth: u32) {
    if depth == 0 { return; }
    if p.is_dir() {
        if let Ok(rd) = fs::read_dir(p) { for e in rd.flatten() { add_tree(z, base, &e.path(), prefix, total, depth - 1); } }
    } else if let Ok(md) = fs::metadata(p) {
        if md.len() <= MAX_FILE {
            if let (Ok(data), Ok(rel)) = (fs::read(p), p.strip_prefix(base)) {
                // zip entry names always use '/', also on Windows
                let rel = rel.components().map(|c| c.as_os_str().to_string_lossy().to_string()).collect::<Vec<_>>().join("/");
                let _ = add_bytes(z, &format!("{prefix}/{rel}"), &data, total);
            }
        }
    }
}

fn system_info(l: &Layout, s: &Settings) -> String {
    let mut t = String::new();
    t.push_str(&format!("Portalcraft launcher {}\n", env!("CARGO_PKG_VERSION")));
    t.push_str(&format!("date: {}\n", util::timestamp()));
    t.push_str(&format!("data folder: {}\n", l.data.display()));
    #[cfg(unix)]
    {
        t.push_str(&format!("kernel: {}\n", fs::read_to_string("/proc/version").unwrap_or_default().trim()));
        t.push_str(&format!("os: {}\n", fs::read_to_string("/etc/os-release").unwrap_or_default().lines().find(|x| x.starts_with("PRETTY_NAME")).unwrap_or("")));
        t.push_str(&format!("memory: {}\n", fs::read_to_string("/proc/meminfo").unwrap_or_default().lines().take(3).collect::<Vec<_>>().join(" | ")));
        t.push_str(&format!("/dev/shm free: {}\n", util::shm_free_bytes().map(util::human_bytes).unwrap_or_else(|| "?".into())));
        t.push_str(&format!("session: {} / {}\n", std::env::var("XDG_SESSION_TYPE").unwrap_or_default(), std::env::var("XDG_CURRENT_DESKTOP").unwrap_or_default()));
    }
    #[cfg(windows)]
    {
        let mut sys = sysinfo::System::new();
        sys.refresh_memory();
        t.push_str(&format!("os: {} (kernel {})\n", sysinfo::System::long_os_version().unwrap_or_default(), sysinfo::System::kernel_version().unwrap_or_default()));
        t.push_str(&format!("memory: {} total, {} available\n", util::human_bytes(sys.total_memory()), util::human_bytes(sys.available_memory())));
        t.push_str(&format!("data drive free: {}\n", util::free_bytes(&l.data).map(util::human_bytes).unwrap_or_else(|| "?".into())));
        t.push_str(&format!("steam registry: {:?}\n", crate::winsys::steam_install_dirs()));
        t.push_str(&format!("plugin file present: {}\n", l.plugin_so().exists()));
    }
    t.push_str(&format!("portal 2: {}\n", crate::portal2::find(&s.portal2_dir).map(|p| p.display().to_string()).unwrap_or_else(|e| format!("{e:?}"))));
    t.push_str(&format!("portal2 running: {:?}\n", util::portal2_pids()));
    t.push_str(&format!("minecraft running: {:?}\n", crate::prism::minecraft_procs(l).iter().map(|p| p.pid).collect::<Vec<_>>()));
    t.push_str(&format!("signed in to Prism (yes/no only): {}\n", crate::prism::signed_in(l)));
    t.push_str(&format!("installed: {:?}\n", State::load(l).installed));
    t.push_str(&format!("jvm args: {}\n", crate::prism::jvm_tokens(l, s).join(" ")));
    t
}

pub fn unique_path(home: &Path) -> PathBuf {
    let base = format!("Portalcraft-bugreport-{}", date_stamp());
    let mut p = home.join(format!("{base}.zip"));
    let mut n = 2;
    while p.exists() {
        p = home.join(format!("{base}-{n}.zip"));
        n += 1;
    }
    p
}

/// A failed attempt (disk full) must not leave a broken zip in the player's home folder.
pub fn create(l: &Layout, s: &Settings, home: &Path) -> std::io::Result<PathBuf> {
    let out = unique_path(home);
    match create_at(l, s, &out) {
        Ok(()) => Ok(out),
        Err(e) => { let _ = fs::remove_file(&out); Err(e) }
    }
}

fn create_at(l: &Layout, s: &Settings, out: &Path) -> std::io::Result<()> {
    let mut z = zip::ZipWriter::new(fs::File::create(out)?);
    let mut total = 0u64;
    add_bytes(&mut z, "system.txt", system_info(l, s).as_bytes(), &mut total)?;
    if let Some(b) = tail_of(&l.bridge_log(), MAX_FILE) { add_bytes(&mut z, "bridge.log", &b, &mut total)?; }
    // mc.log: Minecraft's own log of the last run (the dev flow's cache/bridge/mc.log when it exists)
    if let Some(b) = tail_of(&l.mc_latest_log(), MAX_FILE).or_else(|| tail_of(&l.bridge_dir().join("mc.log"), MAX_FILE)) { add_bytes(&mut z, "mc.log", &b, &mut total)?; }
    if let Ok(rd) = fs::read_dir(l.game_dir().join("crash-reports")) {
        for e in newest_entries(&l.game_dir().join("crash-reports"), 2) { let _ = &rd; if let Some(b) = tail_of(&e, 2 << 20) { let n = e.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default(); add_bytes(&mut z, &format!("crash-reports/{n}"), &b, &mut total)?; } }
    }
    if let Some(b) = tail_of(&l.launcher_log(), 4 << 20) { add_bytes(&mut z, "launcher.log", &b, &mut total)?; }
    if let Some(b) = tail_of(&l.prism_log(), 2 << 20) { add_bytes(&mut z, "prism-output.log", &b, &mut total)?; }
    if let Ok(b) = fs::read(l.instance_dir().join("instance.cfg")) { add_bytes(&mut z, "instance.cfg", &b, &mut total)?; }
    // the latest bugmarks (F8 notes and replays)
    let bm = l.bugmarks_dir();
    for e in newest_entries(&bm, 3) { add_tree(&mut z, &bm, &e, "bugmarks", &mut total, 4); }
    z.finish().map_err(std::io::Error::other)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zip_has_logs_and_no_accounts() {
        let t = std::env::temp_dir().join(format!("pc-bug-{}", std::process::id()));
        let _ = fs::remove_dir_all(&t);
        let l = Layout { data: t.join("data") };
        fs::create_dir_all(l.bugmarks_dir().join("mark1")).unwrap();
        fs::create_dir_all(l.prism_dir()).unwrap();
        fs::write(l.prism_accounts(), "{\"accounts\":[{\"type\":\"MSA\",\"ygg\":{\"token\":\"SECRET\"}}]}").unwrap();
        fs::write(l.bridge_log(), "[bridge] link up\n").unwrap();
        fs::create_dir_all(l.mc_latest_log().parent().unwrap()).unwrap();
        fs::write(l.mc_latest_log(), "mc log\n").unwrap();
        fs::write(l.bugmarks_dir().join("mark1/note.txt"), "stuck").unwrap();
        util::init_log(l.launcher_log());
        util::log("hello");
        fs::create_dir_all(&t).unwrap();
        let out = create(&l, &Settings::default(), &t).unwrap();
        let mut z = zip::ZipArchive::new(fs::File::open(&out).unwrap()).unwrap();
        let names: Vec<String> = (0..z.len()).map(|i| z.by_index(i).unwrap().name().to_string()).collect();
        for want in ["system.txt", "bridge.log", "mc.log", "launcher.log", "bugmarks/mark1/note.txt"] {
            assert!(names.iter().any(|n| n == want), "{want} missing from {names:?}");
        }
        for n in &names {
            let mut s = String::new();
            z.by_name(n).unwrap().read_to_string(&mut s).unwrap();
            assert!(!s.contains("SECRET"));
        }
        assert!(unique_path(&t).to_string_lossy().ends_with("-2.zip"));
        let _ = fs::remove_dir_all(&t);
    }
}
