//! The application-menu entry (Linux): makes Portalcraft show up in the desktop's app menu, and gives the taskbar its
//! icon (on Wayland the taskbar finds the icon through the entry whose name matches the window's app id, `portalcraft`).
//! Setup adds it once; Settings has a button to add it again.
#![cfg(unix)]
use crate::paths::Layout;
use crate::settings::State;
use std::path::PathBuf;

fn quote(p: &std::path::Path) -> String {
    let s = p.to_string_lossy();
    if s.contains(' ') { format!("\"{s}\"") } else { s.to_string() }
}

/// ~/.local/share/applications (or $XDG_DATA_HOME/applications).
fn applications_dir() -> Option<PathBuf> {
    match std::env::var_os("XDG_DATA_HOME").filter(|v| !v.is_empty()) {
        Some(x) => Some(PathBuf::from(x).join("applications")),
        None => std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share/applications")),
    }
}

pub fn add_menu_entry(l: &Layout) {
    let (Ok(exe), Some(dir)) = (crate::util::exe_path(), applications_dir()) else { return };
    let _ = std::fs::create_dir_all(&dir);
    let env = if std::env::var("PORTALCRAFT_DATA").is_ok() { format!("env PORTALCRAFT_DATA={} ", quote(&l.data)) } else { String::new() };
    let icon = l.data.join("portalcraft.png");
    let _ = crate::icon::write_png(&icon);
    let text = format!("[Desktop Entry]\nType=Application\nName=Portalcraft\nComment=Play Portal 2 as a Minecraft player\nExec={env}{}\nIcon={}\nTerminal=false\nCategories=Game;\nStartupWMClass=portalcraft\n", quote(&exe), icon.display());
    if std::fs::write(dir.join("portalcraft.desktop"), text).is_ok() {
        crate::util::log(&format!("added Portalcraft to the application menu ({})", dir.join("portalcraft.desktop").display()));
        let mut s = State::load(l);
        s.shortcut_made = true;
        s.save(l);
    }
}
