//! Finds the player's Portal 2 (same rules as bridge/tools/portal2_dir.sh): the override from Settings, else Steam's
//! libraryfolders.vdf (native, ~/.local/share/Steam, Flatpak) for the library that holds app 620. On Windows the Steam
//! folder comes from the registry (HKCU\Software\Valve\Steam, SteamPath) and the program is portal2.exe.
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq)]
pub enum Portal2Error {
    /// The path set in Settings is not a Portal 2 folder.
    BadOverride(PathBuf),
    /// No Steam folder at all.
    NoSteam,
    /// Steam is there but Portal 2 is not installed in any library (the libraries looked at).
    NotInstalled(Vec<PathBuf>),
    /// Portal 2's files are there but not its program (Linux: portal2_linux; Windows: portal2.exe).
    NoProgram(PathBuf),
}

// the game's own files, not just a portal2 folder: Steam Cloud can leave one with only SAVE/ in a library that does not hold the game
fn has_portal2_data(d: &Path) -> bool { d.join("portal2").join("gameinfo.txt").is_file() }
#[cfg(unix)]
const PROGRAM: &str = "portal2_linux";
#[cfg(windows)]
const PROGRAM: &str = "portal2.exe";
fn is_valid(d: &Path) -> bool { has_portal2_data(d) && d.join(PROGRAM).exists() }

/// Steam's "SteamPath" and friends are written with forward slashes and a lower-case drive ("c:/program files (x86)/steam").
#[allow(dead_code)]
pub fn windows_steam_dir(steam_dir: &str) -> String {
    steam_dir.trim().trim_matches('"').replace('/', "\\").trim_end_matches('\\').to_string()
}

#[cfg(windows)]
pub fn steam_roots(_home: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = Vec::new();
    for d in crate::winsys::steam_install_dirs() {
        let p = PathBuf::from(windows_steam_dir(&d)).join("steamapps");
        if !v.contains(&p) { v.push(p); }
    }
    v
}

#[cfg(unix)]
pub fn steam_roots(home: &Path) -> Vec<PathBuf> {
    vec![
        home.join(".steam/steam/steamapps"),
        home.join(".local/share/Steam/steamapps"),
        // Debian/Ubuntu's steam package, the "root" link, and the Snap build
        home.join(".steam/debian-installation/steamapps"),
        home.join(".steam/root/steamapps"),
        home.join("snap/steam/common/.local/share/Steam/steamapps"),
        home.join(".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps"),
    ]
}

/// "path" "<dir>" entries (current format) and "<n>" "<dir>" entries (old format).
pub fn parse_library_paths(vdf: &str) -> Vec<String> {
    let mut v = Vec::new();
    for line in vdf.lines() {
        let q: Vec<&str> = line.split('"').collect();
        // `\t"key"\t\t"value"` splits into ["\t", key, "\t\t", value, ""]
        if q.len() >= 4 {
            let (k, val) = (q[1], q[3]);
            if (k == "path" && !val.is_empty()) || (k.chars().all(|c| c.is_ascii_digit()) && !k.is_empty() && (val.starts_with('/') || val.as_bytes().get(1) == Some(&b':'))) {
                v.push(val.replace("\\\\", "\\"));
            }
        }
    }
    v
}

pub fn parse_installdir(acf: &str) -> Option<String> {
    for line in acf.lines() {
        let q: Vec<&str> = line.split('"').collect();
        if q.len() >= 4 && q[1] == "installdir" && !q[3].is_empty() {
            return Some(q[3].to_string());
        }
    }
    None
}

/// canonicalize, but without the \\?\ prefix Windows adds (it would end up in messages and cfg files).
fn canonical(p: &Path) -> std::io::Result<PathBuf> {
    let c = std::fs::canonicalize(p)?;
    #[cfg(windows)]
    {
        let s = c.to_string_lossy();
        if let Some(rest) = s.strip_prefix(r"\\?\") { if rest.as_bytes().get(1) == Some(&b':') { return Ok(PathBuf::from(rest)); } }
    }
    Ok(c)
}

pub fn find(override_dir: &str) -> Result<PathBuf, Portal2Error> {
    let home = crate::util::home_dir();
    find_in(override_dir, &home, std::env::var("PORTAL2_DIR").ok())
}

pub fn find_in(override_dir: &str, home: &Path, env_dir: Option<String>) -> Result<PathBuf, Portal2Error> {
    find_in_roots(override_dir, home, &steam_roots(home), env_dir)
}

/// `roots` are the steamapps folders of the Steam installs to look at (tests pass their own).
pub fn find_in_roots(override_dir: &str, home: &Path, roots: &[PathBuf], env_dir: Option<String>) -> Result<PathBuf, Portal2Error> {
    for o in [override_dir.trim().to_string(), env_dir.unwrap_or_default()] {
        let o = o.trim().trim_matches(|c| c == '"' || c == '\'').to_string();
        if o.is_empty() { continue; }
        let o = match o.strip_prefix("~/") { Some(rest) => home.join(rest).to_string_lossy().to_string(), None => o };
        let p = PathBuf::from(if o.len() > 1 { o.trim_end_matches(|c| c == '/' || (cfg!(windows) && c == '\\')) } else { &o });
        if is_valid(&p) { return Ok(p); }
        if has_portal2_data(&p) { return Err(Portal2Error::NoProgram(p)); }
        return Err(Portal2Error::BadOverride(p));
    }
    let mut libs: Vec<PathBuf> = Vec::new();
    let mut any_steam = false;
    for sa in roots.iter().cloned() {
        if !sa.is_dir() { continue; }
        any_steam = true;
        if let Some(parent) = sa.parent() { libs.push(parent.to_path_buf()); }
        if let Ok(t) = std::fs::read_to_string(sa.join("libraryfolders.vdf")) {
            for p in parse_library_paths(&t) { libs.push(PathBuf::from(p)); }
        }
    }
    if !any_steam { return Err(Portal2Error::NoSteam); }
    let mut seen: Vec<PathBuf> = Vec::new();
    let mut half: Option<PathBuf> = None;
    // a library that has Steam's record of Portal 2 (appmanifest_620.acf) comes before one that only has leftovers
    let mut canon: Vec<PathBuf> = Vec::new();
    for lib in libs {
        let Ok(lib) = canonical(&lib) else { continue };
        if !canon.contains(&lib) { canon.push(lib); }
    }
    canon.sort_by_key(|lib| !lib.join("steamapps").join("appmanifest_620.acf").exists());
    for lib in canon {
        seen.push(lib.clone());
        let inst = std::fs::read_to_string(lib.join("steamapps").join("appmanifest_620.acf")).ok().and_then(|t| parse_installdir(&t)).unwrap_or_else(|| "Portal 2".into());
        let cand = lib.join("steamapps").join("common").join(inst);
        if is_valid(&cand) { return Ok(cand); }
        if has_portal2_data(&cand) { half = Some(cand); }
    }
    if let Some(h) = half { return Err(Portal2Error::NoProgram(h)); }
    Err(Portal2Error::NotInstalled(seen))
}

/// Is Portal 2 forced through Steam Play (Proton) in Steam's settings? Then `steam://rungameid/620` would not start the
/// Linux program at all. Reads CompatToolMapping in <steam>/config/config.vdf.
#[cfg(windows)]
pub fn forced_compat_tool(_home: &Path) -> Option<String> { None } // Windows runs Portal 2 itself: no Proton

#[cfg(unix)]
pub fn forced_compat_tool(home: &Path) -> Option<String> {
    for sa in steam_roots(home) {
        let Ok(t) = std::fs::read_to_string(sa.parent()?.join("config/config.vdf")) else { continue };
        if let Some(n) = compat_tool_for_620(&t) { return Some(n); }
    }
    None
}

#[allow(dead_code)] // Windows has no Proton; the parser is shared and tested
pub fn compat_tool_for_620(vdf: &str) -> Option<String> {
    let mut in_map = false;
    let (mut depth, mut map_depth) = (0i32, 0i32);
    let mut in_620 = false;
    let mut pending_key: Option<String> = None;
    for line in vdf.lines() {
        let t = line.trim();
        if t == "{" { depth += 1; if let Some(k) = pending_key.take() { if k == "CompatToolMapping" && !in_map { in_map = true; map_depth = depth; } else if in_map && k == "620" && depth == map_depth + 1 { in_620 = true; } } continue; }
        if t == "}" { if in_620 && depth == map_depth + 1 { in_620 = false; } if in_map && depth == map_depth { in_map = false; } depth -= 1; continue; }
        let q: Vec<&str> = t.split('"').collect();
        if q.len() >= 4 {
            if in_620 && q[1] == "name" && !q[3].is_empty() { return Some(q[3].to_string()); }
            pending_key = None;
        } else if q.len() >= 2 && t.starts_with('"') {
            pending_key = Some(q[1].to_string());
        }
    }
    None
}

/// The Steam account whose saves Portal 2 will load from (Steam's most recently used login), if known.
fn active_steam_id(home: &Path) -> Option<String> {
    for sa in steam_roots(home) {
        let Ok(t) = std::fs::read_to_string(sa.parent()?.join("config/loginusers.vdf")) else { continue };
        let mut id: Option<String> = None;
        for line in t.lines() {
            let q: Vec<&str> = line.trim().split('"').collect();
            if q.len() == 3 && q[1].len() >= 16 && q[1].chars().all(|c| c.is_ascii_digit()) { id = Some(q[1].to_string()); }
            if q.len() >= 4 && q[1] == "MostRecent" && q[3] == "1" { if let Some(i) = id { return Some(i); } }
        }
    }
    None
}

pub fn maps_dir(p2: &Path) -> PathBuf { p2.join("portal2/maps") }

/// A Portal 2 save the launcher can continue from.
#[derive(Debug, Clone, PartialEq)]
pub struct SaveInfo {
    /// File name without .sav (what `load` takes).
    pub name: String,
    /// The campaign chamber it was made in.
    pub map: String,
    pub modified: std::time::SystemTime,
}

/// The map a save was made in: Source saves keep the map name near the start of the file (offset ~4230 in Portal 2).
pub fn save_map(bytes: &[u8]) -> Option<String> {
    let head = &bytes[..bytes.len().min(16384)];
    let i = head.windows(4).position(|w| w == b"sp_a")?;
    let n: String = head[i..].iter().take_while(|c| c.is_ascii_alphanumeric() || **c == b'_').map(|c| *c as char).collect();
    if n.len() > 6 { Some(n) } else { None }
}

/// The newest single-player campaign save (autosave, quick save or a named save) of any Steam user on this PC.
/// Community-map and co-op saves are skipped: "Continue" only means the campaign.
pub fn latest_campaign_save(p2: &Path) -> Option<SaveInfo> {
    let home = crate::util::home_dir();
    latest_campaign_save_for(p2, active_steam_id(&home).as_deref())
}

/// `load <name>` reads the signed-in Steam account's own folder (portal2/SAVE/<steamid64>/), so a second account's
/// saves must not be picked: with a known active account only its folder counts; otherwise every folder does.
pub fn latest_campaign_save_for(p2: &Path, active: Option<&str>) -> Option<SaveInfo> {
    let root = p2.join("portal2").join("SAVE");
    let mut dirs = vec![root.clone()];
    if let Ok(rd) = std::fs::read_dir(&root) { dirs.extend(rd.flatten().map(|e| e.path()).filter(|p| p.is_dir())); }
    if let Some(a) = active {
        if root.join(a).is_dir() { dirs = vec![root.join(a)]; }
    }
    let mut best: Option<SaveInfo> = None;
    for d in dirs {
        let Ok(rd) = std::fs::read_dir(&d) else { continue };
        for e in rd.flatten() {
            let p = e.path();
            if p.extension().and_then(|x| x.to_str()) != Some("sav") { continue; }
            let Some(name) = p.file_stem().and_then(|x| x.to_str()).map(str::to_string) else { continue };
            if name.starts_with("community_") || !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') { continue; }
            let Ok(modified) = e.metadata().and_then(|m| m.modified()) else { continue };
            if best.as_ref().is_some_and(|b| b.modified >= modified) { continue; }
            let mut f = match std::fs::File::open(&p) { Ok(f) => f, Err(_) => continue };
            let mut buf = vec![0u8; 16384];
            let n = std::io::Read::read(&mut f, &mut buf).unwrap_or(0);
            if let Some(map) = save_map(&buf[..n]) { best = Some(SaveInfo { name, map, modified }); }
        }
    }
    best
}

/// Modification time of one of the player's saves (the newest copy of that name among the candidate folders).
pub fn save_modified(p2: &Path, name: &str) -> Option<std::time::SystemTime> {
    latest_campaign_save(p2).filter(|s| s.name == name).map(|s| s.modified)
}

/// After "Continue" loaded save `name`, progress.txt says the visit began now, which is later than the save, so the next
/// Continue would throw the save away as "from an earlier visit". Moves the visit's start back to the save's time.
/// Returns true when the file was rewritten.
pub fn backdate_progress(progress_file: &Path, map: &str, save_time: std::time::SystemTime) -> bool {
    let Ok(t) = std::fs::read_to_string(progress_file) else { return false };
    let mut it = t.lines();
    let (Some(m), Some(ts)) = (it.next(), it.next().and_then(|x| x.trim().parse::<u64>().ok())) else { return false };
    let st = save_time.duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    if m.trim() != map || ts <= st { return false; }
    std::fs::write(progress_file, format!("{map}\n{st}\n")).is_ok()
}

/// Where "Continue" goes: the last campaign level played with Portalcraft, from Portal 2's newest save if that save
/// was made in that level during or after that visit, otherwise from the start of that level. Demo maps, test maps and
/// saves from playing Portal 2 without Portalcraft never count.
#[derive(Debug, Clone, PartialEq)]
pub struct ContinuePoint {
    pub map: String,
    pub save: Option<String>,
    pub when: std::time::SystemTime,
}

pub fn continue_point(progress: &str, save: Option<SaveInfo>) -> Option<ContinuePoint> {
    let mut it = progress.lines();
    let map = it.next()?.trim().to_string();
    let t: u64 = it.next()?.trim().parse().ok()?;
    if !map.starts_with("sp_a") || !map.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') { return None; }
    let mut entered = std::time::UNIX_EPOCH + std::time::Duration::from_secs(t);
    // the clock was set back since (dual boot, NTP): a visit "in the future" cannot rule saves out
    let future = entered > std::time::SystemTime::now() + std::time::Duration::from_secs(120);
    let shown = if future { std::time::SystemTime::now() } else { entered };
    if future { entered = std::time::UNIX_EPOCH; }
    match save {
        Some(s) if s.map == map && s.modified >= entered => Some(ContinuePoint { map, save: Some(s.name), when: s.modified }),
        _ => Some(ContinuePoint { map, save: None, when: shown }),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The Steam folders a test looks at: its own fake tree, whatever the real machine (or wine's registry) has.
    fn test_roots(t: &Path) -> Vec<PathBuf> { vec![t.join(".steam/steam/steamapps")] }

    fn fake_steam(root: &Path, lib: &Path, with_linux: bool) {
        let sa = root.join(".steam/steam/steamapps");
        std::fs::create_dir_all(&sa).unwrap();
        std::fs::write(sa.join("libraryfolders.vdf"), format!("\"libraryfolders\"\n{{\n\t\"0\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t}}\n}}\n", lib.display())).unwrap();
        let g = lib.join("steamapps/common/Portal 2");
        std::fs::create_dir_all(g.join("portal2")).unwrap();
        std::fs::write(g.join("portal2/gameinfo.txt"), "").unwrap();
        std::fs::write(lib.join("steamapps").join("appmanifest_620.acf"), "\"AppState\"\n{\n\t\"installdir\"\t\t\"Portal 2\"\n}\n").unwrap();
        if with_linux { std::fs::write(g.join(PROGRAM), "").unwrap(); } // (the platform's own program file)
    }

    #[test]
    fn finds_in_second_library() {
        let t = std::env::temp_dir().join(format!("pc-p2-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let lib = t.join("SteamLibrary");
        fake_steam(&t, &lib, true);
        let got = find_in_roots("", &t, &test_roots(&t), None).unwrap();
        assert!(got.ends_with("steamapps/common/Portal 2"));
        // override wins, and a bad one is reported
        assert!(matches!(find_in_roots("/nonexistent", &t, &test_roots(&t), None), Err(Portal2Error::BadOverride(_))));
        let _ = std::fs::remove_dir_all(&t);
    }

    #[test]
    fn reports_missing_cases() {
        let t = std::env::temp_dir().join(format!("pc-p2b-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        std::fs::create_dir_all(&t).unwrap();
        assert_eq!(find_in_roots("", &t, &test_roots(&t), None), Err(Portal2Error::NoSteam));
        std::fs::create_dir_all(t.join(".steam/steam/steamapps")).unwrap();
        assert!(matches!(find_in_roots("", &t, &test_roots(&t), None), Err(Portal2Error::NotInstalled(_))));
        let lib = t.join("lib");
        // only Steam Cloud saves in a 'Portal 2' folder (the library holding the game is not listed): not installed, not "no program"
        let saves = t.join(".steam/steam/steamapps/common/Portal 2/portal2/SAVE");
        std::fs::create_dir_all(&saves).unwrap();
        assert!(matches!(find_in_roots("", &t, &test_roots(&t), None), Err(Portal2Error::NotInstalled(_))));
        fake_steam(&t, &lib, false);
        assert!(matches!(find_in_roots("", &t, &test_roots(&t), None), Err(Portal2Error::NoProgram(_))));
        let _ = std::fs::remove_dir_all(&t);
    }

    #[test]
    fn leftover_library_does_not_beat_the_real_one() {
        let t = std::env::temp_dir().join(format!("pc-p2c-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let lib = t.join("Steam Library \u{e9}");
        fake_steam(&t, &lib, true);
        // the default library holds a stale 'Portal 2' folder with a portal2_linux but no appmanifest
        let stale = t.join(".steam/steam/steamapps/common/Portal 2");
        std::fs::create_dir_all(stale.join("portal2")).unwrap();
        std::fs::write(stale.join("portal2/gameinfo.txt"), "").unwrap();
        std::fs::write(stale.join(PROGRAM), "").unwrap();
        let got = find_in_roots("", &t, &test_roots(&t), None).unwrap();
        assert!(got.starts_with(canonical(&lib).unwrap()), "{got:?}");
        // ~ and quotes in the override
        assert_eq!(find_in_roots("\"~/Steam Library \u{e9}/steamapps/common/Portal 2\"", &t, &test_roots(&t), None).unwrap(), lib.join("steamapps/common/Portal 2"));
        let _ = std::fs::remove_dir_all(&t);
    }

    #[test]
    fn proton_forced_for_portal_2() {
        let v = "\"InstallConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n\t\t\t\t\"CompatToolMapping\"\n\t\t\t\t{\n\t\t\t\t\t\"730\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"name\"\t\t\"proton_8\"\n\t\t\t\t\t}\n\t\t\t\t\t\"620\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"name\"\t\t\"proton_experimental\"\n\t\t\t\t\t\t\"priority\"\t\t\"250\"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
        assert_eq!(compat_tool_for_620(v).as_deref(), Some("proton_experimental"));
        assert_eq!(compat_tool_for_620(&v.replace("\"620\"", "\"621\"")), None);
        assert_eq!(compat_tool_for_620(&v.replace("proton_experimental", "")), None);
    }

    #[test]
    fn saves_of_the_active_steam_account_only() {
        use std::time::{Duration, UNIX_EPOCH};
        let t = std::env::temp_dir().join(format!("pc-p2d-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let mk = |user: &str, name: &str| {
            let d = t.join("portal2/SAVE").join(user);
            std::fs::create_dir_all(&d).unwrap();
            let mut b = vec![0u8; 4230]; b.extend_from_slice(b"sp_a2_dual_lasers\0");
            std::fs::write(d.join(format!("{name}.sav")), b).unwrap();
        };
        mk("111", "mine"); mk("222", "theirs");
        let f = std::fs::File::options().write(true).open(t.join("portal2/SAVE/222/theirs.sav")).unwrap();
        f.set_modified(UNIX_EPOCH + Duration::from_secs(4_000_000_000)).unwrap();
        assert_eq!(latest_campaign_save_for(&t, None).unwrap().name, "theirs");
        assert_eq!(latest_campaign_save_for(&t, Some("111")).unwrap().name, "mine");
        // unknown account folder: falls back to all
        assert_eq!(latest_campaign_save_for(&t, Some("999")).unwrap().name, "theirs");
        let _ = std::fs::remove_dir_all(&t);
    }

    #[test]
    fn continue_survives_clock_trouble_and_backdating() {
        use std::time::{Duration, UNIX_EPOCH};
        let sv = Some(SaveInfo { name: "quick".into(), map: "sp_a1_intro1".into(), modified: UNIX_EPOCH + Duration::from_secs(1000) });
        // progress written by a clock that was later set back (year 2100): the save still counts
        assert_eq!(continue_point("sp_a1_intro1\n4102444800\n", sv.clone()).unwrap().save.as_deref(), Some("quick"));
        // garbage and CRLF
        assert!(continue_point("sp_a1_intro1\r\nabc\r\n", None).is_none());
        assert!(continue_point("sp_a1_intro1\r\n5\r\n", None).is_some());
        // after loading `quick` (mtime 1000) the visit began at 5000: backdated so the next Continue loads it again
        let f = std::env::temp_dir().join(format!("pc-prog-{}", std::process::id()));
        std::fs::write(&f, "sp_a1_intro1\n5000\n").unwrap();
        assert!(backdate_progress(&f, "sp_a1_intro1", UNIX_EPOCH + Duration::from_secs(1000)));
        assert_eq!(continue_point(&std::fs::read_to_string(&f).unwrap(), sv).unwrap().save.as_deref(), Some("quick"));
        assert!(!backdate_progress(&f, "sp_a2_other", UNIX_EPOCH + Duration::from_secs(10)));
        let _ = std::fs::remove_file(&f);
    }

    #[test]
    fn vdf_old_and_new() {
        let v = parse_library_paths("\"libraryfolders\"\n{\n \"0\"\t\"/a/b\"\n \"1\" { \"path\" \"/c d\" }\n}\n");
        assert_eq!(v, vec!["/a/b".to_string()]);
        let v = parse_library_paths("\t\t\"path\"\t\t\"/run/media/x/SteamLibrary\"\n");
        assert_eq!(v, vec!["/run/media/x/SteamLibrary".to_string()]);
    }

    #[test]
    fn windows_style_vdf_and_registry_path() {
        // libraryfolders.vdf as Steam writes it on Windows: backslashes doubled, drive letters
        let v = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t\t\"label\"\t\t\"\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n";
        assert_eq!(parse_library_paths(v), vec!["C:\\Program Files (x86)\\Steam".to_string(), "D:\\SteamLibrary".to_string()]);
        // the old numbered format with a drive letter
        assert_eq!(parse_library_paths("\"LibraryFolders\"\n{\n\t\"1\"\t\t\"E:\\\\Games\\\\Steam\"\n}\n"), vec!["E:\\Games\\Steam".to_string()]);
        // SteamPath from the registry: forward slashes, lower-case drive, maybe quotes
        assert_eq!(windows_steam_dir("c:/program files (x86)/steam"), "c:\\program files (x86)\\steam");
        assert_eq!(windows_steam_dir(" \"D:/Steam/\" "), "D:\\Steam");
    }

    #[test]
    fn save_map_reads_the_chamber() {
        let mut b = vec![0u8; 4230];
        b[..4].copy_from_slice(b"JSAV");
        b.extend_from_slice(b"sp_a2_dual_lasers\0\0junk");
        assert_eq!(save_map(&b).as_deref(), Some("sp_a2_dual_lasers"));
        assert_eq!(save_map(b"JSAV nothing here"), None);
    }

    #[test]
    fn continue_ignores_saves_from_other_levels_and_older_visits() {
        use std::time::{Duration, UNIX_EPOCH};
        let sv = |map: &str, t: u64| Some(SaveInfo { name: "autosave".into(), map: map.into(), modified: UNIX_EPOCH + Duration::from_secs(t) });
        // newest save is from another level (a test run): start of the level last played
        assert_eq!(continue_point("sp_a1_intro1\n1000\n", sv("sp_a2_dual_lasers", 2000)).unwrap().save, None);
        // save in that level, made after entering it: load it
        assert_eq!(continue_point("sp_a1_intro1\n1000\n", sv("sp_a1_intro1", 1500)).unwrap().save.as_deref(), Some("autosave"));
        // save in that level but from an earlier visit: start of the level
        assert_eq!(continue_point("sp_a1_intro1\n1000\n", sv("sp_a1_intro1", 900)).unwrap().save, None);
        assert!(continue_point("pc_pit_flings\n1000\n", None).is_none());
        assert!(continue_point("", None).is_none());
    }
}
