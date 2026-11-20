// No console window in release builds on Windows.
#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

mod model;
mod update;

use eframe::egui::{self, Color32, FontFamily, RichText, ViewportCommand};
use model::*;
use serde_json::Value;
use std::collections::HashMap;
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

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Library,
    Game,
    Settings,
}

struct App {
    page: Page,
    /// Index into `cfg.games` of the game being shown / edited.
    sel: usize,
    /// Cover textures by path (None = could not be loaded); loaded once.
    covers: HashMap<String, Option<egui::TextureHandle>>,
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
        if cfg.games.is_empty() {
            cfg.games = LauncherConfig::default().games;
        }
        let doc = load_game_doc();
        let settings = GameSettings::from_value(&doc, &cfg.games[0].serials);
        // Hidden: `--page game|settings` opens that page (used for screenshots).
        let page = match std::env::args().skip_while(|a| a != "--page").nth(1).as_deref() {
            Some("game") => Page::Game,
            Some("settings") => Page::Settings,
            _ => Page::Library,
        };
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
        Self { page, sel: 0, covers: HashMap::new(), cfg, settings, doc, dirty: false, error, logo, upd: Upd::None, tx, rx }
    }

    fn save(&mut self) {
        let serials = self.cfg.games[self.sel].serials.clone();
        self.settings.apply(&mut self.doc, &serials, self.cfg.resolution);
        if let Err(e) = save_game_doc(&self.doc).and_then(|_| save_launcher(&self.cfg)) {
            self.error = Some(format!("Could not save settings: {e}"));
        }
        self.dirty = false;
    }

    fn play(&mut self, ctx: &egui::Context) {
        self.error = None;
        self.save();
        let g = &self.cfg.games[self.sel];
        let Some(exe) = game_exe_for(&self.cfg, g) else {
            self.error = Some(format!("{} not found: pick it under Settings > Game executable.", exe_name()));
            return;
        };
        let lang = self.settings.language.clone();
        let Some(cue) = disc_for(g, &lang) else {
            let name = if lang == "ja" { "Japanese" } else { "English" };
            self.error = Some(format!("Set the {name} disc (.cue) first."));
            return;
        };
        let mut args = build_args(&game_settings_json().to_string_lossy(), cue, &self.cfg, &self.settings);
        args.extend(g.args.iter().cloned());
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


/// Decode a PNG into a texture, shrunk (nearest) so its longest side is at most 512 px.
fn load_cover(ctx: &egui::Context, path: &str) -> Option<egui::TextureHandle> {
    let mut d = png::Decoder::new(std::io::BufReader::new(std::fs::File::open(path).ok()?));
    d.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
    let mut r = d.read_info().ok()?;
    let mut buf = vec![0; r.output_buffer_size()?];
    let info = r.next_frame(&mut buf).ok()?;
    let (w, h) = (info.width as usize, info.height as usize);
    let n = match info.color_type {
        png::ColorType::Rgba => 4,
        png::ColorType::Rgb => 3,
        png::ColorType::GrayscaleAlpha => 2,
        _ => 1,
    };
    let k = (w.max(h) / 512).max(1);
    let (tw, th) = (w / k, h / k);
    let mut px = Vec::with_capacity(tw * th * 4);
    for y in 0..th {
        for x in 0..tw {
            let i = ((y * k) * w + x * k) * n;
            let c = &buf[i..i + n];
            px.extend(match n {
                4 => [c[0], c[1], c[2], c[3]],
                3 => [c[0], c[1], c[2], 255],
                2 => [c[0], c[0], c[0], c[1]],
                _ => [c[0], c[0], c[0], 255],
            });
        }
    }
    Some(ctx.load_texture(path, egui::ColorImage::from_rgba_unmultiplied([tw, th], &px), egui::TextureOptions::LINEAR))
}

const CARD: egui::Vec2 = egui::vec2(190.0, 270.0);

/// One library card. Returns true when clicked.
fn card(ui: &mut egui::Ui, g: &Game, st: Status, cover: Option<&egui::TextureHandle>) -> bool {
    let (rect, resp) = ui.allocate_exact_size(CARD, egui::Sense::click());
    let p = ui.painter_at(rect);
    let hot = resp.hovered();
    p.rect_filled(rect, 0.0, INK_2);
    let band = 54.0;
    let art = egui::Rect::from_min_max(rect.min, egui::pos2(rect.max.x, rect.max.y - band));
    let title = |size: f32, wrap: f32| p.layout(g.name.clone(), egui::FontId::new(size, cond()), PAPER, wrap);
    if let Some(t) = cover {
        // Fill the art area, cropping the overflow (centred).
        let ts = t.size_vec2();
        let s = (art.width() / ts.x).max(art.height() / ts.y);
        let uv = egui::vec2(art.width() / s / ts.x, art.height() / s / ts.y);
        let uv = egui::Rect::from_center_size(egui::pos2(0.5, 0.5), uv);
        p.image(t.id(), art, uv, Color32::WHITE);
        let gal = title(20.0, art.width() - 24.0);
        let h = gal.size().y + 16.0;
        p.rect_filled(egui::Rect::from_min_max(egui::pos2(art.min.x, art.max.y - h), art.max), 0.0, Color32::from_black_alpha(190));
        p.galley(egui::pos2(art.min.x + 12.0, art.max.y - h + 8.0), gal, PAPER);
    } else {
        p.rect_filled(art, 0.0, INK_3);
        p.rect_filled(egui::Rect::from_min_size(art.min, egui::vec2(5.0, art.height())), 0.0, RED);
        let gal = title(30.0, art.width() - 36.0);
        let y = art.center().y - gal.size().y / 2.0;
        p.galley(egui::pos2(art.min.x + 20.0, y), gal, PAPER);
    }
    // Footer: platform badge + status.
    let foot_y = rect.max.y - band / 2.0;
    let badge = p.layout_no_wrap(g.platform.to_uppercase(), egui::FontId::new(14.0, cond()), Color32::WHITE);
    let br = egui::Rect::from_min_size(egui::pos2(rect.min.x + 12.0, foot_y - 11.0), badge.size() + egui::vec2(12.0, 4.0));
    p.rect_filled(br, 0.0, RED);
    p.galley(br.min + egui::vec2(6.0, 2.0), badge, Color32::WHITE);
    let (txt, col) = match st {
        Status::Ready => ("Ready", PAPER),
        Status::NeedsDisc => ("Needs disc", RED_HI),
        Status::NotPlayable => ("Not playable yet", MUTED),
    };
    let sg = p.layout_no_wrap(txt.into(), egui::FontId::proportional(14.0), col);
    p.galley(egui::pos2(rect.max.x - 12.0 - sg.size().x, foot_y - sg.size().y / 2.0), sg, col);
    p.rect_stroke(rect, 0.0, egui::Stroke::new(1.0, if hot { RED_HI } else { LINE }), egui::StrokeKind::Inside);
    resp.on_hover_cursor(egui::CursorIcon::PointingHand).clicked()
}

/// Gear icon button (drawn: the embedded fonts have no gear glyph).
fn gear_button(ui: &mut egui::Ui) -> bool {
    let (rect, resp) = ui.allocate_exact_size(egui::vec2(38.0, 38.0), egui::Sense::click());
    let col = if resp.hovered() { RED_HI } else { PAPER };
    let p = ui.painter();
    p.rect_filled(rect, 0.0, INK_3);
    p.rect_stroke(rect, 0.0, egui::Stroke::new(1.0, if resp.hovered() { RED_HI } else { LINE }), egui::StrokeKind::Inside);
    let c = rect.center();
    for i in 0..8 {
        let a = i as f32 * std::f32::consts::FRAC_PI_4;
        let d = egui::vec2(a.cos(), a.sin());
        p.line_segment([c + d * 8.0, c + d * 12.0], egui::Stroke::new(4.0, col));
    }
    p.circle_stroke(c, 8.0, egui::Stroke::new(3.0, col));
    resp.on_hover_text("Settings").on_hover_cursor(egui::CursorIcon::PointingHand).clicked()
}

fn grid_row(ui: &mut egui::Ui, label: &str, add: impl FnOnce(&mut egui::Ui)) {
    ui.label(label);
    add(ui);
    ui.end_row();
}

fn play_button(ui: &mut egui::Ui) -> bool {
    // Primary button like .btn-primary on the site (hover: #a80f1f).
    ui.scope(|ui| {
        let w = &mut ui.visuals_mut().widgets;
        w.inactive.weak_bg_fill = RED;
        w.inactive.bg_stroke = egui::Stroke::new(1.0, RED);
        w.hovered.weak_bg_fill = Color32::from_rgb(0xa8, 0x0f, 0x1f);
        let btn = egui::Button::new(caps("Play", 24.0).color(Color32::WHITE)).min_size(egui::vec2(ui.available_width(), 46.0));
        ui.add(btn).clicked()
    })
    .inner
}

impl App {
    fn library(&mut self, ui: &mut egui::Ui) {
        heading(ui, "LIBRARY");
        let gap = 14.0;
        let cols = (((ui.available_width() + gap) / (CARD.x + gap)) as usize).max(1);
        let ctx = ui.ctx().clone();
        let mut open = None;
        egui::Grid::new("lib").spacing([gap, gap]).show(ui, |ui| {
            for (i, g) in self.cfg.games.iter().enumerate() {
                if !g.cover.is_empty() {
                    self.covers.entry(g.cover.clone()).or_insert_with(|| load_cover(&ctx, &g.cover));
                }
                let tex = self.covers.get(&g.cover).and_then(Option::as_ref);
                if card(ui, g, status(&self.cfg, g), tex) {
                    open = Some(i);
                }
                if (i + 1) % cols == 0 {
                    ui.end_row();
                }
            }
        });
        if let Some(i) = open {
            self.open_game(i);
        }
    }

    fn open_game(&mut self, i: usize) {
        self.sel = i;
        // text_speed is per game: re-read it for this game's serials.
        self.settings.text_speed = GameSettings::from_value(&self.doc, &self.cfg.games[i].serials).text_speed;
        self.page = Page::Game;
    }

    /// Returns (dirty, play).
    fn game_page(&mut self, ui: &mut egui::Ui) -> (bool, bool) {
        let mut dirty = false;
        let s = &mut self.settings;
        let g = &mut self.cfg.games[self.sel];
        ui.label(RichText::new(g.name.clone()).family(cond()).size(26.0).extra_letter_spacing(2.0));
        ui.label(RichText::new(g.platform.to_uppercase()).color(RED_HI));
        egui::ScrollArea::vertical().max_height(ui.available_height() - 74.0).show(ui, |ui| {
            heading(ui, "DISCS");
            egui::Grid::new("discs").num_columns(2).min_col_width(130.0).show(ui, |ui| {
                for (code, label) in [("ja", "Japanese disc"), ("en", "English disc")] {
                    grid_row(ui, label, |ui| {
                        let mut p = g.discs.get(code).cloned().unwrap_or_default();
                        let pick = || rfd::FileDialog::new().add_filter("Disc image", &["cue"]).pick_file();
                        if path_row(ui, &mut p, pick) {
                            g.discs.insert(code.into(), p);
                            dirty = true;
                        }
                    });
                }
            });
            heading(ui, "OPTIONS");
            egui::Grid::new("opts").num_columns(2).min_col_width(130.0).show(ui, |ui| {
                grid_row(ui, "Language", |ui| dirty |= combo(ui, "lang", &mut s.language, &[("ja", "Japanese"), ("en", "English")]));
                grid_row(ui, "Text speed", |ui| dirty |= combo(ui, "ts", &mut s.text_speed, &[("normal", "Normal"), ("fast", "Fast"), ("instant", "Instant")]));
                grid_row(ui, "Cover image (PNG)", |ui| {
                    let pick = || rfd::FileDialog::new().add_filter("PNG image", &["png"]).pick_file();
                    dirty |= path_row(ui, &mut g.cover, pick);
                });
            });
        });
        if let Some(e) = &self.error {
            ui.label(RichText::new(e).color(RED_HI).small());
        }
        ui.add_space(4.0);
        (dirty, play_button(ui))
    }

    fn settings_page(&mut self, ui: &mut egui::Ui) -> bool {
        let mut dirty = false;
        let s = &mut self.settings;
        let c = &mut self.cfg;
        ui.label(RichText::new("SETTINGS").family(cond()).size(26.0).extra_letter_spacing(2.0));
        ui.label(RichText::new("Shared by all games").color(MUTED));
        egui::ScrollArea::vertical().show(ui, |ui| {
            heading(ui, "VIDEO");
            egui::Grid::new("video").num_columns(2).min_col_width(130.0).show(ui, |ui| {
                grid_row(ui, "Renderer", |ui| {
                    if combo(ui, "rend", &mut c.renderer, &[("software", "Software"), ("gpu", "GPU")]) {
                        if c.renderer != "gpu" {
                            c.resolution = c.resolution.clamp(1, 4);
                        }
                        dirty = true;
                    }
                });
                grid_row(ui, "Internal resolution", |ui| {
                    let gpu = c.renderer == "gpu";
                    let label = if c.resolution == 0 { "Auto".to_string() } else { format!("{}x", c.resolution) };
                    egui::ComboBox::from_id_salt("res").selected_text(label).width(170.0).show_ui(ui, |ui| {
                        let max = if gpu { 9 } else { 4 };
                        for n in (if gpu { 0 } else { 1 })..=max {
                            let l = if n == 0 { "Auto".to_string() } else { format!("{n}x") };
                            dirty |= ui.selectable_value(&mut c.resolution, n, l).changed();
                        }
                    });
                });
                grid_row(ui, "Frame rate", |ui| {
                    ui.horizontal(|ui| {
                        for f in [30, 60, 90, 120] {
                            dirty |= ui.selectable_value(&mut s.fps, f, format!("{f}")).changed();
                        }
                    });
                });
                grid_row(ui, "Widescreen", |ui| dirty |= combo(ui, "ws", &mut s.widescreen, &[("off", "Off (4:3)"), ("16:9", "16:9"), ("21:9", "21:9")]));
                grid_row(ui, "Window mode", |ui| dirty |= combo(ui, "wm", &mut s.window_mode, &[("windowed", "Windowed"), ("fullscreen", "Fullscreen"), ("borderless", "Borderless")]));
            });
            if matches!(s.fps, 30 | 90) {
                ui.label(RichText::new("30 and 90 FPS currently run at 60.").small().color(Color32::GRAY));
            }
            heading(ui, "AUDIO");
            egui::Grid::new("audio").num_columns(2).min_col_width(130.0).show(ui, |ui| {
                grid_row(ui, "Volume", |ui| {
                    ui.horizontal(|ui| {
                        let mut v = s.volume as f32;
                        if ui.add(egui::Slider::new(&mut v, 0.0..=100.0).integer()).changed() {
                            s.volume = v as u32;
                            dirty = true;
                        }
                        dirty |= ui.checkbox(&mut s.mute, "Mute").changed();
                    });
                });
            });
            heading(ui, "GENERAL");
            egui::Grid::new("general").num_columns(2).min_col_width(130.0).show(ui, |ui| {
                grid_row(ui, "Save folder", |ui| dirty |= path_row(ui, &mut s.save_dir, || rfd::FileDialog::new().pick_folder()));
                grid_row(ui, "Game executable", |ui| dirty |= path_row(ui, &mut c.game_exe, || rfd::FileDialog::new().pick_file()));
                grid_row(ui, "", |ui| dirty |= ui.checkbox(&mut s.pause_on_focus_loss, "Pause when unfocused").changed());
                grid_row(ui, "", |ui| dirty |= ui.checkbox(&mut c.check_updates, "Check for updates on start").changed());
            });
        });
        dirty
    }
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
        egui::CentralPanel::default().show(ui, |ui| {
            egui::Frame::new().inner_margin(egui::Margin::symmetric(20, 14)).show(ui, |ui| {
                ui.horizontal(|ui| {
                    if let Some(t) = &self.logo {
                        ui.add(egui::Image::new(t).fit_to_exact_size(egui::vec2(52.0, 52.0)));
                    }
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 0.0;
                        ui.label(RichText::new("PersonaRM").family(cond()).size(30.0).extra_letter_spacing(4.0));
                        ui.label(RichText::new(format!("launcher v{}", update::VERSION)).color(MUTED));
                    });
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if self.page == Page::Library {
                            if gear_button(ui) {
                                self.page = Page::Settings;
                            }
                        } else if ui.add(egui::Button::new(caps("\u{2190} Back", 16.0))).clicked() {
                            self.page = Page::Library;
                            self.error = None;
                        }
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
                match self.page {
                    Page::Library => {
                        egui::ScrollArea::vertical().show(ui, |ui| self.library(ui));
                    }
                    Page::Game => (dirty, play) = self.game_page(ui),
                    Page::Settings => dirty = self.settings_page(ui),
                }
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
        .with_inner_size([940.0, 680.0])
        .with_min_inner_size([480.0, 520.0]);
    if let Some(i) = icon {
        vp = vp.with_icon(i);
    }
    let opts = eframe::NativeOptions {
        viewport: vp,
        ..Default::default()
    };
    eframe::run_native("PersonaRM", opts, Box::new(|cc| Ok(Box::new(App::new(cc)))))
}
