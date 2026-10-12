//! launcher.json: the release manifest (schema like ReSkate's). Every component has a version, a url, a sha256 and a size.
//! `asset:<name>` urls are resolved against the folder the manifest itself was read from, so a release (or a local test
//! folder) is self-contained.
use crate::paths::Layout;
use crate::util::{fetch_text, version_newer, write_atomic, NetError};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Asset {
    pub version: String,
    pub url: String,
    pub sha256: String,
    #[serde(default)]
    pub size: u64,
}

/// The parts of a release that differ per operating system. Everything not listed here (the Fabric mod, Fabric API, the
/// profile template, the map tools) is the same on every system.
#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct PlatformAssets {
    /// The launcher program for this system (self-update).
    #[serde(default)]
    pub launcher: Option<Asset>,
    /// The Portal 2 plugin for this system. Absent: this release has none yet (the launcher says so when you press Play).
    #[serde(default)]
    pub plugin: Option<Asset>,
    /// The portable Prism Launcher for this system.
    #[serde(default)]
    pub prism: Option<Asset>,
}

/// The release list. Schema 1 stays readable by every launcher: the top-level `launcher`, `plugin` and `prism` are the
/// Linux x86_64 files (exactly as before), and the optional `platforms` map adds the other systems. A launcher that does
/// not know `platforms` ignores it; a Windows launcher reads only `platforms["windows-x86_64"]`. So adding a system, or
/// the Windows plugin later, never needs a schema bump (that is kept for changes an old launcher could misread).
#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Manifest {
    pub schema: u32,
    #[serde(default)]
    pub release: String,
    /// The Linux launcher binary (self-update).
    #[serde(default)]
    pub launcher: Option<Asset>,
    /// The 32-bit Linux Portal 2 plugin (portalcraft_bridge.so).
    #[serde(default)]
    pub plugin: Option<Asset>,
    /// The Fabric mod jar.
    pub r#mod: Asset,
    /// Fabric API (from Modrinth's CDN, pinned by hash).
    pub fabric_api: Asset,
    /// Portable Prism Launcher for Linux (Qt6 portable tarball, from github.com/PrismLauncher).
    #[serde(default)]
    pub prism: Option<Asset>,
    /// Instance template (instance.cfg.in + mmc-pack.json). Optional: the launcher has its own copy built in.
    #[serde(default)]
    pub instance: Option<Asset>,
    /// Optional: the map-building scripts (needs python3 and Portal 2's own compile tools).
    #[serde(default)]
    pub map_tools: Option<Asset>,
    /// Per-system files, keyed "windows-x86_64" (and "linux-x86_64" if a release wants to override the top level).
    #[serde(default)]
    pub platforms: BTreeMap<String, PlatformAssets>,
}

pub const SCHEMA: u32 = 1;

/// The key of this system in `platforms`.
#[cfg(windows)]
pub const PLATFORM: &str = "windows-x86_64";
#[cfg(not(windows))]
pub const PLATFORM: &str = "linux-x86_64";

impl Manifest {
    pub fn parse(text: &str, base: &str) -> Result<Manifest, String> {
        let mut m: Manifest = serde_json::from_str(text).map_err(|e| format!("launcher.json is not valid: {e}"))?;
        if m.schema > SCHEMA {
            return Err(format!("launcher.json uses schema {}, this launcher understands {SCHEMA}: update the launcher first", m.schema));
        }
        m.resolve(base);
        Ok(m)
    }

    fn resolve(&mut self, base: &str) {
        let fix = |a: &mut Asset| {
            if let Some(name) = a.url.strip_prefix("asset:") {
                a.url = format!("{}/{}", base.trim_end_matches('/'), name);
            }
        };
        for a in [&mut self.launcher, &mut self.plugin, &mut self.prism, &mut self.instance, &mut self.map_tools] {
            if let Some(a) = a.as_mut() { fix(a); }
        }
        fix(&mut self.r#mod);
        fix(&mut self.fabric_api);
        for p in self.platforms.values_mut() {
            for a in [&mut p.launcher, &mut p.plugin, &mut p.prism] {
                if let Some(a) = a.as_mut() { fix(a); }
            }
        }
    }

    fn here(&self) -> Option<&PlatformAssets> { self.platforms.get(PLATFORM) }

    /// The launcher program for this system, if the release has one.
    pub fn launcher_asset(&self) -> Option<&Asset> {
        self.here().and_then(|p| p.launcher.as_ref()).or(if cfg!(windows) { None } else { self.launcher.as_ref() })
    }
    /// The Portal 2 plugin for this system. None: this release has no plugin for it (Windows today).
    pub fn plugin_asset(&self) -> Option<&Asset> {
        self.here().and_then(|p| p.plugin.as_ref()).or(if cfg!(windows) { None } else { self.plugin.as_ref() })
    }
    /// The portable Prism Launcher for this system.
    pub fn prism_asset(&self) -> Option<&Asset> {
        self.here().and_then(|p| p.prism.as_ref()).or(if cfg!(windows) { None } else { self.prism.as_ref() })
    }

    pub fn launcher_update_available(&self) -> bool {
        self.launcher_asset().map(|a| version_newer(&a.version, env!("CARGO_PKG_VERSION"))).unwrap_or(false)
    }
}

fn base_of(url: &str) -> String {
    match if cfg!(windows) { url.rfind(['/', '\\']) } else { url.rfind('/') } {
        Some(i) => url[..i].to_string(),
        None => ".".to_string(),
    }
}

/// Fetches and caches the manifest.
pub fn fetch(l: &Layout, url: &str) -> Result<Manifest, FetchError> {
    let text = fetch_text(url).map_err(FetchError::Net)?;
    let m = match Manifest::parse(&text, &base_of(url)) {
        Ok(m) => m,
        Err(e) => return Err(launcher_only(&text, &base_of(url)).map(FetchError::NeedsNewerLauncher).unwrap_or(FetchError::Bad(e))),
    };
    // cache the raw text with the resolved base so offline mode can still use it
    let _ = write_atomic(&l.manifest_cache(), text.as_bytes());
    let _ = write_atomic(&l.manifest_cache().with_extension("base"), base_of(url).as_bytes());
    Ok(m)
}

/// A release made for a newer launcher (higher schema) that this one cannot read in full: it still names the new
/// launcher, so the player can update instead of being stuck on the old saved list forever.
pub fn launcher_only(text: &str, base: &str) -> Option<Asset> {
    #[derive(Deserialize)]
    struct Head {
        schema: u32,
        #[serde(default)]
        launcher: Option<Asset>,
        #[serde(default)]
        platforms: BTreeMap<String, PlatformAssets>,
    }
    let h: Head = serde_json::from_str(text).ok()?;
    if h.schema <= SCHEMA { return None; }
    let mine = h.platforms.get(PLATFORM).and_then(|p| p.launcher.clone());
    let mut a = mine.or(if cfg!(windows) { None } else { h.launcher })?;
    if let Some(name) = a.url.strip_prefix("asset:") { a.url = format!("{}/{}", base.trim_end_matches('/'), name); }
    Some(a)
}

/// The release list in the release zip's `files` folder (next to the launcher); its `asset:` files are in that folder.
pub fn bundled() -> Option<Manifest> {
    let d = crate::engine::bundle_dir()?;
    let text = std::fs::read_to_string(d.join("launcher.json")).ok()?;
    Manifest::parse(&text, &d.to_string_lossy()).ok()
}

pub fn cached(l: &Layout) -> Option<Manifest> {
    let text = std::fs::read_to_string(l.manifest_cache()).ok()?;
    let base = std::fs::read_to_string(l.manifest_cache().with_extension("base")).unwrap_or_default();
    Manifest::parse(&text, &base).ok()
}

#[derive(Debug)]
pub enum FetchError {
    Net(NetError),
    Bad(String),
    /// The release needs a newer launcher; here is that launcher.
    NeedsNewerLauncher(Asset),
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn newer_schema_still_offers_the_launcher() {
        let t = r#"{"schema": 99, "launcher": {"version": "9.0.0", "url": "asset:pl", "sha256": "ab", "size": 1}, "plugin": 5}"#;
        // the top-level launcher is the Linux one: a Windows launcher has none to offer from this
        let a = launcher_only(t, "https://x/rel");
        if cfg!(windows) { assert!(a.is_none()); return; }
        let a = a.unwrap();
        assert_eq!(a.url, "https://x/rel/pl");
        assert!(launcher_only(r#"{"schema": 1, "launcher": {"version": "1", "url": "u", "sha256": "s"}}"#, "b").is_none());
    }

    const OLD: &str = r#"{"schema": 1, "release": "0.1.0",
      "launcher": {"version": "0.1.0", "url": "asset:pl", "sha256": "ab", "size": 1},
      "plugin": {"version": "0.1.0", "url": "asset:p.so", "sha256": "cd", "size": 2},
      "mod": {"version": "0.1.0", "url": "asset:m.jar", "sha256": "ef", "size": 3},
      "fabric_api": {"version": "1", "url": "https://x/f.jar", "sha256": "01", "size": 4},
      "prism": {"version": "11.1.1", "url": "https://x/p.tar.gz", "sha256": "23", "size": 5}}"#;

    #[test]
    fn schema_one_from_before_windows_still_reads() {
        let m = Manifest::parse(OLD, "https://x/rel").unwrap();
        assert!(m.platforms.is_empty());
        if cfg!(windows) {
            // a Windows launcher finds no Windows files in an old release
            assert!(m.prism_asset().is_none() && m.plugin_asset().is_none() && m.launcher_asset().is_none());
        } else {
            assert_eq!(m.plugin_asset().unwrap().url, "https://x/rel/p.so");
            assert_eq!(m.prism_asset().unwrap().version, "11.1.1");
            assert_eq!(m.launcher_asset().unwrap().url, "https://x/rel/pl");
        }
    }

    #[test]
    fn windows_files_ride_in_platforms_and_old_launchers_ignore_them() {
        let with_win = OLD.replace("\"prism\":", "\"platforms\": {\"windows-x86_64\": {\"launcher\": {\"version\": \"0.2.0\", \"url\": \"asset:pl.exe\", \"sha256\": \"aa\", \"size\": 9}, \"prism\": {\"version\": \"11.1.1\", \"url\": \"https://x/p.zip\", \"sha256\": \"bb\", \"size\": 8}}}, \"prism\":");
        let m = Manifest::parse(&with_win, "https://x/rel").unwrap();
        let w = &m.platforms["windows-x86_64"];
        assert_eq!(w.launcher.as_ref().unwrap().url, "https://x/rel/pl.exe");
        assert!(w.plugin.is_none());
        if cfg!(windows) {
            assert_eq!(m.prism_asset().unwrap().url, "https://x/p.zip");
            assert!(m.plugin_asset().is_none(), "no Windows plugin yet: Play explains it");
        } else {
            assert_eq!(m.prism_asset().unwrap().url, "https://x/p.tar.gz");
            assert_eq!(m.plugin_asset().unwrap().version, "0.1.0");
        }
        // adding the plugin later is just one more entry
        let later = with_win.replace("\"launcher\": {\"version\": \"0.2.0\"", "\"plugin\": {\"version\": \"0.2.0\", \"url\": \"asset:p.dll\", \"sha256\": \"cc\", \"size\": 7}, \"launcher\": {\"version\": \"0.2.0\"");
        let m = Manifest::parse(&later, "https://x/rel").unwrap();
        assert_eq!(m.platforms["windows-x86_64"].plugin.as_ref().unwrap().url, "https://x/rel/p.dll");
    }

    #[test]
    fn newer_schema_with_platform_launcher() {
        let t = r#"{"schema": 99, "launcher": {"version": "9.0.0", "url": "asset:lin", "sha256": "ab"}, "platforms": {"windows-x86_64": {"launcher": {"version": "9.0.0", "url": "asset:win.exe", "sha256": "cd"}}}}"#;
        let a = launcher_only(t, "https://x/rel").unwrap();
        assert_eq!(a.url, if cfg!(windows) { "https://x/rel/win.exe" } else { "https://x/rel/lin" });
    }
}
