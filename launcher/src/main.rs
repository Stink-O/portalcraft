// The release program on Windows is a window program: no console flashes up. --cli/--help/--screenshot attach to the
// console they were started from (see winsys::attach_parent_console) and --cli also writes logs\cli-output.txt.
#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

mod autoexec;
mod bugreport;
mod engine;
mod icon;
mod menu;
mod manifest;
mod maps;
mod paths;
mod play;
mod portal2;
mod prism;
mod settings;
mod ui;
mod update;
mod util;
#[cfg(windows)]
mod winsys;

use engine::*;
use paths::Layout;
use settings::Settings;
use std::sync::atomic::AtomicBool;
use std::sync::Arc;

macro_rules! out {
    ($($a:tt)*) => { util::cli_out(&format!($($a)*)) };
}

fn usage() {
    out!("Portalcraft launcher {}\n", env!("CARGO_PKG_VERSION"));
    out!("  portalcraft-launcher                      open the launcher window");
    out!("  portalcraft-launcher --screenshot <dir>   render every screen with demo data into <dir> and quit");
    out!("  portalcraft-launcher --autostart setup|play   start that task right away");
    out!("  portalcraft-launcher --snap <seconds> <png>   save one real screenshot after that many seconds, then quit");
    out!("  portalcraft-launcher --cli <command>      no window: detect | status | setup | verify | bugreport | selfupdate | maps | prism | procs | play");
    out!("\nEnvironment: PORTALCRAFT_DATA (data folder), PORTALCRAFT_MANIFEST_URL (release list), PORTAL2_DIR, PORTALCRAFT_P2_DIRECT (Windows, development: start that folder's portal2.exe instead of through Steam)");
    if cfg!(windows) { out!("Windows: the output is also written to <data folder>\\logs\\cli-output.txt (cmd.exe does not wait for a window program)."); }
}

fn cli_ctx(l: &Layout) -> Ctx {
    let sink: Arc<dyn Fn(Event) + Send + Sync> = Arc::new(|e| match e {
        Event::Step { id, state, detail } => out!("[{:?}] {} {}", state, id.title(), detail),
        Event::Progress { .. } | Event::Prompt(_) => {}
        Event::Log(l) => out!("  {l}"),
    });
    Ctx { layout: l.clone(), settings: Settings::load(l), rep: Reporter::new(sink, Arc::new(AtomicBool::new(false))) }
}

fn print_err(e: &UserError) -> i32 {
    out!("\n{}\n{}", e.title, e.body);
    1
}

fn cli(cmd: &str, l: &Layout) -> i32 {
    let cx = cli_ctx(l);
    let _ = l.ensure_dirs();
    match cmd {
        "detect" => match portal2::find(&cx.settings.portal2_dir) {
            Ok(p) => { out!("Portal 2: {}", p.display()); 0 }
            Err(e) => print_err(&portal2_error(&e)),
        },
        "status" => {
            for (n, ok) in installed_report(l) { out!("{:<20} {}", n, if ok { "ok" } else { "MISSING" }); }
            out!("portal2 running: {:?}", util::portal2_pids());
            out!("minecraft running: {:?}", prism::minecraft_procs(l).iter().map(|p| p.pid).collect::<Vec<_>>());
            0
        }
        "setup" => match run_setup(&cx) { Ok(_) => { out!("\nsetup complete"); 0 } Err(e) => print_err(&e) },
        "verify" => match step_manifest(&cx).and_then(|m| verify_files(&cx, &m)) { Ok(b) => { out!("repaired/missing: {b:?}"); 0 } Err(e) => print_err(&e) },
        "bugreport" => match bugreport::create(l, &cx.settings, std::path::Path::new(&util::home_dir().to_string_lossy().to_string())) { Ok(p) => { out!("{}", p.display()); 0 } Err(e) => { out!("failed: {e}"); 1 } },
        "selfupdate" => match update::wanted_launcher(&cx).and_then(|a| update::self_update(&cx, &a)) { Ok(p) => { out!("updated {}", p.display()); 0 } Err(e) => print_err(&e) },
        "maps" => match step_manifest(&cx).and_then(|m| maps::build(&cx, &m)) { Ok(_) => 0, Err(e) => print_err(&e) },
        "procs" => {
            // support: what the launcher sees when it looks for Portal 2, Minecraft and Prism
            for p in util::list_procs() {
                let n = p.comm.to_ascii_lowercase();
                if n.contains("java") || n.contains("portal2") || n.contains("prism") { out!("pid {} name {:?} start {} exe {:?} cmd {:?}", p.pid, p.comm, p.starttime, p.exe, p.cmdline); }
            }
            0
        }
        "prism" => match prism::open_window(l) { Ok(_) => { out!("Prism started"); 0 } Err(e) => { out!("{e}"); 1 } },
        "play" | "play-pit" => {
            let o = play::PlayOpts { kind: if cmd == "play" { play::PlayKind::Campaign } else { play::PlayKind::PitFlings }, map: None, start_as_mc: cx.settings.start_mode != "chell", load: None, reset_world: false };
            match play::run_play(&cx, &o) { Ok(_) => { out!("\nREADY"); 0 } Err(e) => print_err(&e) }
        }
        _ => { usage(); 2 }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let (Some("--icon"), Some(out)) = (args.first().map(String::as_str), args.get(1)) {
        // support/release helper: write the icon as a PNG
        if let Err(e) = icon::write_png(std::path::Path::new(out)) { eprintln!("{e}"); std::process::exit(1); }
        return;
    }
    #[cfg(windows)]
    {
        // a console program's output needs a console: attach to the one that started us, if any
        if args.iter().any(|a| matches!(a.as_str(), "--cli" | "--help" | "-h" | "--screenshot" | "--snap")) { winsys::attach_parent_console(); }
    }
    let layout = Layout::resolve();
    let _ = layout.ensure_dirs();
    #[cfg(windows)]
    {
        update::remove_old_exe();
        if args.iter().any(|a| a == "--cli") {
            let p = layout.logs_dir().join("cli-output.txt");
            let _ = std::fs::write(&p, format!("portalcraft-launcher {} {}\n", env!("CARGO_PKG_VERSION"), args.join(" ")));
            util::set_cli_output(p);
        }
    }
    util::init_log(layout.launcher_log());
    util::log(&format!("launcher {} started, data folder {}", env!("CARGO_PKG_VERSION"), layout.data.display()));
    if args.iter().any(|a| a == "--help" || a == "-h") { usage(); return; }
    let cli_cmd = args.iter().position(|a| a == "--cli").map(|i| args.get(i + 1).cloned().unwrap_or_default());
    // Only one launcher at a time per data folder: two would fight over the temporary autoexec.cfg, the downloads and
    // Prism's folder. Read-only commands and screenshots do not need it.
    let read_only = matches!(cli_cmd.as_deref(), Some("detect" | "status" | "bugreport" | "procs")) || args.iter().any(|a| a == "--screenshot");
    let _lock = if read_only { None } else {
        match util::lock_instance(&layout.data, std::time::Duration::from_secs(4)) {
            Ok(l) => l,
            Err(()) => {
                let msg = "Portalcraft is already open (another launcher window or --cli command is using this data folder). Use that one; look for its window or close it first.";
                eprintln!("{msg}");
                if cli_cmd.is_none() { util::notify("Portalcraft is already open", msg); }
                std::process::exit(3);
            }
        }
    };
    // from here on nothing else is writing: clean up after a launcher that was killed
    let gone = util::clean_leftovers(&layout, None);
    if !gone.is_empty() { util::log(&format!("removed leftovers of an interrupted run: {}", gone.join(", "))); }
    if prism::signed_in(&layout) { prism::release_url_handler(&layout); }
    if let Some(cmd) = cli_cmd {
        std::process::exit(cli(&cmd, &layout));
    }
    let shot_dir = args.iter().position(|a| a == "--screenshot").and_then(|i| args.get(i + 1)).map(std::path::PathBuf::from);
    let autostart = args.iter().position(|a| a == "--autostart").and_then(|i| args.get(i + 1)).cloned();
    let snap = args.iter().position(|a| a == "--snap").and_then(|i| Some((args.get(i + 1)?.parse::<f64>().ok()?, std::path::PathBuf::from(args.get(i + 2)?), false)));
    let options = eframe::NativeOptions {
        viewport: eframe::egui::ViewportBuilder::default()
            .with_inner_size([900.0, 680.0])
            .with_min_inner_size([700.0, 520.0])
            .with_title("Portalcraft")
            .with_app_id("portalcraft")
            .with_icon({ let (rgba, width, height) = icon::rgba(); std::sync::Arc::new(eframe::egui::IconData { rgba, width, height }) }),
        ..Default::default()
    };
    let layout_exit = layout.clone();
    let res = eframe::run_native("Portalcraft", options, Box::new(move |cc| {
        let plan = shot_dir.map(ui::ShotPlan::new);
        let mut app = ui::App::new(cc.egui_ctx.clone(), layout, plan);
        app.autostart = autostart;
        app.snap = snap;
        Ok(Box::new(app))
    }));
    if prism::signed_in(&layout_exit) { prism::release_url_handler(&layout_exit); }
    if let Err(e) = res {
        eprintln!("The launcher window could not open: {e}\nUse --cli commands in a terminal, or check your graphics drivers (OpenGL is needed).");
        #[cfg(windows)]
        winsys::message_box("Portalcraft", &format!("The launcher window could not open: {e}\n\nIt needs OpenGL 2 or newer: update your graphics driver (a remote desktop or a virtual machine often has none). The text commands still work: see portalcraft-launcher.exe --help."));
        std::process::exit(1);
    }
}
