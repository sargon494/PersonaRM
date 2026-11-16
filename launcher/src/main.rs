// No console window in release builds on Windows.
#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

mod model;

use eframe::egui::{self, Color32, RichText, ViewportCommand};
use model::*;
use serde_json::Value;
use std::path::PathBuf;

const ACCENT: Color32 = Color32::from_rgb(110, 140, 255);

struct App {
    cfg: LauncherConfig,
    settings: GameSettings,
    doc: Value,
    dirty: bool,
    error: Option<String>,
}

impl App {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let mut v = egui::Visuals::dark();
        v.panel_fill = Color32::from_rgb(22, 23, 28);
        v.selection.bg_fill = ACCENT;
        v.widgets.inactive.bg_fill = Color32::from_rgb(40, 42, 52);
        v.widgets.inactive.weak_bg_fill = Color32::from_rgb(40, 42, 52);
        cc.egui_ctx.set_visuals(v);
        cc.egui_ctx.all_styles_mut(|s| {
            s.spacing.item_spacing = egui::vec2(10.0, 9.0);
            s.spacing.button_padding = egui::vec2(10.0, 5.0);
        });
        let mut cfg = load_launcher();
        if let Some(p) = find_game_exe(&cfg) {
            cfg.game_exe = p.to_string_lossy().into_owned();
        }
        let doc = load_game_doc();
        let settings = GameSettings::from_value(&doc, &cfg.games[0].serials);
        let last = log_dir().join("last_error.txt");
        let error = std::fs::read_to_string(&last).ok();
        let _ = std::fs::remove_file(last);
        Self { cfg, settings, doc, dirty: false, error }
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
    ui.add_space(6.0);
    ui.label(RichText::new(t).strong().color(ACCENT).size(13.0));
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        let mut dirty = false;
        let mut play = false;
        let game_name = self.cfg.games[0].name.clone();
        egui::CentralPanel::default().show(ui, |ui| {
            egui::Frame::new().inner_margin(egui::Margin::symmetric(20, 14)).show(ui, |ui| {
                ui.label(RichText::new("PersonaRM").size(26.0).strong());
                ui.label(RichText::new(game_name).color(Color32::GRAY));
                egui::ScrollArea::vertical().max_height(ui.available_height() - 96.0).show(ui, |ui| {
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
                    });
                });
                ui.add_space(8.0);
                if let Some(e) = &self.error {
                    ui.label(RichText::new(e).color(Color32::from_rgb(240, 110, 110)).small());
                }
                ui.add_space(4.0);
                let btn = egui::Button::new(RichText::new("Play").size(20.0).strong().color(Color32::WHITE))
                    .fill(ACCENT)
                    .min_size(egui::vec2(ui.available_width(), 44.0));
                play = ui.add(btn).clicked();
            });
        });
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
    let opts = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("PersonaRM")
            .with_inner_size([560.0, 790.0])
            .with_resizable(false)
            .with_maximize_button(false),
        ..Default::default()
    };
    eframe::run_native("PersonaRM", opts, Box::new(|cc| Ok(Box::new(App::new(cc)))))
}
