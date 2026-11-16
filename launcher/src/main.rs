// No console window in release builds on Windows.
#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

mod model;
mod update;

use eframe::egui::{self, Color32, FontFamily, RichText, ViewportCommand};
use model::*;
use serde_json::Value;
use std::path::PathBuf;
use std::sync::mpsc;

// Colour tokens of the site (site/styles.css :root).
const INK: Color32 = Color32::from_rgb(0x07, 0x07, 0x0b);
const INK_2: Color32 = Color32::from_rgb(0x0e, 0x0e, 0x15);
const INK_3: Color32 = Color32::from_rgb(0x17, 0x17, 0x20);
const PAPER: Color32 = Color32::from_rgb(0xec, 0xe8, 0xe1);
const MUTED: Color32 = Color32::from_rgb(0x9b, 0x98, 0xa3);
const LINE: Color32 = Color32::from_rgb(0x1f, 0x1e, 0x25); // paper at 10% over ink
const RED: Color32 = Color32::from_rgb(0xd7, 0x17, 0x2a);
const RED_HI: Color32 = Color32::from_rgb(0xff, 0x33, 0x47);
const RED_DEEP: Color32 = Color32::from_rgb(0x6e, 0x0b, 0x14);
const LOGO: &[u8] = include_bytes!("../assets/logo.png");

fn cond() -> FontFamily {
    FontFamily::Name("cond".into())
}

fn load_logo() -> Option<egui::ColorImage> {
    let mut r = png::Decoder::new(std::io::Cursor::new(LOGO)).read_info().ok()?;
    let mut buf = vec![0; r.output_buffer_size()?];
    let info = r.next_frame(&mut buf).ok()?;
    (info.color_type == png::ColorType::Rgba && info.bit_depth == png::BitDepth::Eight).then(|| {
        egui::ColorImage::from_rgba_unmultiplied([info.width as usize, info.height as usize], &buf[..info.buffer_size()])
    })
}

fn style(ctx: &egui::Context) {
    let mut f = egui::FontDefinitions::default();
    let mut add = |name: &str, bytes: &'static [u8]| {
        f.font_data.insert(name.into(), std::sync::Arc::new(egui::FontData::from_static(bytes)));
    };
    add("barlow", include_bytes!("../assets/Barlow-Regular.ttf"));
    add("barlow_cond", include_bytes!("../assets/BarlowCondensed-Bold.ttf"));
    f.families.entry(FontFamily::Proportional).or_default().insert(0, "barlow".into());
    f.families.insert(cond(), vec!["barlow_cond".into(), "barlow".into()]);
    ctx.set_fonts(f);

    let mut v = egui::Visuals::dark();
    v.override_text_color = Some(PAPER);
    v.panel_fill = INK;
    v.window_fill = INK_2;
    v.extreme_bg_color = INK_2;
    v.faint_bg_color = INK_3;
    v.hyperlink_color = RED_HI;
    v.selection.bg_fill = RED;
    v.selection.stroke = egui::Stroke::new(1.0, PAPER);
    let sharp = egui::CornerRadius::ZERO; // the site is all angular cuts: no rounded corners
    v.window_corner_radius = sharp;
    v.menu_corner_radius = sharp;
    let w = &mut v.widgets;
    w.noninteractive.bg_stroke = egui::Stroke::new(1.0, LINE);
    w.noninteractive.corner_radius = sharp;
    for (s, fill) in [(&mut w.inactive, INK_3), (&mut w.hovered, INK_3), (&mut w.active, RED_DEEP), (&mut w.open, INK_3)] {
        s.bg_fill = fill;
        s.weak_bg_fill = fill;
        s.corner_radius = sharp;
        s.fg_stroke.color = PAPER;
    }
    w.inactive.bg_stroke = egui::Stroke::new(1.0, Color32::from_rgb(0x4f, 0x4d, 0x4c)); // paper at 25%
    w.hovered.bg_stroke = egui::Stroke::new(1.0, RED_HI);
    w.active.bg_stroke = egui::Stroke::new(1.0, RED_HI);
    ctx.set_visuals(v);
    ctx.all_styles_mut(|s| {
        s.spacing.item_spacing = egui::vec2(10.0, 9.0);
        s.spacing.button_padding = egui::vec2(12.0, 5.0);
        for (_, t) in s.text_styles.iter_mut() {
            t.size *= 1.06; // Barlow runs smaller than egui's default face
        }
    });
}

enum Msg {
    Found(update::Release),
    Done,
    Failed(String),
}

enum Upd {
    None,
    Available(update::Release),
    Working,
    Failed(String),
}

struct App {
    cfg: LauncherConfig,
    settings: GameSettings,
    doc: Value,
    dirty: bool,
    error: Option<String>,
    logo: Option<egui::TextureHandle>,
    upd: Upd,
    tx: mpsc::Sender<Msg>,
    rx: mpsc::Receiver<Msg>,
}

impl App {
    fn start_update(&mut self, r: update::Release, ctx: &egui::Context) {
        self.upd = Upd::Working;
        let (tx, ctx) = (self.tx.clone(), ctx.clone());
        std::thread::spawn(move || {
            let _ = tx.send(match update::download_and_apply(&r) {
                Ok(()) => Msg::Done,
                Err(e) => Msg::Failed(e),
            });
            ctx.request_repaint();
        });
    }

    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        style(&cc.egui_ctx);
        let logo = load_logo().map(|i| cc.egui_ctx.load_texture("logo", i, egui::TextureOptions::LINEAR));
        let mut cfg = load_launcher();
        if let Some(p) = find_game_exe(&cfg) {
            cfg.game_exe = p.to_string_lossy().into_owned();
        }
        let doc = load_game_doc();
        let settings = GameSettings::from_value(&doc, &cfg.games[0].serials);
        let last = log_dir().join("last_error.txt");
        let error = std::fs::read_to_string(&last).ok();
        let _ = std::fs::remove_file(last);
        let (tx, rx) = mpsc::channel();
        if cfg.check_updates {
            // One short-lived thread; it wakes the UI only if it found something.
            let (tx, ctx) = (tx.clone(), cc.egui_ctx.clone());
            std::thread::spawn(move || {
                if let Some(r) = update::check() {
                    let _ = tx.send(Msg::Found(r));
                    ctx.request_repaint();
                }
            });
        }
        Self { cfg, settings, doc, dirty: false, error, logo, upd: Upd::None, tx, rx }
    }

    fn save(&mut self) {
        let serials = self.cfg.games[0].serials.clone();
        self.settings.apply(&mut self.doc, &serials, self.cfg.resolution);
        if let Err(e) = save_game_doc(&self.doc).and_then(|_| save_launcher(&self.cfg)) {
            self.error = Some(format!("Could not save settings: {e}"));
        }
        self.dirty = false;
    }

    fn play(&mut self, ctx: &egui::Context) {
        self.error = None;
        self.save();
        let Some(exe) = find_game_exe(&self.cfg) else {
            self.error = Some(format!("{} not found: pick it under Game executable.", exe_name()));
            return;
        };
        let lang = self.settings.language.clone();
        let Some(cue) = disc_for(&self.cfg.games[0], &lang) else {
            let name = if lang == "ja" { "Japanese" } else { "English" };
            self.error = Some(format!("Set the {name} disc (.cue) first."));
            return;
        };
        let args = build_args(&game_settings_json().to_string_lossy(), cue, &self.cfg, &self.settings);
        // The UI process exits while the game runs (an OpenGL window costs ~100 MB even hidden); a
        // windowless watcher (`--run`, a few MB) waits for the game and reopens the launcher.
        let me = std::env::current_exe().unwrap_or_default();
        if let Err(e) = std::process::Command::new(me).arg("--run").arg(&exe).args(&args).spawn() {
            self.error = Some(format!("Could not start the game: {e}"));
            return;
        }
        ctx.send_viewport_cmd(ViewportCommand::Close);
    }
}

fn combo(ui: &mut egui::Ui, id: &str, cur: &mut String, opts: &[(&str, &str)]) -> bool {
    let shown = opts.iter().find(|o| o.0 == cur).map_or(cur.as_str(), |o| o.1).to_string();
    let mut changed = false;
    egui::ComboBox::from_id_salt(id).selected_text(shown).width(170.0).show_ui(ui, |ui| {
        for (v, label) in opts {
            changed |= ui.selectable_value(cur, v.to_string(), *label).changed();
        }
    });
    changed
}

fn path_row(ui: &mut egui::Ui, path: &mut String, pick: impl FnOnce() -> Option<PathBuf>) -> bool {
    let mut changed = false;
    ui.horizontal(|ui| {
        changed |= ui.add(egui::TextEdit::singleline(path).desired_width(250.0)).changed();
        if ui.button("Browse").clicked() {
            if let Some(p) = pick() {
                *path = p.to_string_lossy().into_owned();
                changed = true;
            }
        }
    });
    changed
}

fn heading(ui: &mut egui::Ui, t: &str) {
    ui.add_space(8.0);
    ui.label(RichText::new(t).family(cond()).color(RED_HI).size(15.0).extra_letter_spacing(3.5));
}

/// Site-style button text: condensed bold, uppercase, tracked.
fn caps(t: &str, size: f32) -> RichText {
    RichText::new(t.to_uppercase()).family(cond()).size(size).extra_letter_spacing(2.0)
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        let mut dirty = false;
        let mut play = false;
        let mut start: Option<update::Release> = None;
        while let Ok(m) = self.rx.try_recv() {
            match m {
                Msg::Found(r) if cfg!(debug_assertions) && std::env::var_os("PRM_UPDATE_AUTO").is_some() => start = Some(r),
                Msg::Found(r) => self.upd = Upd::Available(r),
                Msg::Failed(e) => self.upd = Upd::Failed(e),
                Msg::Done => {
                    // Files are swapped: start the new launcher and let this one go.
                    let _ = std::env::current_exe().map(|me| std::process::Command::new(me).spawn());
                    ctx.send_viewport_cmd(ViewportCommand::Close);
                }
            }
        }
        let game_name = self.cfg.games[0].name.clone();
        egui::CentralPanel::default().show(ui, |ui| {
            egui::Frame::new().inner_margin(egui::Margin::symmetric(20, 14)).show(ui, |ui| {
                ui.horizontal(|ui| {
                    if let Some(t) = &self.logo {
                        ui.add(egui::Image::new(t).fit_to_exact_size(egui::vec2(52.0, 52.0)));
                    }
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 0.0;
                        ui.label(RichText::new("PersonaRM").family(cond()).size(30.0).extra_letter_spacing(4.0));
                        ui.label(RichText::new(format!("{game_name}  \u{b7}  launcher v{}", update::VERSION)).color(MUTED));
                    });
                });
                ui.separator();
                match &self.upd {
                    Upd::None => {}
                    Upd::Working => {
                        ui.label(RichText::new("Downloading the update...").color(MUTED));
                    }
                    Upd::Failed(e) => {
                        ui.label(RichText::new(format!("Update failed: {e}")).color(RED_HI).small());
                    }
                    Upd::Available(r) => {
                        egui::Frame::new().fill(INK_3).stroke(egui::Stroke::new(1.0, RED_DEEP)).inner_margin(egui::Margin::symmetric(12, 8)).show(ui, |ui| {
                            ui.horizontal(|ui| {
                                ui.label(RichText::new(format!("Update available   v{}  to  {}", update::VERSION, r.tag)).family(cond()).size(17.0).extra_letter_spacing(1.0));
                                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                    if ui.add(egui::Button::new(caps("Update", 15.0).color(Color32::WHITE)).fill(RED)).clicked() {
                                        start = Some(r.clone());
                                    }
                                });
                            });
                        });
                    }
                }
                egui::ScrollArea::vertical().max_height(ui.available_height() - 74.0).show(ui, |ui| {
                    let s = &mut self.settings;
                    let c = &mut self.cfg;
                    heading(ui, "GAME");
                    egui::Grid::new("game").num_columns(2).min_col_width(130.0).show(ui, |ui| {
                        ui.label("Language");
                        dirty |= combo(ui, "lang", &mut s.language, &[("ja", "Japanese"), ("en", "English")]);
                        ui.end_row();
                        for (code, label) in [("ja", "Japanese disc"), ("en", "English disc")] {
                            ui.label(label);
                            let mut p = c.games[0].discs.get(code).cloned().unwrap_or_default();
                            let pick = || rfd::FileDialog::new().add_filter("Disc image", &["cue"]).pick_file();
                            if path_row(ui, &mut p, pick) {
                                c.games[0].discs.insert(code.into(), p);
                                dirty = true;
                            }
                            ui.end_row();
                        }
                        ui.label("Game executable");
                        let pick = || rfd::FileDialog::new().pick_file();
                        dirty |= path_row(ui, &mut c.game_exe, pick);
                        ui.end_row();
                    });
                    heading(ui, "VIDEO");
                    egui::Grid::new("video").num_columns(2).min_col_width(130.0).show(ui, |ui| {
                        ui.label("Renderer");
                        if combo(ui, "rend", &mut c.renderer, &[("software", "Software"), ("gpu", "GPU")]) {
                            if c.renderer != "gpu" {
                                c.resolution = c.resolution.clamp(1, 4);
                            }
                            dirty = true;
                        }
                        ui.end_row();
                        ui.label("Internal resolution");
                        let gpu = c.renderer == "gpu";
                        let label = if c.resolution == 0 { "Auto".to_string() } else { format!("{}x", c.resolution) };
                        egui::ComboBox::from_id_salt("res").selected_text(label).width(170.0).show_ui(ui, |ui| {
                            let max = if gpu { 9 } else { 4 };
                            for n in (if gpu { 0 } else { 1 })..=max {
                                let l = if n == 0 { "Auto".to_string() } else { format!("{n}x") };
                                dirty |= ui.selectable_value(&mut c.resolution, n, l).changed();
                            }
                        });
                        ui.end_row();
                        ui.label("Frame rate");
                        ui.horizontal(|ui| {
                            for f in [30, 60, 90, 120] {
                                dirty |= ui.selectable_value(&mut s.fps, f, format!("{f}")).changed();
                            }
                        });
                        ui.end_row();
                        ui.label("Widescreen");
                        dirty |= combo(ui, "ws", &mut s.widescreen, &[("off", "Off (4:3)"), ("16:9", "16:9"), ("21:9", "21:9")]);
                        ui.end_row();
                        ui.label("Window mode");
                        dirty |= combo(ui, "wm", &mut s.window_mode, &[("windowed", "Windowed"), ("fullscreen", "Fullscreen"), ("borderless", "Borderless")]);
                        ui.end_row();
                    });
                    if matches!(s.fps, 30 | 90) {
                        ui.label(RichText::new("30 and 90 FPS currently run at 60.").small().color(Color32::GRAY));
                    }
                    heading(ui, "AUDIO");
                    egui::Grid::new("audio").num_columns(2).min_col_width(130.0).show(ui, |ui| {
                        ui.label("Volume");
                        ui.horizontal(|ui| {
                            let mut v = s.volume as f32;
                            if ui.add(egui::Slider::new(&mut v, 0.0..=100.0).integer()).changed() {
                                s.volume = v as u32;
                                dirty = true;
                            }
                            dirty |= ui.checkbox(&mut s.mute, "Mute").changed();
                        });
                        ui.end_row();
                    });
                    heading(ui, "GAMEPLAY");
                    egui::Grid::new("play").num_columns(2).min_col_width(130.0).show(ui, |ui| {
                        ui.label("Text speed");
                        dirty |= combo(ui, "ts", &mut s.text_speed, &[("normal", "Normal"), ("fast", "Fast"), ("instant", "Instant")]);
                        ui.end_row();
                        ui.label("Save folder");
                        let pick = || rfd::FileDialog::new().pick_folder();
                        dirty |= path_row(ui, &mut s.save_dir, pick);
                        ui.end_row();
                        ui.label("");
                        dirty |= ui.checkbox(&mut s.pause_on_focus_loss, "Pause when unfocused").changed();
                        ui.end_row();
                        ui.label("");
                        dirty |= ui.checkbox(&mut c.check_updates, "Check for updates on start").changed();
                        ui.end_row();
                    });
                });
                ui.add_space(8.0);
                if let Some(e) = &self.error {
                    ui.label(RichText::new(e).color(RED_HI).small());
                }
                ui.add_space(4.0);
                ui.scope(|ui| {
                    // Primary button like .btn-primary on the site (hover: #a80f1f).
                    let w = &mut ui.visuals_mut().widgets;
                    w.inactive.weak_bg_fill = RED;
                    w.inactive.bg_stroke = egui::Stroke::new(1.0, RED);
                    w.hovered.weak_bg_fill = Color32::from_rgb(0xa8, 0x0f, 0x1f);
                    let btn = egui::Button::new(caps("Play", 24.0).color(Color32::WHITE))
                        .min_size(egui::vec2(ui.available_width(), 46.0));
                    play = ui.add(btn).clicked();
                });
            });
        });
        if let Some(r) = start {
            self.start_update(r, &ctx);
        }
        self.dirty |= dirty;
        if self.dirty {
            self.save();
        }
        if play {
            self.play(&ctx);
        }
    }
}

/// `--run <exe> <args...>`: start the game, wait for it, record a failure for the UI, reopen the UI.
fn run_watcher(exe: &str, args: &[String]) {
    let log_path = log_dir().join("personarm_sdl.log");
    let log = std::fs::create_dir_all(log_dir()).and_then(|_| std::fs::File::create(&log_path));
    let mut cmd = std::process::Command::new(exe);
    cmd.args(args).stdout(std::process::Stdio::null());
    if let Ok(f) = log {
        cmd.stderr(f);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: personarm_sdl is a console app; no cmd window
    }
    let status = cmd.status();
    if !status.as_ref().is_ok_and(|s| s.success()) {
        let code = status.map_or("?".into(), |s| s.code().map_or("signal".into(), |c| c.to_string()));
        let tail = std::fs::read_to_string(&log_path).unwrap_or_default();
        let tail: Vec<&str> = tail.lines().rev().take(8).collect();
        let msg = format!("The game exited with code {code}.
{}", tail.into_iter().rev().collect::<Vec<_>>().join("
"));
        let _ = std::fs::write(log_dir().join("last_error.txt"), msg);
    }
    if let Ok(me) = std::env::current_exe() {
        let _ = std::process::Command::new(me).spawn();
    }
}

fn main() -> eframe::Result {
    let argv: Vec<String> = std::env::args().collect();
    if argv.len() >= 3 && argv[1] == "--run" {
        run_watcher(&argv[2], &argv[3..]);
        return Ok(());
    }
    if let Some(dir) = std::env::current_exe().ok().as_deref().and_then(|p| p.parent()) {
        if update::cleanup_old(dir) {
            // Right after an update the previous exe may still be shutting down: retry a few times.
            let dir = dir.to_path_buf();
            std::thread::spawn(move || {
                for _ in 0..5 {
                    std::thread::sleep(std::time::Duration::from_secs(2));
                    if !update::cleanup_old(&dir) {
                        break;
                    }
                }
            });
        }
    }
    let icon = load_logo().map(|i| egui::IconData {
        rgba: i.pixels.iter().flat_map(|c| c.to_array()).collect(),
        width: i.width() as u32,
        height: i.height() as u32,
    });
    let mut vp = egui::ViewportBuilder::default()
        .with_title("PersonaRM")
        .with_inner_size([560.0, 870.0])
        .with_resizable(false)
        .with_maximize_button(false);
    if let Some(i) = icon {
        vp = vp.with_icon(i);
    }
    let opts = eframe::NativeOptions {
        viewport: vp,
        ..Default::default()
    };
    eframe::run_native("PersonaRM", opts, Box::new(|cc| Ok(Box::new(App::new(cc)))))
}
