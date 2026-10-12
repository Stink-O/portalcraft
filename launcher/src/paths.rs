//! Where everything lives. One folder per player: ~/.local/share/portalcraft on Linux, %LOCALAPPDATA%\Portalcraft on
//! Windows (or $PORTALCRAFT_DATA on both).
//!
//! ```text
//! <data>/.portalcraft-root        marker: the plugin finds <data> by walking up from its .so
//! <data>/plugin/portalcraft_bridge.so          (Windows: portalcraft_bridge.dll)
//! <data>/prism/                   portable Prism Launcher (portable.txt inside: all of Prism's data stays here)
//! <data>/prism/instances/Portalcraft/{instance.cfg,mmc-pack.json,.minecraft/{mods,saves,logs}}
//! <data>/cache/bridge/            bridge.log, bugmarks/, portalcraft_world.env, ... (the plugin's CacheDir())
//! <data>/cache/minecraft-26.3/... the unpacked Minecraft textures (the plugin's MinecraftAssetsDir())
//! <data>/downloads/               verified downloads (kept for retry)
//! <data>/logs/                    launcher.log, prism.log
//! <data>/tools/                   the optional map-building scripts
//! <data>/{settings.json,state.json,launcher.json}
//! ```
use std::path::{Path, PathBuf};

pub const INSTANCE_ID: &str = "Portalcraft";
pub const MC_VERSION: &str = "26.3";

#[cfg(unix)]
pub const PLUGIN_FILE: &str = "portalcraft_bridge.so";
#[cfg(windows)]
pub const PLUGIN_FILE: &str = "portalcraft_bridge.dll";
#[cfg(unix)]
pub const PRISM_EXE: &str = "PrismLauncher";
#[cfg(windows)]
pub const PRISM_EXE: &str = "prismlauncher.exe";

/// A path as the games and the JVM want to read it in command lines and cfg files: on Windows with forward slashes
/// (Portal 2 and Java accept them, and nothing needs escaping in Prism's INI); on Linux unchanged.
pub fn slashed(p: &Path) -> String {
    let s = p.to_string_lossy().to_string();
    if cfg!(windows) { s.replace('\\', "/") } else { s }
}

#[derive(Clone, Debug)]
pub struct Layout {
    pub data: PathBuf,
}

impl Layout {
    pub fn resolve() -> Layout {
        if let Ok(p) = std::env::var("PORTALCRAFT_DATA") {
            if !p.is_empty() {
                return Layout { data: PathBuf::from(p) };
            }
        }
        #[cfg(unix)]
        {
            let base = match std::env::var("XDG_DATA_HOME") {
                Ok(x) if !x.is_empty() => PathBuf::from(x),
                _ => PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| "/tmp".into())).join(".local/share"),
            };
            Layout { data: base.join("portalcraft") }
        }
        #[cfg(windows)]
        {
            let base = match std::env::var("LOCALAPPDATA") {
                Ok(x) if !x.is_empty() => PathBuf::from(x),
                _ => crate::util::home_dir().join("AppData").join("Local"),
            };
            Layout { data: base.join("Portalcraft") }
        }
    }
    pub fn marker(&self) -> PathBuf { self.data.join(".portalcraft-root") }
    pub fn plugin_dir(&self) -> PathBuf { self.data.join("plugin") }
    /// The Portal 2 plugin: a 32-bit .so on Linux, a .dll on Windows (platforms.windows-x86_64.plugin in the release list).
    pub fn plugin_so(&self) -> PathBuf { self.plugin_dir().join(PLUGIN_FILE) }
    pub fn prism_dir(&self) -> PathBuf { self.data.join("prism") }
    pub fn prism_bin(&self) -> PathBuf { self.prism_dir().join(PRISM_EXE) }
    pub fn prism_accounts(&self) -> PathBuf { self.prism_dir().join("accounts.json") }
    pub fn instance_dir(&self) -> PathBuf { self.prism_dir().join("instances").join(INSTANCE_ID) }
    pub fn game_dir(&self) -> PathBuf { self.instance_dir().join(".minecraft") }
    /// The Minecraft world the mod plays in (saves/Portalcraft).
    pub fn world_dir(&self) -> PathBuf { self.game_dir().join("saves").join(INSTANCE_ID) }
    pub fn mods_dir(&self) -> PathBuf { self.game_dir().join("mods") }
    pub fn mc_latest_log(&self) -> PathBuf { self.game_dir().join("logs").join("latest.log") }
    pub fn cache_dir(&self) -> PathBuf { self.data.join("cache") }
    pub fn bridge_dir(&self) -> PathBuf { self.cache_dir().join("bridge") }
    pub fn bridge_log(&self) -> PathBuf { self.bridge_dir().join("bridge.log") }
    /// Written by the plugin at each campaign level start: "<map>\n<unix time>". Drives "Continue".
    pub fn progress_file(&self) -> PathBuf { self.bridge_dir().join("progress.txt") }
    pub fn bugmarks_dir(&self) -> PathBuf { self.bridge_dir().join("bugmarks") }
    pub fn mc_assets_dir(&self) -> PathBuf {
        self.cache_dir().join(format!("minecraft-{MC_VERSION}/resourcepacks/local/minecraft-{MC_VERSION}/assets/minecraft"))
    }
    pub fn downloads_dir(&self) -> PathBuf { self.data.join("downloads") }
    pub fn logs_dir(&self) -> PathBuf { self.data.join("logs") }
    pub fn launcher_log(&self) -> PathBuf { self.logs_dir().join("launcher.log") }
    pub fn prism_log(&self) -> PathBuf { self.logs_dir().join("prism.log") }
    pub fn tools_dir(&self) -> PathBuf { self.data.join("tools") }
    pub fn settings_path(&self) -> PathBuf { self.data.join("settings.json") }
    pub fn state_path(&self) -> PathBuf { self.data.join("state.json") }
    pub fn manifest_cache(&self) -> PathBuf { self.data.join("launcher.json") }
    /// Prism's client jar libraries folder for the pinned Minecraft version.
    pub fn mc_client_jar_dir(&self) -> PathBuf {
        self.prism_dir().join("libraries/com/mojang/minecraft").join(MC_VERSION)
    }

    pub fn ensure_dirs(&self) -> std::io::Result<()> {
        for d in [self.data.clone(), self.plugin_dir(), self.bridge_dir(), self.downloads_dir(), self.logs_dir()] {
            std::fs::create_dir_all(d)?;
        }
        let m = self.marker();
        if !m.exists() {
            std::fs::write(&m, "Portalcraft data folder. The plugin finds this folder through this file; do not delete it.\n")?;
        }
        Ok(())
    }
}

#[allow(dead_code)]
pub fn quote_arg(p: &Path) -> String {
    let s = p.to_string_lossy().to_string();
    if s.contains(' ') { format!("\"{s}\"") } else { s }
}
