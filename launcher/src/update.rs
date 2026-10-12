//! Launcher self-update: download the new binary named in launcher.json, verify it, and swap it in place.
use crate::engine::*;
use crate::util::log;
#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;

/// Downloads and installs the new launcher over the running executable. Returns its path (restart it to use it).
pub fn self_update(cx: &Ctx, launcher: &crate::manifest::Asset) -> R<PathBuf> {
    let r = &cx.rep;
    let id = StepId::Verify;
    r.step(id, StepState::Running, &format!("downloading launcher {}", launcher.version));
    let file = fetch_asset(cx, id, launcher, "the new Portalcraft launcher").map_err(|e| { r.step(id, StepState::Failed, &e.title); e })?;
    let exe = crate::util::exe_path().map_err(|e| io_err("finding the launcher program", e))?;
    #[cfg(windows)]
    let res = swap_in_place(&file, &exe);
    #[cfg(unix)]
    let new = exe.with_file_name(format!(".{}.new", exe.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_else(|| "portalcraft-launcher".into())));
    #[cfg(unix)]
    let res = (|| -> std::io::Result<()> {
        std::fs::copy(&file, &new)?;
        std::fs::set_permissions(&new, std::fs::Permissions::from_mode(0o755))?;
        std::fs::rename(&new, &exe)
    })();
    if let Err(e) = res {
        #[cfg(unix)]
        let _ = std::fs::remove_file(&new);
        let ue = UserError::new(
            "Could not replace the launcher",
            &format!("{e}. The new launcher is saved at {}. Copy it over {} yourself (or move the launcher to a folder you can write to), then start it again.", file.display(), exe.display()),
        ).at(id);
        r.step(id, StepState::Failed, &ue.title);
        return Err(ue);
    }
    log(&format!("launcher updated to {}", launcher.version));
    r.step(id, StepState::Done, &format!("launcher {} installed; restart to use it", launcher.version));
    Ok(exe)
}

/// The name the running program is moved to while its replacement takes its place (Windows).
pub fn old_exe_path(exe: &std::path::Path) -> PathBuf { exe.with_extension("old.exe") }

/// Windows: a running .exe cannot be overwritten or deleted, but it can be renamed. The running program is renamed to
/// portalcraft-launcher.old.exe, the new one is copied to the old name, and the .old.exe is deleted at the next start.
#[cfg(windows)]
fn swap_in_place(new_file: &std::path::Path, exe: &std::path::Path) -> std::io::Result<()> {
    let old = old_exe_path(exe);
    let _ = std::fs::remove_file(&old); // left by an earlier update; fails harmlessly while that one still runs
    std::fs::rename(exe, &old)?;
    if let Err(e) = std::fs::copy(new_file, exe) {
        let _ = std::fs::remove_file(exe);
        let _ = std::fs::rename(&old, exe);
        return Err(e);
    }
    Ok(())
}

/// Windows: deletes the .old.exe an earlier self-update left behind (call at start-up; harmless if it is not there).
#[allow(dead_code)]
pub fn remove_old_exe() {
    if let Ok(exe) = crate::util::exe_path() { let _ = std::fs::remove_file(old_exe_path(&exe)); }
}

/// The launcher to update to: from the release list, or from a newer release this launcher cannot fully read.
pub fn wanted_launcher(cx: &Ctx) -> R<crate::manifest::Asset> {
    if !cx.settings.offline {
        if let Err(crate::manifest::FetchError::NeedsNewerLauncher(a)) = crate::manifest::fetch(&cx.layout, &cx.settings.manifest_url()) { return Ok(a); }
    }
    step_manifest(cx).and_then(|m| m.launcher_asset().cloned().ok_or_else(|| UserError::new(
        "This release has no launcher for your system",
        "The release list (launcher.json) names no launcher program for this system, so there is nothing to update to. Try again after a newer release is published.",
    )))
}

/// Starts the (new) launcher and returns; the caller closes its own window.
pub fn restart(exe: &PathBuf) {
    let mut c = std::process::Command::new(exe);
    c.stdin(std::process::Stdio::null());
    let _ = c.spawn();
}
