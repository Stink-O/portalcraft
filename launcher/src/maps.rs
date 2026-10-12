//! Optional: build the pit_flings demo map from the player's own copy of Portal 2 (Valve's files are never shipped).
//! This runs maps/tools/build.sh --install from a small scripts bundle, with Portal 2's own Linux map compilers.
use crate::engine::*;
use crate::manifest::Manifest;
use crate::portal2;
use crate::util::{self, log};
use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};

pub fn built(p2: &std::path::Path) -> bool { portal2::maps_dir(p2).join("pc_pit_flings.bsp").exists() }

pub fn build(cx: &Ctx, m: &Manifest) -> R<()> {
    let r = &cx.rep;
    let id = StepId::Maps;
    let fail = |e: UserError| { r.step(id, StepState::Failed, &e.title); e };
    r.step(id, StepState::Running, "");
    if cfg!(windows) {
        return Err(fail(UserError::new("The demo map cannot be built on Windows yet", "Building it needs Portal 2's Linux map tools and python3 with bash. The campaign does not need it, so you can skip this.").at(id)));
    }
    let p2 = portal2::find(&cx.settings.portal2_dir).map_err(|e| fail(portal2_error(&e).at(id)))?;
    if !util::portal2_pids().is_empty() {
        return Err(fail(UserError::new("Close Portal 2 first", "The map is written into Portal 2's folder, which is not allowed while the game runs. Close Portal 2 and press RETRY.").at(id)));
    }
    if Command::new("python3").arg("--version").stdout(Stdio::null()).stderr(Stdio::null()).status().map(|s| !s.success()).unwrap_or(true) {
        return Err(fail(UserError::new("Python 3 is missing", "Building the demo map needs python3 (it ships with most Linux systems; Fedora: sudo dnf install python3, Ubuntu: sudo apt install python3). Install it, then press RETRY. The campaign does not need this.").at(id)));
    }
    if !p2.join("bin/linux32/vbsp_linux").exists() {
        return Err(fail(UserError::new("Portal 2's map compiler is missing", "The map is built with Portal 2's own Linux tools (bin/linux32/vbsp_linux), which this install does not have. The campaign does not need this map, so you can skip it.").at(id)));
    }
    let Some(a) = &m.map_tools else {
        return Err(fail(UserError::new("The map tools are not part of this release", "This release has no map-building scripts. Update the launcher or skip the demo.").at(id)));
    };
    let archive = fetch_asset(cx, id, a, "the map-building scripts").map_err(|e| fail(e))?;
    let dest = cx.layout.tools_dir();
    crate::prism::extract_prism(&archive, &dest).map_err(|e| fail(io_err("unpacking the map tools", e).at(id)))?;
    let script = dest.join("maps/tools/build.sh");
    r.note(id, "compiling (this takes a few minutes)");
    let mut child = Command::new("bash")
        .arg(&script).arg("--install")
        .env("PORTAL2_DIR", &p2)
        .current_dir(dest.join("maps/tools"))
        .stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped())
        .spawn().map_err(|e| fail(io_err("starting the map build", e).at(id)))?;
    let err = child.stderr.take();
    let tail = std::sync::Arc::new(std::sync::Mutex::new(Vec::<String>::new()));
    let t2 = tail.clone();
    let h = std::thread::spawn(move || { if let Some(e) = err { for l in BufReader::new(e).lines().flatten() { let mut t = t2.lock().unwrap(); t.push(l); if t.len() > 30 { t.remove(0); } } } });
    if let Some(out) = child.stdout.take() {
        for line in BufReader::new(out).lines().flatten() {
            log(&format!("maps: {line}"));
            tail.lock().unwrap().push(line.clone());
            if line.starts_with("== ") || line.starts_with("installed") { r.note(id, line.trim_start_matches("== ")); }
            if r.cancelled() { let _ = child.kill(); break; }
        }
    }
    let status = child.wait().map_err(|e| fail(io_err("waiting for the map build", e).at(id)))?;
    let _ = h.join();
    if !status.success() || !built(&p2) {
        let t = tail.lock().unwrap().iter().rev().take(6).rev().cloned().collect::<Vec<_>>().join("\n");
        return Err(fail(UserError::new("The map build failed", &format!("The last lines were:\n{t}\nThe campaign still works without this map. Press RETRY, or send a bug report.")).hint(Hint::Logs).at(id)));
    }
    r.step(id, StepState::Done, "pc_pit_flings.bsp installed into Portal 2");
    Ok(())
}
