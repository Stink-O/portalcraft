//! User settings (settings.json) and the small installed-state file (state.json).
use crate::paths::Layout;
use crate::util::write_atomic;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Settings {
    /// Folder that contains portal2_linux. Empty: find it through Steam.
    pub portal2_dir: String,
    /// true: a random world on the first launch, kept afterwards. false: always use `seed`.
    pub seed_random: bool,
    pub seed: String,
    /// "mc" (Minecraft player) or "chell"
    pub start_mode: String,
    /// Offline mode: no update checks, no manifest download (setup still needs the files it has not got yet).
    pub offline: bool,
    pub memory_mb: u32,
    pub render_distance: u32,
    pub mute: bool,
    /// Map for "Play the campaign" (Valve's names). Empty: the first level.
    pub start_map: String,
    /// Where the release manifest is read from. Empty: the project's GitHub release.
    pub manifest_url: String,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            portal2_dir: String::new(),
            seed_random: true,
            seed: String::new(),
            start_mode: "mc".into(),
            offline: false,
            memory_mb: 6144,
            render_distance: 24,
            mute: false,
            start_map: String::new(),
            manifest_url: String::new(),
        }
    }
}

/// The launcher's files live in a separate repository, so the main release page only shows the launchers players download.
pub const DEFAULT_MANIFEST_URL: &str = "https://github.com/Stink-O/portalcraft-updates/releases/latest/download/launcher.json";

impl Settings {
    pub fn load(l: &Layout) -> Settings {
        std::fs::read_to_string(l.settings_path()).ok().and_then(|s| serde_json::from_str(&s).ok()).unwrap_or_default()
    }
    pub fn save(&self, l: &Layout) {
        let _ = write_atomic(&l.settings_path(), serde_json::to_string_pretty(self).unwrap().as_bytes());
    }
    pub fn manifest_url(&self) -> String {
        if !self.manifest_url.trim().is_empty() {
            return self.manifest_url.trim().to_string();
        }
        if let Ok(u) = std::env::var("PORTALCRAFT_MANIFEST_URL") {
            if !u.is_empty() {
                return u;
            }
        }
        DEFAULT_MANIFEST_URL.to_string()
    }
    /// The seed to force, if the player chose a fixed one and typed digits.
    pub fn forced_seed(&self) -> Option<i64> {
        if self.seed_random { None } else { self.seed.trim().parse::<i64>().ok() }
    }
}

/// What the launcher has installed (versions and hashes) so it knows what to update or repair.
#[derive(Serialize, Deserialize, Clone, Debug, Default)]
#[serde(default)]
pub struct State {
    /// component name -> installed "version sha256"
    pub installed: BTreeMap<String, String>,
    /// pid and start time of the Minecraft this launcher started (it may stop that one; never another)
    pub mc_pid: u32,
    pub mc_starttime: u64,
    pub setup_done: bool,
    pub shortcut_made: bool,
    /// The player says they are signed in although Prism's accounts file was not recognised (safety valve).
    pub signin_override: bool,
    pub last_notice_version: String,
    /// The prismlauncher:// handler that was the default before sign-in borrowed it ("-" = none); put back after sign-in.
    pub prev_url_handler: Option<String>,
}

impl State {
    pub fn load(l: &Layout) -> State {
        std::fs::read_to_string(l.state_path()).ok().and_then(|s| serde_json::from_str(&s).ok()).unwrap_or_default()
    }
    pub fn save(&self, l: &Layout) {
        let _ = write_atomic(&l.state_path(), serde_json::to_string_pretty(self).unwrap().as_bytes());
    }
}
