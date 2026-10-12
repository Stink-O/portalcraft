//! The launcher window (egui). Everything it does is a Task running on a worker thread that reports Events.
use crate::engine::*;
use crate::manifest::Manifest;
use crate::paths::Layout;
use crate::play::{self, PlayKind, PlayOpts};
use crate::portal2::{self, Portal2Error};
use crate::prism;
use crate::settings::{Settings, State};
use crate::util::{self, log};
use eframe::egui::{self, Color32, RichText};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::Arc;
use std::time::{Duration, Instant};

// Portal 2's test-chamber look: off-white panels, dark grey type, thin rules, the two portal colours as the only accents.
const BG: Color32 = Color32::from_rgb(0xe4, 0xe6, 0xe9);
const PANEL: Color32 = Color32::from_rgb(0xfb, 0xfb, 0xfc);
const PANEL2: Color32 = Color32::from_rgb(0xef, 0xf0, 0xf2);
const LINE: Color32 = Color32::from_rgb(0xc5, 0xc9, 0xcf);
const TEXT: Color32 = Color32::from_rgb(0x22, 0x24, 0x28);
const DIM: Color32 = Color32::from_rgb(0x6e, 0x73, 0x7b);
/// Portal orange: highlights and warnings.
const ACCENT: Color32 = Color32::from_rgb(0xe8, 0x82, 0x0c);
const GREEN: Color32 = Color32::from_rgb(0x2f, 0x7d, 0x4f);
const RED: Color32 = Color32::from_rgb(0xc2, 0x33, 0x2b);
/// Portal blue: the main action.
const BLUE: Color32 = Color32::from_rgb(0x1f, 0x7a, 0xd6);
const SEMI: &str = "barlow-semibold";
const MED: &str = "barlow-medium";

#[derive(Clone, Debug)]
pub enum TaskKind { Setup, Play(PlayOpts), Maps, Verify, SelfUpdate }

enum Outcome { Setup(Manifest), Play, Maps, Verify(Vec<String>), SelfUpdate(PathBuf) }

enum Msg {
    Ev(Event),
    Done(Result<Outcome, UserError>),
    Boot { manifest: Option<Manifest>, note: String },
}

#[derive(Clone, Copy, PartialEq)]
enum Screen { Home, Settings }

struct StepRow { id: StepId, state: StepState, detail: String, progress: Option<(u64, u64)> }

struct Task {
    kind: Option<TaskKind>,
    steps: Vec<StepRow>,
    running: bool,
    visible: bool,
    error: Option<UserError>,
    prompt: Prompt,
    done: Option<String>,
    lines: Vec<String>,
    cancel: Arc<AtomicBool>,
}

impl Task {
    fn new() -> Task {
        Task { kind: None, steps: vec![], running: false, visible: false, error: None, prompt: Prompt::None, done: None, lines: vec![], cancel: Arc::new(AtomicBool::new(false)) }
    }
}

pub struct ShotPlan {
    pub dir: PathBuf,
    pub names: Vec<&'static str>,
    pub idx: usize,
    frames: u32,
    requested: bool,
}

impl ShotPlan {
    pub fn new(dir: PathBuf) -> Self {
        // Windows has no shared-memory screen; its last screen is the "Windows plugin missing" message instead
        let last = if cfg!(windows) { "play_error_noplugin" } else { "play_error_shm" };
        ShotPlan { dir, names: vec!["welcome", "setup_downloading", "setup_signin", "setup_error_offline", "home", "home_update", "settings", "play_linking", "play_ready", last], idx: 0, frames: 0, requested: false }
    }
}

pub struct App {
    layout: Layout,
    settings: Settings,
    saved: Settings,
    screen: Screen,
    task: Task,
    tx: Sender<Msg>,
    rx: Receiver<Msg>,
    manifest: Option<Manifest>,
    updates: Option<Updates>,
    manifest_note: String,
    p2: Result<PathBuf, Portal2Error>,
    installed: Vec<(&'static str, bool)>,
    maps_built: bool,
    /// Newest campaign save, for "Continue".
    save: Option<portal2::ContinuePoint>,
    /// "New game" asked for confirmation; and whether it also makes a fresh Minecraft world.
    confirm_new: bool,
    new_world: bool,
    confirm_delete_worlds: bool,
    /// The Portalcraft wordmark (docs/branding), loaded once.
    logo: Option<Option<egui::TextureHandle>>,
    mc_running: bool,
    p2_running: bool,
    last_refresh: Instant,
    bug: Option<Result<PathBuf, String>>,
    restart_exe: Option<PathBuf>,
    start_map_ok: bool,
    shot: Option<ShotPlan>,
    /// support/test helpers: --autostart setup|play, --snap <seconds> <png> (one real screenshot, then quit)
    pub autostart: Option<String>,
    pub snap: Option<(f64, PathBuf, bool)>,
    started_at: Instant,
    demo: bool,
    demo_installed: bool,
    ctx: egui::Context,
}

fn install_style(ctx: &egui::Context) {
    // Barlow (SIL Open Font License, assets/fonts/OFL.txt): a DIN-like face close to Portal 2's signage
    let mut fonts = egui::FontDefinitions::default();
    for (name, bytes) in [
        ("barlow", &include_bytes!("../assets/fonts/Barlow-Regular.ttf")[..]),
        (MED, &include_bytes!("../assets/fonts/Barlow-Medium.ttf")[..]),
        (SEMI, &include_bytes!("../assets/fonts/Barlow-SemiBold.ttf")[..]),
    ] {
        fonts.font_data.insert(name.into(), std::sync::Arc::new(egui::FontData::from_static(bytes)));
    }
    fonts.families.entry(egui::FontFamily::Proportional).or_default().insert(0, "barlow".into());
    for n in [MED, SEMI] {
        let mut fam = vec![n.to_string()];
        fam.extend(fonts.families[&egui::FontFamily::Proportional].iter().skip(1).cloned());
        fonts.families.insert(egui::FontFamily::Name(n.into()), fam);
    }
    ctx.set_fonts(fonts);

    let mut v = egui::Visuals::light();
    v.panel_fill = BG;
    v.window_fill = PANEL;
    v.extreme_bg_color = Color32::WHITE;
    v.faint_bg_color = PANEL2;
    v.override_text_color = Some(TEXT);
    let r = egui::CornerRadius::same(2);
    for w in [&mut v.widgets.noninteractive, &mut v.widgets.inactive, &mut v.widgets.hovered, &mut v.widgets.active, &mut v.widgets.open] {
        w.corner_radius = r;
        w.expansion = 0.0;
    }
    v.widgets.noninteractive.bg_fill = PANEL;
    v.widgets.noninteractive.bg_stroke = egui::Stroke::new(1.0, LINE);
    v.widgets.inactive.bg_fill = PANEL;
    v.widgets.inactive.weak_bg_fill = PANEL;
    v.widgets.inactive.bg_stroke = egui::Stroke::new(1.0, LINE);
    v.widgets.hovered.bg_fill = PANEL2;
    v.widgets.hovered.weak_bg_fill = PANEL2;
    v.widgets.hovered.bg_stroke = egui::Stroke::new(1.0, BLUE);
    v.widgets.active.bg_fill = Color32::from_rgb(0xe2, 0xe5, 0xe9);
    v.widgets.active.weak_bg_fill = Color32::from_rgb(0xe2, 0xe5, 0xe9);
    v.widgets.active.bg_stroke = egui::Stroke::new(1.0, BLUE);
    v.selection.bg_fill = Color32::from_rgb(0xcf, 0xe3, 0xf8);
    v.selection.stroke = egui::Stroke::new(1.0, BLUE);
    ctx.set_visuals(v);
    ctx.global_style_mut(|s| {
        s.spacing.item_spacing = egui::vec2(8.0, 7.0);
        s.spacing.button_padding = egui::vec2(14.0, 6.0);
        s.spacing.interact_size.y = 28.0;
        s.text_styles.insert(egui::TextStyle::Body, egui::FontId::proportional(15.0));
        s.text_styles.insert(egui::TextStyle::Button, egui::FontId::new(15.0, egui::FontFamily::Name(MED.into())));
    });
}

/// The wordmark shown in the header (rendered from docs/branding/portalcraft-wordmark.svg).
fn load_logo(ctx: &egui::Context) -> Option<egui::TextureHandle> {
    let dec = png::Decoder::new(std::io::Cursor::new(&include_bytes!("../assets/branding/wordmark.png")[..]));
    let mut r = dec.read_info().ok()?;
    let mut buf = vec![0; r.output_buffer_size()];
    let info = r.next_frame(&mut buf).ok()?;
    if info.color_type != png::ColorType::Rgba || info.bit_depth != png::BitDepth::Eight { return None; }
    let img = egui::ColorImage::from_rgba_unmultiplied([info.width as usize, info.height as usize], &buf[..info.buffer_size()]);
    Some(ctx.load_texture("portalcraft-logo", img, egui::TextureOptions::LINEAR))
}

/// Heading text: Barlow SemiBold, upper case, spaced out like the chamber signs.
fn caps(t: &str, size: f32) -> RichText {
    RichText::new(t.to_uppercase()).family(egui::FontFamily::Name(SEMI.into())).size(size).extra_letter_spacing(size * 0.08)
}

fn semi(t: impl Into<String>, size: f32) -> RichText { RichText::new(t).family(egui::FontFamily::Name(SEMI.into())).size(size) }

impl App {
    pub fn new(ctx: egui::Context, layout: Layout, shot: Option<ShotPlan>) -> App {
        install_style(&ctx);
        let (tx, rx) = channel();
        let settings = Settings::load(&layout);
        let mut app = App {
            layout: layout.clone(), saved: settings.clone(), settings, screen: Screen::Home, task: Task::new(), tx, rx,
            manifest: None, updates: None, manifest_note: String::new(), p2: Err(Portal2Error::NoSteam), installed: vec![], maps_built: false, save: None, confirm_new: false, new_world: false, confirm_delete_worlds: false, logo: None,
            mc_running: false, p2_running: false, last_refresh: Instant::now() - Duration::from_secs(10), bug: None, restart_exe: None,
            start_map_ok: true, shot, autostart: None, snap: None, started_at: Instant::now(), demo: false, demo_installed: true, ctx,
        };
        app.refresh();
        if app.shot.is_none() { app.boot(); } else { app.demo = true; }
        app
    }

    /// Startup work: a leftover temporary autoexec from a crashed run is cleaned up, then the manifest is checked.
    fn boot(&mut self) {
        if let Ok(p2) = &self.p2 {
            if util::portal2_pids().is_empty() {
                if let Some(m) = crate::autoexec::recover(p2) { log(&format!("start-up: {m}")); }
            }
        }
        let (tx, ctx, l, s) = (self.tx.clone(), self.ctx.clone(), self.layout.clone(), self.settings.clone());
        std::thread::spawn(move || {
            let (manifest, note) = if s.offline {
                (crate::manifest::cached(&l), "Offline mode: not checking for updates.".to_string())
            } else {
                match crate::manifest::fetch(&l, &s.manifest_url()) {
                    Ok(m) => (Some(m), String::new()),
                    Err(e) => (crate::manifest::cached(&l), match e {
                        crate::manifest::FetchError::Net(n) => format!("Could not check for updates ({n})."),
                        crate::manifest::FetchError::Bad(b) => b,
                        crate::manifest::FetchError::NeedsNewerLauncher(a) => format!("Portalcraft {} needs a newer launcher: Settings, 'Update the launcher'.", a.version),
                    }),
                }
            };
            let _ = tx.send(Msg::Boot { manifest, note });
            ctx.request_repaint();
        });
    }

    fn refresh(&mut self) {
        self.p2 = portal2::find(&self.settings.portal2_dir);
        self.installed = if self.demo { vec![] } else { installed_report(&self.layout) };
        self.maps_built = self.p2.as_ref().map(|p| crate::maps::built(p)).unwrap_or(false);
        self.save = if self.demo { None } else {
            std::fs::read_to_string(self.layout.progress_file()).ok()
                .and_then(|t| portal2::continue_point(&t, self.p2.as_ref().ok().and_then(|p| portal2::latest_campaign_save(p))))
        };
        self.mc_running = !prism::minecraft_procs(&self.layout).is_empty();
        self.p2_running = !util::portal2_pids().is_empty();
        self.start_map_ok = self.settings.start_map.trim().is_empty() || play::valid_map_name(self.settings.start_map.trim());
        if let (Some(m), false) = (&self.manifest, self.demo) { self.updates = Some(check_updates(&self.layout, m)); }
        self.last_refresh = Instant::now();
    }

    fn is_installed(&self) -> bool { if self.demo { self.demo_installed } else { self.installed.iter().all(|(_, ok)| *ok) } }

    fn start(&mut self, kind: TaskKind) {
        if self.task.running { return; }
        let steps: Vec<StepId> = match &kind {
            TaskKind::Setup => SETUP_STEPS.to_vec(),
            TaskKind::Play(_) => play::PLAY_STEPS_LIST.to_vec(),
            TaskKind::Maps => vec![StepId::Maps],
            TaskKind::Verify => vec![StepId::Verify],
            TaskKind::SelfUpdate => vec![StepId::Verify],
        };
        self.task = Task::new();
        self.task.cancel = Arc::new(AtomicBool::new(false));
        self.task.steps = steps.into_iter().map(|id| StepRow { id, state: StepState::Pending, detail: String::new(), progress: None }).collect();
        self.task.kind = Some(kind.clone());
        self.task.running = true;
        self.task.visible = true;
        self.bug = None;
        let (tx, ctx) = (self.tx.clone(), self.ctx.clone());
        let sink_tx = tx.clone();
        let sink_ctx = ctx.clone();
        let sink: Arc<dyn Fn(Event) + Send + Sync> = Arc::new(move |e| { let _ = sink_tx.send(Msg::Ev(e)); sink_ctx.request_repaint(); });
        let rep = Reporter::new(sink, self.task.cancel.clone());
        let cx = Ctx { layout: self.layout.clone(), settings: self.settings.clone(), rep };
        let manifest = self.manifest.clone();
        std::thread::spawn(move || {
            let res: Result<Outcome, UserError> = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| match kind {
                TaskKind::Setup => run_setup(&cx).map(Outcome::Setup),
                TaskKind::Play(o) => play::run_play(&cx, &o).map(|_| Outcome::Play),
                TaskKind::Maps => match manifest.or_else(|| crate::manifest::cached(&cx.layout)) {
                    Some(m) => crate::maps::build(&cx, &m).map(|_| Outcome::Maps),
                    None => step_manifest(&cx).and_then(|m| crate::maps::build(&cx, &m)).map(|_| Outcome::Maps),
                },
                TaskKind::Verify => step_manifest(&cx).and_then(|m| verify_files(&cx, &m)).map(Outcome::Verify),
                TaskKind::SelfUpdate => crate::update::wanted_launcher(&cx).and_then(|a| crate::update::self_update(&cx, &a)).map(Outcome::SelfUpdate),
            })).unwrap_or_else(|_| Err(UserError::new("Something went wrong inside the launcher", "An unexpected error stopped this task. Nothing was closed. Press RETRY; if it happens again send a bug report (it includes the launcher log).").hint(Hint::Logs)));
            let _ = tx.send(Msg::Done(res));
            ctx.request_repaint();
        });
    }

    fn pump(&mut self) {
        while let Ok(m) = self.rx.try_recv() {
            match m {
                Msg::Boot { manifest, note } => {
                    self.manifest_note = note;
                    if let Some(m) = &manifest { self.updates = Some(check_updates(&self.layout, m)); }
                    self.manifest = manifest;
                }
                Msg::Ev(e) => self.on_event(e),
                Msg::Done(res) => {
                    self.task.running = false;
                    self.task.prompt = Prompt::None;
                    match res {
                        Ok(o) => {
                            self.task.done = Some(match o {
                                Outcome::Setup(m) => { self.manifest = Some(m); "Everything is installed.".into() }
                                Outcome::Play => "READY".into(),
                                Outcome::Maps => "The pit_flings demo map is installed.".into(),
                                Outcome::Verify(bad) => if bad.is_empty() { "All installed files are intact.".into() } else { format!("Repaired: {}. Run the setup again to download them.", bad.join(", ")) },
                                Outcome::SelfUpdate(exe) => { self.restart_exe = Some(exe); "The launcher was updated.".into() }
                            });
                        }
                        Err(e) => self.task.error = Some(e),
                    }
                    self.refresh();
                }
            }
        }
    }

    fn on_event(&mut self, e: Event) {
        match e {
            Event::Step { id, state, detail } => {
                if let Some(r) = self.task.steps.iter_mut().find(|r| r.id == id) {
                    r.state = state;
                    if !detail.is_empty() || state != StepState::Running { r.detail = detail; }
                    if state == StepState::Done || state == StepState::Skipped || state == StepState::Failed { r.progress = None; }
                }
            }
            Event::Progress { id, done, total } => {
                if let Some(r) = self.task.steps.iter_mut().find(|r| r.id == id) { r.progress = if total > 0 { Some((done, total)) } else { None }; }
            }
            Event::Log(l) => { self.task.lines.push(l); if self.task.lines.len() > 200 { self.task.lines.remove(0); } }
            Event::Prompt(p) => self.task.prompt = p,
        }
    }

    // ------------------------------------------------------------ drawing

    fn header(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            let logo = self.logo.get_or_insert_with(|| load_logo(ui.ctx())).clone();
            match logo {
                Some(t) => { ui.add(egui::Image::new(&t).fit_to_exact_size(egui::vec2(t.size()[0] as f32 * 54.0 / t.size()[1] as f32, 54.0))); }
                None => { ui.label(caps("Portalcraft", 30.0).color(TEXT)); }
            }
            ui.add_space(4.0);
            ui.label(RichText::new(format!("v{}", env!("CARGO_PKG_VERSION"))).color(DIM).size(13.0));
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                status(ui, "Portal 2", self.p2.is_ok(), match &self.p2 { Ok(_) => "found", Err(_) => "not found" });
                ui.add_space(14.0);
                let signed = self.installed.iter().find(|(n, _)| *n == "Signed in").map(|x| x.1).unwrap_or(self.demo && self.demo_installed);
                status(ui, "Minecraft account", signed, if signed { "signed in" } else { "sign-in needed" });
            });
        });
        portal_rule(ui);
    }

    fn footer(&mut self, ui: &mut egui::Ui) {
        ui.add_space(4.0);
        ui.horizontal(|ui| {
            if ui.button("Send a bug report").on_hover_text("Zips the logs (no passwords) into your home folder").clicked() {
                self.bug = Some(crate::bugreport::create(&self.layout, &self.settings, &util::home_dir()).map_err(|e| e.to_string()));
            }
            if ui.button("Open logs folder").clicked() { util::open_path(&self.layout.logs_dir()); }
            if self.screen != Screen::Settings || self.task.visible {
                if ui.button("Settings").clicked() { self.task.visible = self.task.visible && self.task.running; self.screen = Screen::Settings; }
            } else if ui.button("Back").clicked() { self.screen = Screen::Home; }
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if self.settings.offline { ui.label(RichText::new("Offline mode").color(ACCENT)); }
                else if !self.manifest_note.is_empty() { ui.label(RichText::new(&self.manifest_note).color(DIM).size(12.0)); }
            });
        });
        match &self.bug {
            Some(Ok(p)) => {
                let p = p.clone();
                ui.horizontal(|ui| {
                    ui.colored_label(GREEN, "Bug report saved:");
                    ui.label(p.display().to_string());
                    if ui.small_button("Show folder").clicked() { if let Some(d) = p.parent() { util::open_path(d); } }
                });
                ui.label(RichText::new("Attach this zip to your report on GitHub. It holds logs, no passwords or sign-in tokens.").color(DIM).size(12.0));
            }
            Some(Err(e)) => { ui.colored_label(RED, format!("Could not write the bug report: {e}")); }
            None => {}
        }
    }

    fn welcome(&mut self, ui: &mut egui::Ui) {
        card(ui, |ui| {
            ui.label(caps("Welcome", 15.0).color(DIM));
            ui.label(semi("Let's get Portal 2 and Minecraft talking.", 22.0));
            ui.add_space(4.0);
            ui.label("Portalcraft runs your real Portal 2 and your real Minecraft side by side. This one-time setup gets everything ready. It takes a few minutes and needs the internet.");
            ui.add_space(6.0);
            ui.label(caps("You need", 13.0).color(DIM));
            let needs: [&str; 3] = if cfg!(windows) {
                ["Portal 2, installed in Steam (you must own it)", "Minecraft: Java Edition on a Microsoft account (you must own it)", "about 1.5 GB of free disk space"]
            } else {
                ["Portal 2, installed in Steam (the Linux version, you must own it)", "Minecraft: Java Edition on a Microsoft account (you must own it)", "about 1.5 GB of free disk space and 300 MB of free /dev/shm"]
            };
            for t in needs { ui.label(format!("\u{2014}  {t}")); }
            ui.add_space(6.0);
            ui.label(caps("What happens", 13.0).color(DIM));
            for t in [
                "1. Portalcraft finds Portal 2 and downloads Prism Launcher (the program that signs you in to Minecraft).",
                "2. A Prism window opens once: you sign in with your Microsoft account there. Portalcraft never sees your password.",
                "3. Prism downloads Minecraft and Java. Portalcraft unpacks Minecraft's textures from your own copy.",
            ] { ui.label(format!("   {t}")); }
            ui.add_space(10.0);
            if primary_button(ui, "Set up Portalcraft").clicked() { self.start(TaskKind::Setup); }
        });
    }

    fn task_view(&mut self, ui: &mut egui::Ui) {
        let kind = self.task.kind.clone();
        let title = match &kind {
            Some(TaskKind::Setup) => "Setting up",
            Some(TaskKind::Play(_)) => "Starting the game",
            Some(TaskKind::Maps) => "Building the demo map",
            Some(TaskKind::Verify) => "Checking files",
            Some(TaskKind::SelfUpdate) => "Updating the launcher",
            None => "",
        };
        ui.label(caps(title, 18.0));
        // sign-in prompt
        if matches!(self.task.prompt, Prompt::SignIn) && self.task.running {
            ui.add_space(8.0);
            card_colored(ui, BLUE, |ui| {
                ui.label(semi("Sign in with your Microsoft account", 18.0));
                ui.label("A Prism Launcher window will open. In it: click the account button in the top right, choose \"Manage Accounts\", then \"Add Microsoft\". Prism shows a short code: open microsoft.com/link in your browser, type the code and sign in with the account that owns Minecraft: Java Edition. When you are done, come back here: this window notices by itself.");
                ui.label(RichText::new("If your browser asks which app should open a \"prismlauncher://\" link, press Cancel and use the code instead.").color(DIM).size(12.0));
                ui.label(RichText::new("Prism is the official-style open source launcher many people use; the sign-in happens on Microsoft's page, never in Portalcraft.").color(DIM).size(12.0));
                ui.add_space(4.0);
                ui.horizontal(|ui| {
                    if primary_button(ui, "Open Prism to sign in").clicked() {
                        if let Err(e) = prism::open_window(&self.layout) { self.task.error = Some(UserError::new("Prism would not open", &e)); }
                    }
                    if ui.button("Skip for now").clicked() { self.task.cancel.store(true, Ordering::Relaxed); }
                    if ui.button("I signed in, continue").on_hover_text("Use this only if you did sign in and this window does not notice").clicked() {
                        let mut st = State::load(&self.layout);
                        st.signin_override = true;
                        st.save(&self.layout);
                    }
                });
            });
        }
        if matches!(self.task.prompt, Prompt::PrismWorking) && self.task.running {
            ui.add_space(8.0);
            card_colored(ui, BLUE, |ui| {
                ui.label("Prism is downloading Minecraft and Java now. This can take several minutes. Minecraft starts hidden in the background when it is done; that is normal.");
                if ui.button("Open Prism window").clicked() { let _ = prism::open_window(&self.layout); }
            });
        }
        // error with RETRY
        if let Some(e) = self.task.error.clone() {
            ui.add_space(8.0);
            card_colored(ui, RED, |ui| {
                ui.label(semi(&e.title, 18.0).color(RED));
                ui.label(&e.body);
                ui.add_space(6.0);
                ui.horizontal(|ui| {
                    if e.hint == Hint::Setup {
                        if primary_button(ui, "Continue the setup").clicked() { self.task.error = None; self.start(TaskKind::Setup); }
                    } else if primary_button(ui, "RETRY").clicked() { if let Some(k) = kind.clone() { self.task.error = None; self.start(k); } }
                    match e.hint {
                        Hint::Settings => { if ui.button("Open Settings").clicked() { self.task.visible = false; self.screen = Screen::Settings; } }
                        Hint::Prism => { if ui.button("Open Prism").clicked() { let _ = prism::open_window(&self.layout); } }
                        Hint::Logs => { if ui.button("Open logs folder").clicked() { util::open_path(&self.layout.logs_dir()); } }
                        Hint::Steam | Hint::Setup | Hint::None => {}
                    }
                    if ui.button("Close").clicked() { self.task.visible = false; self.task.error = None; }
                });
            });
        }
        if let Some(d) = self.task.done.clone() {
            ui.add_space(8.0);
            let is_play = matches!(kind, Some(TaskKind::Play(_)));
            card_colored(ui, GREEN, |ui| {
                if is_play {
                    ui.label(caps("Ready", 14.0).color(GREEN));
                    ui.label(semi("Portal 2 and Minecraft are linked.", 20.0));
                    ui.label("Switch to the Portal 2 window and play. Minecraft is hidden on purpose and closes by itself when Portal 2 does.");
                    ui.add_space(4.0);
                    keys_list(ui);
                } else {
                    ui.label(semi(&d, 18.0).color(GREEN));
                }
                ui.add_space(6.0);
                ui.horizontal(|ui| {
                    if let Some(exe) = self.restart_exe.clone() {
                        if primary_button(ui, "Restart the launcher").clicked() { crate::update::restart(&exe); std::process::exit(0); }
                    }
                    if ui.button(if is_play { "Done" } else { "Continue" }).clicked() { self.task.visible = false; self.task.done = None; }
                });
            });
        }
        ui.add_space(8.0);
        card(ui, |ui| {
            let n = self.task.steps.len();
            for (i, r) in self.task.steps.iter().enumerate() {
                step_row(ui, i + 1, r);
                if i + 1 < n { thin_rule(ui); }
            }
        });
        if self.task.running {
            ui.add_space(8.0);
            if ui.button("Stop").on_hover_text("Stops waiting. Nothing is closed.").clicked() { self.task.cancel.store(true, Ordering::Relaxed); }
        }
    }

    fn home(&mut self, ui: &mut egui::Ui) {
        if let Some(u) = &self.updates {
            if u.launcher.is_some() || !u.components.is_empty() {
                let mut go_launcher = false;
                let mut go_components = false;
                card_colored(ui, BLUE, |ui| {
                    ui.horizontal(|ui| {
                        ui.label(semi("A Portalcraft update is ready", 16.0));
                        if let Some(v) = &u.launcher { ui.label(format!("launcher {v}")); }
                        if !u.components.is_empty() { ui.label(format!("new: {}", u.components.join(", "))); }
                        if !self.settings.offline {
                            if u.launcher.is_some() && ui.button("Update the launcher").clicked() { go_launcher = true; }
                            if !u.components.is_empty() && ui.button("Update now").clicked() { go_components = true; }
                        }
                    });
                });
                if go_launcher { self.start(TaskKind::SelfUpdate); }
                if go_components { self.start(TaskKind::Setup); }
                ui.add_space(6.0);
            }
        }
        if plugin_missing(&self.layout) && !self.demo {
            card_colored(ui, ACCENT, |ui| {
                ui.label(semi("The Portal 2 plugin for Windows is missing", 16.0));
                ui.label("The release list this launcher read has no Windows plugin. The release may still be uploading: try again in a few minutes, or use Settings, \"Check and repair installed files\". Setup, sign-in and updates work.");
            });
            ui.add_space(6.0);
        }
        card(ui, |ui| {
            ui.label(caps("Play", 15.0).color(DIM));
            ui.add_space(2.0);
            let blocked = self.p2_running && !self.mc_running;
            let can_play = !self.task.running && self.start_map_ok;
            let mc_mode = self.settings.start_mode != "chell";
            ui.horizontal(|ui| {
                ui.label("Start as:");
                if toggle(ui, mc_mode, "Minecraft player").clicked() { self.settings.start_mode = "mc".into(); }
                if toggle(ui, !mc_mode, "Chell").clicked() { self.settings.start_mode = "chell".into(); }
                ui.label(RichText::new("(J switches in game)").color(DIM).size(12.0));
            });
            ui.add_space(6.0);
            let as_mc = self.settings.start_mode != "chell";
            let save = self.save.clone();
            let mut go: Option<PlayOpts> = None;
            ui.horizontal(|ui| {
                let big = |t: &str| egui::Button::new(caps(t, 20.0).color(Color32::WHITE)).fill(BLUE).stroke(egui::Stroke::NONE).min_size(egui::vec2(260.0, 54.0));
                let small = |t: &str| egui::Button::new(caps(t, 14.0)).min_size(egui::vec2(170.0, 54.0));
                if let Some(sv) = &save {
                    if ui.add_enabled(can_play, big("Continue")).clicked() {
                        go = Some(PlayOpts { kind: PlayKind::Campaign, map: Some(sv.map.clone()), start_as_mc: as_mc, load: sv.save.clone(), reset_world: false });
                    }
                    if ui.add_enabled(can_play, small("New game")).clicked() { self.confirm_new = true; self.new_world = false; }
                } else if ui.add_enabled(can_play, big("Play the campaign")).clicked() {
                    go = Some(PlayOpts { kind: PlayKind::Campaign, map: None, start_as_mc: as_mc, load: None, reset_world: false });
                }
                let demo_ready = self.maps_built || self.demo;
                let r = ui.add_enabled(can_play && demo_ready, small("Pit flings demo"));
                if r.clicked() { go = Some(PlayOpts { kind: PlayKind::PitFlings, map: None, start_as_mc: as_mc, load: None, reset_world: false }); }
                if !demo_ready { r.on_disabled_hover_text(if cfg!(windows) { "The demo map can only be built on Linux for now" } else { "Build the demo map once in Settings (optional)" }); }
            });
            let map = if self.settings.start_map.trim().is_empty() { "the first level".to_string() } else { self.settings.start_map.trim().to_string() };
            match &save {
                Some(sv) => ui.label(RichText::new(format!("Continue: {} ({}, {}). New game starts at {map}.", sv.map, if sv.save.is_some() { "from your last save" } else { "from the start of the level" }, ago(sv.when))).color(DIM).size(12.5)),
                None => ui.label(RichText::new(format!("The campaign starts at {map}. You can pick another level in Settings.")).color(DIM).size(12.5)),
            };
            if self.confirm_new {
                ui.add_space(6.0);
                card_colored(ui, ACCENT, |ui| {
                    ui.label(semi("Start the campaign again?", 17.0));
                    ui.label(format!("Portal 2 starts at {map}. Your saves stay, but Portal 2's autosaves will be replaced as you play, so Continue will follow the new game."));
                    ui.checkbox(&mut self.new_world, "Also start a fresh Minecraft world (new random seed). The old world is kept as a backup folder, not deleted.");
                    let new_world = self.new_world;
                    let mut cancel = false;
                    ui.horizontal(|ui| {
                        let b = egui::Button::new(caps("Start new game", 13.0).color(Color32::WHITE)).fill(BLUE).stroke(egui::Stroke::NONE).min_size(egui::vec2(150.0, 32.0));
                        if ui.add_enabled(can_play, b).clicked() {
                            go = Some(PlayOpts { kind: PlayKind::Campaign, map: None, start_as_mc: as_mc, load: None, reset_world: new_world });
                        }
                        if ui.button("Cancel").clicked() { cancel = true; }
                    });
                    if cancel { self.confirm_new = false; }
                });
            }
            if let Some(o) = go { self.confirm_new = false; self.start(TaskKind::Play(o)); }
            if blocked { ui.colored_label(ACCENT, "Portal 2 is already open without Portalcraft. Close it first."); }
            ui.add_space(6.0);
            thin_rule(ui);
            ui.horizontal(|ui| {
                ui.label(caps("World", 12.0).color(DIM));
                ui.label(match self.settings.forced_seed() { Some(s) => format!("always seed {s}"), None => "random seed, created on the first launch and kept".into() });
                ui.label(RichText::new("(change in Settings)").color(DIM).size(12.0));
            });
            ui.horizontal(|ui| {
                ui.label(caps("Minecraft", 12.0).color(DIM));
                if self.mc_running { ui.label("running hidden in the background (it will be reused)"); } else { ui.colored_label(DIM, "not running (Play starts it)"); }
            });
            if !self.start_map_ok { ui.colored_label(RED, "The level name in Settings is not valid (letters, numbers, underscores)."); }
        });
        ui.add_space(10.0);
        card(ui, |ui| {
            ui.label(caps("Controls", 15.0).color(DIM));
            ui.add_space(2.0);
            keys_list(ui);
        });
    }

    fn settings_view(&mut self, ui: &mut egui::Ui) {
        egui::ScrollArea::vertical().show(ui, |ui| {
            card(ui, |ui| {
                ui.label(caps("Settings", 18.0));
                ui.add_space(6.0);
                ui.label(caps("Portal 2", 13.0).color(DIM));
                ui.horizontal(|ui| {
                    ui.label("Folder (empty: found through Steam):");
                    ui.add(egui::TextEdit::singleline(&mut self.settings.portal2_dir).desired_width(340.0).hint_text(if cfg!(windows) { r"C:\Program Files (x86)\Steam\steamapps\common\Portal 2" } else { "/path/to/Portal 2" }));
                });
                match &self.p2 {
                    Ok(p) => { ui.colored_label(GREEN, format!("Using {}", p.display())); }
                    Err(e) => { ui.colored_label(RED, portal2_error(e).title); }
                }
                ui.add_space(6.0);
                ui.label(caps("World", 13.0).color(DIM));
                ui.horizontal(|ui| {
                    ui.radio_value(&mut self.settings.seed_random, true, "Random (new world on first launch, then kept)");
                });
                ui.horizontal(|ui| {
                    ui.radio_value(&mut self.settings.seed_random, false, "Fixed seed:");
                    ui.add_enabled(!self.settings.seed_random, egui::TextEdit::singleline(&mut self.settings.seed).desired_width(180.0).hint_text("12345"));
                    if !self.settings.seed_random && self.settings.seed.trim().parse::<i64>().is_err() { ui.colored_label(RED, "digits only"); }
                });
                ui.label(RichText::new("A fixed seed saves the world as Portalcraft_<seed>; your random world is never touched.").color(DIM).size(12.0));
                ui.add_space(6.0);
                ui.label(caps("Game", 13.0).color(DIM));
                ui.horizontal(|ui| {
                    ui.label("Start as:");
                    ui.radio_value(&mut self.settings.start_mode, "mc".to_string(), "Minecraft player");
                    ui.radio_value(&mut self.settings.start_mode, "chell".to_string(), "Chell");
                });
                ui.horizontal(|ui| {
                    ui.label("Campaign start level:");
                    ui.add(egui::TextEdit::singleline(&mut self.settings.start_map).desired_width(220.0).hint_text("sp_a1_intro1 (the first level)"));
                });
                ui.checkbox(&mut self.settings.mute, "Mute Minecraft's sound");
                ui.horizontal(|ui| {
                    ui.label("Minecraft memory (MB):");
                    ui.add(egui::DragValue::new(&mut self.settings.memory_mb).range(2048..=32768).speed(64));
                    ui.label("View distance:");
                    ui.add(egui::DragValue::new(&mut self.settings.render_distance).range(6..=32));
                });
                ui.label(RichText::new("Changes reach Minecraft the next time it starts. PLAY restarts a Minecraft this launcher started when its settings differ.").color(DIM).size(12.0));
                ui.add_space(6.0);
                ui.label(caps("Updates", 13.0).color(DIM));
                ui.checkbox(&mut self.settings.offline, "Offline mode (no update checks, no downloads of the file list)");
                ui.horizontal(|ui| {
                    ui.label("Release list URL:");
                    ui.add(egui::TextEdit::singleline(&mut self.settings.manifest_url).desired_width(380.0).hint_text(crate::settings::DEFAULT_MANIFEST_URL));
                });
                ui.add_space(8.0);
                ui.label(caps("Tools", 13.0).color(DIM));
                ui.horizontal_wrapped(|ui| {
                    let idle = !self.task.running;
                    if ui.add_enabled(idle, egui::Button::new("Check and repair installed files")).clicked() { self.screen = Screen::Home; self.start(TaskKind::Verify); }
                    if ui.add_enabled(idle, egui::Button::new("Run the setup again")).clicked() { self.screen = Screen::Home; self.start(TaskKind::Setup); }
                    if ui.button("Open Prism (accounts, Minecraft profile)").clicked() { let _ = prism::open_window(&self.layout); }
                    if ui.button("Open the data folder").clicked() { util::open_path(&self.layout.data); }
                    #[cfg(unix)]
                    if ui.button("Add to the application menu").clicked() { make_shortcut(&self.layout); }
                    if ui.add_enabled(idle && !self.settings.offline, egui::Button::new("Update the launcher")).clicked() { self.screen = Screen::Home; self.start(TaskKind::SelfUpdate); }
                });
                let old = play::old_worlds(&self.layout);
                if !old.is_empty() {
                    ui.add_space(6.0);
                    ui.label(caps("Old Minecraft worlds", 13.0).color(DIM));
                    let bytes: u64 = old.iter().map(|(_, b)| *b).sum();
                    ui.label(format!("{} world{} kept from \"New game\", {:.1} GB in total.", old.len(), if old.len() == 1 { "" } else { "s" }, bytes as f64 / 1e9));
                    ui.horizontal(|ui| {
                        if !self.confirm_delete_worlds {
                            if ui.button("Delete old worlds").clicked() { self.confirm_delete_worlds = true; }
                        } else {
                            ui.colored_label(RED, "Delete them for good?");
                            if ui.button("Yes, delete").clicked() {
                                for (p, _) in &old { let _ = std::fs::remove_dir_all(p); }
                                log(&format!("deleted {} old Minecraft world(s)", old.len()));
                                self.confirm_delete_worlds = false;
                            }
                            if ui.button("Cancel").clicked() { self.confirm_delete_worlds = false; }
                        }
                        if ui.button("Show them").clicked() { util::open_path(&self.layout.world_dir().with_file_name("")); }
                    });
                }
                if cfg!(unix) {
                    ui.add_space(6.0);
                    ui.label(caps("Demo map (optional)", 13.0).color(DIM));
                    ui.label(RichText::new("The pit_flings demo is built from your own copy of Portal 2 with Portal 2's own map tools (needs python3). The campaign does not need it.").color(DIM).size(12.0));
                    let idle = !self.task.running;
                    if ui.add_enabled(idle, egui::Button::new(if self.maps_built { "Rebuild the demo map" } else { "Build the demo map" })).clicked() { self.screen = Screen::Home; self.start(TaskKind::Maps); }
                }
                ui.add_space(6.0);
                ui.label(RichText::new(format!("Data folder: {}", self.layout.data.display())).color(DIM).size(12.0));
            });
        });
    }

    fn apply_demo(&mut self, name: &str) {
        let mk = |ids: &[(StepId, StepState, &str)]| -> Vec<StepRow> { ids.iter().map(|(id, s, d)| StepRow { id: *id, state: *s, detail: d.to_string(), progress: None }).collect() };
        self.task = Task::new();
        self.screen = Screen::Home;
        self.demo_installed = true;
        self.updates = None;
        self.bug = None;
        self.settings = Settings::default();
        let demo_p2 = if cfg!(windows) { r"C:\Program Files (x86)\Steam\steamapps\common\Portal 2" } else { "/home/player/.local/share/Steam/steamapps/common/Portal 2" };
        self.p2 = Ok(PathBuf::from(demo_p2));
        let pre_signin = matches!(name, "welcome" | "setup_downloading" | "setup_signin" | "setup_error_offline");
        self.installed = vec![("Signed in", !pre_signin)];
        let mut setup_done = vec![(StepId::Portal2, StepState::Done, demo_p2), (StepId::Shm, StepState::Done, "30.1 GB free"), (StepId::Manifest, StepState::Done, "release 0.1.0")];
        if cfg!(windows) { setup_done.remove(1); }
        match name {
            "welcome" => { self.demo_installed = false; }
            "setup_downloading" => {
                self.demo_installed = false;
                self.task.visible = true; self.task.running = true; self.task.kind = Some(TaskKind::Setup);
                let mut rows = mk(&setup_done);
                rows.push(StepRow { id: StepId::Prism, state: StepState::Running, detail: "Prism Launcher: 41.2 MB of 64.0 MB".into(), progress: Some((41_200_000, 64_000_000)) });
                for id in [StepId::Plugin, StepId::Instance, StepId::SignIn, StepId::Minecraft, StepId::Textures] { rows.push(StepRow { id, state: StepState::Pending, detail: String::new(), progress: None }); }
                self.task.steps = rows;
            }
            "setup_signin" => {
                self.demo_installed = false;
                self.task.visible = true; self.task.running = true; self.task.kind = Some(TaskKind::Setup);
                let mut rows = mk(&setup_done);
                rows.extend(mk(&[(StepId::Prism, StepState::Done, "Prism 11.1.1 (3112 files)"), (StepId::Plugin, if cfg!(windows) { StepState::Skipped } else { StepState::Done }, if cfg!(windows) { "this release has no Windows plugin yet" } else { "plugin 0.1.0 installed" }), (StepId::Instance, StepState::Done, "Minecraft 26.3, Fabric, mod 0.1.0"), (StepId::SignIn, StepState::Waiting, "waiting for you to sign in")]));
                for id in [StepId::Minecraft, StepId::Textures] { rows.push(StepRow { id, state: StepState::Pending, detail: String::new(), progress: None }); }
                self.task.steps = rows;
                self.task.prompt = Prompt::SignIn;
            }
            "setup_error_offline" => {
                self.demo_installed = false;
                self.task.visible = true; self.task.kind = Some(TaskKind::Setup);
                let mut rows = mk(&setup_done);
                rows.push(StepRow { id: StepId::Prism, state: StepState::Failed, detail: "Can't reach the internet".into(), progress: None });
                for id in [StepId::Plugin, StepId::Instance, StepId::SignIn, StepId::Minecraft, StepId::Textures] { rows.push(StepRow { id, state: StepState::Pending, detail: String::new(), progress: None }); }
                self.task.steps = rows;
                self.task.error = Some(net_error(crate::util::NetError::Offline("dns error: failed to lookup address information".into()), "Prism Launcher").at(StepId::Prism));
            }
            "home" => {
                self.save = Some(portal2::ContinuePoint { map: "sp_a2_laser_stairs".into(), save: Some("autosave".into()), when: std::time::SystemTime::now() - Duration::from_secs(3 * 3600) });
            }
            "home_update" => {
                self.updates = Some(Updates { launcher: Some("0.1.1".into()), components: vec!["plugin", "Minecraft mod"] });
                self.mc_running = true;
            }
            "settings" => { self.screen = Screen::Settings; self.maps_built = false; }
            "play_linking" => {
                self.task.visible = true; self.task.running = true; self.task.kind = Some(TaskKind::Play(PlayOpts { kind: PlayKind::Campaign, map: None, start_as_mc: true, load: None, reset_world: false }));
                self.task.steps = mk(&[(StepId::PlayPrepare, StepState::Done, "ready; map sp_a1_intro1"), (StepId::PlayMinecraft, StepState::Done, "reusing the running Minecraft (pid 48211)"), (StepId::PlayPortal2, StepState::Done, "Portal 2 is running (pid 48790), level sp_a1_intro1 loaded"), (StepId::PlayLink, StepState::Running, "waiting (34s): link not yet")]);
            }
            "play_ready" => {
                self.task.visible = true; self.task.kind = Some(TaskKind::Play(PlayOpts { kind: PlayKind::Campaign, map: None, start_as_mc: true, load: None, reset_world: false }));
                self.task.steps = mk(&[(StepId::PlayPrepare, StepState::Done, "ready; map sp_a1_intro1"), (StepId::PlayMinecraft, StepState::Done, "reusing the running Minecraft (pid 48211)"), (StepId::PlayPortal2, StepState::Done, "Portal 2 is running (pid 48790), level sp_a1_intro1 loaded"), (StepId::PlayLink, StepState::Done, "linked")]);
                self.task.done = Some("READY".into());
            }
            "play_error_noplugin" => {
                self.task.visible = true; self.task.kind = Some(TaskKind::Play(PlayOpts { kind: PlayKind::Campaign, map: None, start_as_mc: true, load: None, reset_world: false }));
                self.task.steps = mk(&[(StepId::PlayPrepare, StepState::Failed, NO_PLUGIN_TITLE), (StepId::PlayMinecraft, StepState::Pending, ""), (StepId::PlayPortal2, StepState::Pending, ""), (StepId::PlayLink, StepState::Pending, "")]);
                self.task.error = Some(no_plugin_error().at(StepId::PlayPrepare));
            }
            "play_error_shm" => {
                self.task.visible = true; self.task.kind = Some(TaskKind::Play(PlayOpts { kind: PlayKind::Campaign, map: None, start_as_mc: true, load: None, reset_world: false }));
                self.task.steps = mk(&[(StepId::PlayPrepare, StepState::Failed, "Shared memory is full"), (StepId::PlayMinecraft, StepState::Pending, ""), (StepId::PlayPortal2, StepState::Pending, ""), (StepId::PlayLink, StepState::Pending, "")]);
                self.task.error = Some(shm_full_error(120 * 1024 * 1024).at(StepId::PlayPrepare));
            }
            _ => {}
        }
    }

    fn drive_screenshots(&mut self, ctx: &egui::Context) {
        let Some(mut plan) = self.shot.take() else { return };
        if plan.idx >= plan.names.len() { ctx.send_viewport_cmd(egui::ViewportCommand::Close); self.shot = Some(plan); return; }
        let name = plan.names[plan.idx];
        if plan.frames == 0 { self.apply_demo(name); }
        plan.frames += 1;
        if plan.frames == 8 && !plan.requested {
            ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::default()));
            plan.requested = true;
        }
        let got = ctx.input(|i| i.events.iter().find_map(|e| if let egui::Event::Screenshot { image, .. } = e { Some(image.clone()) } else { None }));
        if let Some(img) = got {
            let _ = std::fs::create_dir_all(&plan.dir);
            let path = plan.dir.join(format!("{:02}_{}.png", plan.idx + 1, name));
            save_png(&path, &img);
            println!("screenshot {}", path.display());
            plan.idx += 1; plan.frames = 0; plan.requested = false;
        }
        ctx.request_repaint();
        self.shot = Some(plan);
    }
}

fn save_png(path: &std::path::Path, img: &egui::ColorImage) {
    let (w, h) = (img.size[0] as u32, img.size[1] as u32);
    let mut data = Vec::with_capacity((w * h * 4) as usize);
    for p in &img.pixels { data.extend_from_slice(&[p.r(), p.g(), p.b(), p.a()]); }
    if let Ok(f) = std::fs::File::create(path) {
        let mut enc = png::Encoder::new(std::io::BufWriter::new(f), w, h);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        if let Ok(mut wr) = enc.write_header() { let _ = wr.write_image_data(&data); }
    }
}

#[cfg(unix)]
fn make_shortcut(l: &Layout) { crate::menu::add_menu_entry(l); }

/// "5 minutes ago", "3 days ago".
fn ago(t: std::time::SystemTime) -> String {
    let s = std::time::SystemTime::now().duration_since(t).map(|d| d.as_secs()).unwrap_or(0);
    let (n, u) = match s { 0..=89 => return "just now".into(), 90..=5399 => (s / 60, "minute"), 5400..=129599 => (s / 3600, "hour"), _ => (s / 86400, "day") };
    format!("{n} {u}{} ago", if n == 1 { "" } else { "s" })
}

// ---------------------------------------------------------------- widgets

fn card(ui: &mut egui::Ui, f: impl FnOnce(&mut egui::Ui)) {
    egui::Frame::new().fill(PANEL).stroke(egui::Stroke::new(1.0, LINE)).corner_radius(2).inner_margin(egui::Margin::same(16)).show(ui, |ui| { ui.set_width(ui.available_width()); f(ui); });
}

/// A card with a coloured strip down its left edge (blue: info, orange: attention, red: error, green: done).
fn card_colored(ui: &mut egui::Ui, c: Color32, f: impl FnOnce(&mut egui::Ui)) {
    let resp = egui::Frame::new().fill(PANEL).stroke(egui::Stroke::new(1.0, LINE)).corner_radius(2)
        .inner_margin(egui::Margin { left: 20, right: 16, top: 14, bottom: 14 })
        .show(ui, |ui| { ui.set_width(ui.available_width()); f(ui); });
    let r = resp.response.rect;
    ui.painter().rect_filled(egui::Rect::from_min_max(r.min, egui::pos2(r.min.x + 5.0, r.max.y)), 0.0, c);
}

fn primary_button(ui: &mut egui::Ui, text: &str) -> egui::Response {
    ui.add(egui::Button::new(caps(text, 13.0).color(Color32::WHITE)).fill(BLUE).stroke(egui::Stroke::NONE).min_size(egui::vec2(130.0, 32.0)))
}

/// The header's thin rule: half portal blue, half portal orange.
fn portal_rule(ui: &mut egui::Ui) {
    ui.add_space(4.0);
    let (r, _) = ui.allocate_exact_size(egui::vec2(ui.available_width(), 3.0), egui::Sense::hover());
    let mid = r.center().x;
    ui.painter().rect_filled(egui::Rect::from_min_max(r.min, egui::pos2(mid, r.max.y)), 0.0, BLUE);
    ui.painter().rect_filled(egui::Rect::from_min_max(egui::pos2(mid, r.min.y), r.max), 0.0, ACCENT);
}

fn thin_rule(ui: &mut egui::Ui) {
    let (r, _) = ui.allocate_exact_size(egui::vec2(ui.available_width(), 1.0), egui::Sense::hover());
    ui.painter().rect_filled(r, 0.0, PANEL2);
}

/// "PORTAL 2  found": a small caps label and a word. Problems are in orange; nothing blinks.
fn status(ui: &mut egui::Ui, name: &str, ok: bool, word: &str) {
    ui.label(RichText::new(word).size(14.0).color(if ok { TEXT } else { ACCENT }).family(egui::FontFamily::Name(MED.into())));
    ui.label(caps(name, 11.0).color(DIM));
}

fn toggle(ui: &mut egui::Ui, on: bool, text: &str) -> egui::Response {
    let b = if on { egui::Button::new(RichText::new(text).color(Color32::WHITE)).fill(TEXT).stroke(egui::Stroke::NONE) } else { egui::Button::new(text) };
    ui.add(b)
}

/// The in-game keys: a keycap column and a description that wraps to the window.
fn keys_list(ui: &mut egui::Ui) {
    for (k, v) in play::keys_help() {
        ui.horizontal(|ui| {
            ui.allocate_ui_with_layout(egui::vec2(118.0, 24.0), egui::Layout::left_to_right(egui::Align::Center), |ui| { ui.set_min_width(118.0); keycap(ui, k); });
            let w = ui.available_width();
            ui.allocate_ui_with_layout(egui::vec2(w, 24.0), egui::Layout::top_down(egui::Align::LEFT), |ui| { ui.add(egui::Label::new(*v).wrap()); });
        });
    }
}

fn keycap(ui: &mut egui::Ui, k: &str) {
    egui::Frame::new().fill(PANEL2).stroke(egui::Stroke::new(1.0, LINE)).corner_radius(2).inner_margin(egui::Margin::symmetric(8, 2))
        .show(ui, |ui| { ui.label(semi(k, 13.0)); });
}

/// One numbered setup/play step: "03  Download Prism Launcher ........ DONE".
fn step_row(ui: &mut egui::Ui, n: usize, r: &StepRow) {
    let (word, col) = match r.state {
        StepState::Pending => ("", DIM),
        StepState::Running => ("working", BLUE),
        StepState::Waiting => ("waiting for you", ACCENT),
        StepState::Done => ("done", GREEN),
        StepState::Skipped => ("skipped", DIM),
        StepState::Failed => ("failed", RED),
    };
    let num_col = match r.state { StepState::Pending | StepState::Skipped => LINE, StepState::Done => DIM, _ => col };
    ui.horizontal(|ui| {
        ui.add_sized(egui::vec2(34.0, 26.0), egui::Label::new(semi(format!("{n:02}"), 20.0).color(num_col)));
        ui.vertical(|ui| {
            ui.horizontal(|ui| {
                let tcol = match r.state { StepState::Pending => DIM, StepState::Failed => RED, _ => TEXT };
                ui.label(RichText::new(r.id.title()).color(tcol).size(15.5).family(egui::FontFamily::Name(MED.into())));
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| { if !word.is_empty() { ui.label(caps(word, 11.0).color(col)); } });
            });
            if !r.detail.is_empty() && r.state != StepState::Pending { ui.label(RichText::new(&r.detail).color(DIM).size(13.0)); }
            if let Some((d, t)) = r.progress {
                let f = (d as f32 / t.max(1) as f32).clamp(0.0, 1.0);
                ui.horizontal(|ui| {
                    let (bar, _) = ui.allocate_exact_size(egui::vec2(380.0, 6.0), egui::Sense::hover());
                    ui.painter().rect_filled(bar, 0.0, PANEL2);
                    ui.painter().rect_filled(egui::Rect::from_min_size(bar.min, egui::vec2(bar.width() * f, bar.height())), 0.0, BLUE);
                    ui.label(RichText::new(format!("{:.0}%", f * 100.0)).color(DIM).size(13.0));
                });
            }
        });
    });
}

impl eframe::App for App {
    /// Closing the window mid-launch must not leave the temporary autoexec.cfg behind.
    fn on_exit(&mut self, _gl: Option<&eframe::glow::Context>) {
        self.task.cancel.store(true, Ordering::Relaxed);
        if let Ok(p2) = portal2::find(&self.settings.portal2_dir) {
            if let Some(m) = crate::autoexec::recover(&p2) { log(&format!("closing: {m}")); }
        }
        if let Some(m) = play::abandon_unlinked_minecraft(&self.layout) { log(&format!("closing: {m}")); }
    }

    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        self.pump();
        if self.shot.is_some() { self.drive_screenshots(&ctx); }
        if let Some(a) = self.autostart.take() {
            match a.as_str() {
                "setup" => self.start(TaskKind::Setup),
                "play" => self.start(TaskKind::Play(PlayOpts { kind: PlayKind::Campaign, map: None, start_as_mc: self.settings.start_mode != "chell", load: None, reset_world: false })),
                _ => {}
            }
        }
        if let Some((secs, path, requested)) = self.snap.clone() {
            if self.started_at.elapsed().as_secs_f64() >= secs && !requested {
                ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::default()));
                self.snap = Some((secs, path.clone(), true));
            }
            let got = ctx.input(|i| i.events.iter().find_map(|e| if let egui::Event::Screenshot { image, .. } = e { Some(image.clone()) } else { None }));
            if let (Some(img), true) = (got, requested) {
                save_png(&path, &img);
                println!("snapshot {}", path.display());
                self.on_exit(None);
                std::process::exit(0);
            }
        }
        if self.last_refresh.elapsed() > Duration::from_secs(3) && !self.demo { self.refresh(); }
        ctx.request_repaint_after(Duration::from_millis(500));
        egui::Panel::bottom("footer").frame(egui::Frame::new().fill(BG).inner_margin(egui::Margin::symmetric(14, 6))).show(ui, |ui| self.footer(ui));
        egui::CentralPanel::default().frame(egui::Frame::new().fill(BG).inner_margin(egui::Margin::same(14))).show(ui, |ui| {
            self.header(ui);
            ui.add_space(6.0);
            egui::ScrollArea::vertical().auto_shrink([false, false]).scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::VisibleWhenNeeded).show(ui, |ui| {
                if self.task.visible { self.task_view(ui); }
                else if !self.is_installed() && self.screen == Screen::Home { self.welcome(ui); }
                else if self.screen == Screen::Settings { self.settings_view(ui); }
                else { self.home(ui); }
            });
        });
        if self.settings != self.saved && !self.demo {
            self.settings.save(&self.layout);
            self.saved = self.settings.clone();
        }
    }
}
