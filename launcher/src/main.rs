// No console window in release builds on Windows.
#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

mod disc;
mod model;
mod update;

use eframe::egui::{self, Color32, FontFamily, RichText, ViewportCommand};
use model::*;
use serde_json::Value;
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
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

fn arg(name: &str) -> Option<String> {
    std::env::args().skip_while(|a| a != name).nth(1)
}

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
    v.selection.stroke = egui::Stroke::new(2.0, RED_HI); // keyboard focus ring
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
        s.spacing.scroll = egui::style::ScrollStyle::solid(); // thin bar that is visible whenever the page scrolls
        s.spacing.scroll.bar_width = 8.0;
        s.spacing.item_spacing = egui::vec2(10.0, 7.0);
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
    /// personarm_native progress, then its end (None = native code ready, Some = why not).
    Native(NativeLine),
    NativeEnd(Option<String>),
    /// "Find my discs" finished: what it found, the cache entries it made, seconds it took.
    Scan(Vec<disc::Found>, disc::Cache, f32),
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
    /// Hotkey action waiting for a key press (game page).
    capture: Option<&'static str>,
    dirty: bool,
    error: Option<String>,
    logo: Option<egui::TextureHandle>,
    upd: Upd,
    tx: mpsc::Sender<Msg>,
    rx: mpsc::Receiver<Msg>,
    /// Page transition: start time (egui clock) and direction (+1 deeper, -1 back).
    t0: f64,
    dir: f32,
    /// Hidden `--shot <png>` (screenshots) and `--focus` (focus the first card).
    shot: Option<String>,
    frames: u32,
    focus_first: bool,
    /// Preparing native code before PLAY: progress 0..1, message, and the game command to start after.
    native: Option<(f32, String, PathBuf, Vec<String>)>,
    /// "Find my discs": running now, what the last run found, and how long it took (None = never ran).
    scanning: bool,
    found: Vec<disc::Found>,
    scan_secs: Option<f32>,
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

    /// Search the usual folders (and the user's extra ones) on a thread; the UI repaints once when it is done.
    fn start_scan(&mut self, ctx: &egui::Context) {
        if self.scanning {
            return;
        }
        self.scanning = true;
        let (games, dirs, mut cache) = (self.cfg.games.clone(), self.cfg.scan_dirs.clone(), self.cfg.disc_cache.clone());
        let (tx, ctx) = (self.tx.clone(), ctx.clone());
        std::thread::spawn(move || {
            let t = std::time::Instant::now();
            let found = disc::scan(&games, &dirs, &mut cache);
            let _ = tx.send(Msg::Scan(found, cache, t.elapsed().as_secs_f32()));
            ctx.request_repaint();
        });
    }

    /// Fill the empty (or missing) Japanese / English disc fields with the complete discs that were found.
    fn apply_found(&mut self) -> bool {
        let mut changed = false;
        for f in self.found.iter().filter(|f| f.disc.problem.is_none()) {
            let g = &mut self.cfg.games[f.game];
            if let Some(slot) = disc::slot_of(g, &f.disc) {
                if path_state(g.discs.get(slot).map_or("", String::as_str), false) != PathState::Found {
                    g.discs.insert(slot.into(), f.path.to_string_lossy().into_owned());
                    changed = true;
                }
            }
        }
        changed
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
        let page = match arg("--page").as_deref() {
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
        let mut app = Self { page, sel: 0, covers: HashMap::new(), cfg, settings, doc, capture: None, dirty: false, error, logo, upd: Upd::None, tx, rx, t0: -10.0, dir: 1.0, shot: arg("--shot"), frames: 0, focus_first: std::env::args().any(|a| a == "--focus"), native: None, scanning: false, found: Vec::new(), scan_secs: None };
        // First launch, no disc set anywhere: look for the discs once (hidden `--find` forces it).
        let no_disc = !app.cfg.games.iter().any(|g| g.discs.values().any(|d| path_state(d, false) == PathState::Found));
        if (!app.cfg.scanned && no_disc) || std::env::args().any(|a| a == "--find") {
            app.start_scan(&cc.egui_ctx);
        }
        app
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
            self.go(ctx, Page::Settings);
            return;
        };
        let lang = self.settings.language.clone();
        let Some(cue) = disc_for(g, &lang) else {
            let name = if lang == "ja" { "Japanese" } else { "English" };
            self.error = Some(format!("Set the {name} disc (.cue) first."));
            self.go(ctx, Page::Game);
            return;
        };
        let mut args = build_args(&game_settings_json().to_string_lossy(), cue, &self.cfg, &self.settings);
        args.extend(g.args.iter().cloned());
        // Native code: personarm_native builds it from this disc on this PC the first time (a no-op
        // once cached), then the game runs it. Missing tool or failure: play interpreted.
        let tool = native_tool_for(&exe);
        if self.settings.native && tool.is_file() {
            self.native = Some((0.0, "Checking native code...".into(), exe, args));
            let (tx, ctx, cue) = (self.tx.clone(), ctx.clone(), cue.to_string());
            std::thread::spawn(move || {
                let _ = tx.send(Msg::NativeEnd(prepare_native(&tool, &cue, &tx, &ctx)));
                ctx.request_repaint();
            });
            return;
        }
        self.launch(ctx, exe, args);
    }

    fn launch(&mut self, ctx: &egui::Context, exe: PathBuf, args: Vec<String>) {
        // The UI process exits while the game runs (an OpenGL window costs ~100 MB even hidden); a
        // windowless watcher (`--run`, a few MB) waits for the game and reopens the launcher.
        let me = update::relaunch_exe().unwrap_or_default();
        if let Err(e) = std::process::Command::new(me).arg("--run").arg(&exe).args(&args).spawn() {
            self.error = Some(format!("Could not start the game: {e}"));
            return;
        }
        ctx.send_viewport_cmd(ViewportCommand::Close);
    }
}

/// Runs `personarm_native <cue> --json`, forwarding its progress. None = native code ready.
fn prepare_native(tool: &std::path::Path, cue: &str, tx: &mpsc::Sender<Msg>, ctx: &egui::Context) -> Option<String> {
    use std::io::BufRead;
    let mut cmd = std::process::Command::new(tool);
    cmd.arg(cue).arg("--json").stdout(std::process::Stdio::piped()).stderr(std::process::Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => return Some(format!("could not start {}: {e}", tool.display())),
    };
    let mut last = None;
    for line in std::io::BufReader::new(child.stdout.take()?).lines().map_while(Result::ok) {
        if let Some(l) = parse_native_line(&line) {
            if l.ok.is_some() {
                last = Some(l);
            } else {
                let _ = tx.send(Msg::Native(l));
                ctx.request_repaint();
            }
        }
    }
    let _ = child.wait();
    match last {
        Some(l) if l.ok == Some(true) => None,
        Some(l) => Some(l.message),
        None => Some("the native code tool stopped unexpectedly".into()),
    }
}

/// Set from `reduce_motion`: every `anim` then jumps straight to its target.
static REDUCE: AtomicBool = AtomicBool::new(false);

fn id(x: impl std::hash::Hash + std::fmt::Debug) -> egui::Id {
    #[allow(deprecated)]
    egui::Id::new(x)
}

/// 0..1 ease-out tween toward `on`. egui repaints only while it is moving, so idle stays at 0 CPU.
fn anim(ctx: &egui::Context, key: impl std::hash::Hash + std::fmt::Debug, on: bool, secs: f32) -> f32 {
    let t = if REDUCE.load(Ordering::Relaxed) { 0.0 } else { secs };
    ctx.animate_bool_with_time_and_easing(id(key), on, t, egui::emath::easing::cubic_out)
}

fn mix(a: Color32, b: Color32, t: f32) -> Color32 {
    a.lerp_to_gamma(b, t)
}

fn ring(p: &egui::Painter, r: egui::Rect) {
    p.rect_stroke(r.expand(2.0), 0.0, egui::Stroke::new(2.0, RED_HI), egui::StrokeKind::Outside);
}

#[derive(Clone, Copy)]
enum Icon {
    Check,
    Cross,
    Dash,
    Ring,
}

/// Status glyphs are drawn (the embedded fonts have none), so state never depends on colour alone.
fn icon(p: &egui::Painter, c: egui::Pos2, k: Icon, col: Color32) {
    let s = egui::Stroke::new(2.0, col);
    let v = |x: f32, y: f32| c + egui::vec2(x, y);
    match k {
        Icon::Check => {
            p.add(egui::Shape::line(vec![v(-4.5, 0.0), v(-1.5, 3.5), v(4.5, -3.5)], s));
        }
        Icon::Cross => {
            p.line_segment([v(-3.5, -3.5), v(3.5, 3.5)], s);
            p.line_segment([v(-3.5, 3.5), v(3.5, -3.5)], s);
        }
        Icon::Dash => {
            p.line_segment([v(-4.0, 0.0), v(4.0, 0.0)], s);
        }
        Icon::Ring => {
            p.circle_stroke(c, 4.5, s);
        }
    }
}

/// Small muted line under a control.
fn hint(ui: &mut egui::Ui, t: &str) {
    ui.add(egui::Label::new(RichText::new(t).size(13.0).color(MUTED)));
}

/// Icon + words for a path's state; `unset` replaces the word when the field is empty.
fn state_line(ui: &mut egui::Ui, s: PathState, empty: bool, unset: &str) {
    let (k, col, txt) = match s {
        PathState::Found => (Icon::Check, PAPER, "found"),
        PathState::Missing => (Icon::Cross, RED_HI, "file missing"),
        PathState::NotSet => (Icon::Dash, MUTED, "not set"),
    };
    icon_line(ui, k, col, if empty { unset } else { txt });
}

fn icon_line(ui: &mut egui::Ui, k: Icon, col: Color32, txt: &str) -> egui::Response {
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = 5.0;
        let (r, _) = ui.allocate_exact_size(egui::vec2(12.0, 14.0), egui::Sense::hover());
        icon(ui.painter(), r.center(), k, col);
        ui.label(RichText::new(txt).size(13.0).color(col));
    })
    .response
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

/// Path field + Browse + a live found / missing / not set line, validated on every edit.
/// `dir`: the path is a folder. `empty`: state to show when the field is empty (it can fall back to a
/// default); `unset`: the words for that case.
fn path_row(ui: &mut egui::Ui, path: &mut String, dir: bool, empty: PathState, unset: &str, pick: impl FnOnce() -> Option<PathBuf>) -> bool {
    path_row_v(ui, path, dir, empty, unset, None, pick)
}

/// `path_row` with an optional verdict (ok, text) that replaces "found" once the file exists (disc check).
fn path_row_v(ui: &mut egui::Ui, path: &mut String, dir: bool, empty: PathState, unset: &str, verdict: Option<(bool, String)>, pick: impl FnOnce() -> Option<PathBuf>) -> bool {
    let mut changed = false;
    ui.vertical(|ui| {
        ui.spacing_mut().item_spacing.y = 3.0;
        ui.horizontal(|ui| {
            let w = (ui.available_width() - 78.0).clamp(120.0, 270.0);
            if ui.add(egui::TextEdit::singleline(path).desired_width(w)).changed() {
                path.retain(|c| c != '"'); // "Copy as path" pastes quotes
                changed = true;
            }
            if ui.button("Browse").clicked() {
                if let Some(p) = pick() {
                    *path = p.to_string_lossy().into_owned();
                    changed = true;
                }
            }
        });
        let st = path_state(path, dir);
        if let (PathState::Found, Some((ok, t))) = (st, &verdict) {
            icon_line(ui, if *ok { Icon::Check } else { Icon::Cross }, if *ok { PAPER } else { RED_HI }, t);
            return;
        }
        state_line(ui, if st == PathState::NotSet { empty } else { st }, st == PathState::NotSet, unset);
    });
    changed
}

fn heading(ui: &mut egui::Ui, t: &str) {
    ui.add_space(4.0);
    ui.label(RichText::new(t).family(cond()).color(RED_HI).size(15.0).extra_letter_spacing(3.5));
}

/// Site-style button text: condensed bold, uppercase, tracked.
fn caps(t: &str, size: f32) -> RichText {
    RichText::new(t.to_uppercase()).family(cond()).size(size).extra_letter_spacing(2.0)
}

/// Primary red button (the site's .btn-primary): hover darkens, press sinks 2 px; both tweened.
fn red_button(ui: &mut egui::Ui, rect: egui::Rect, wid: egui::Id, label: &str, size: f32, focusable: bool) -> egui::Response {
    let resp = ui.interact(rect, wid, if focusable { egui::Sense::click() } else { egui::Sense::CLICK });
    let ctx = ui.ctx().clone();
    let hot = anim(&ctx, (wid, "h"), ui.rect_contains_pointer(rect) || resp.has_focus(), 0.12);
    let down = anim(&ctx, (wid, "d"), resp.is_pointer_button_down_on(), 0.07);
    let r = rect.shrink(2.0 * down).translate(egui::vec2(0.0, 1.5 * down));
    let p = ui.painter();
    p.rect_filled(r, 0.0, mix(RED, Color32::from_rgb(0xa8, 0x0f, 0x1f), hot * (1.0 - 0.5 * down)));
    let g = p.layout_no_wrap(label.to_uppercase(), egui::FontId::new(size, cond()), Color32::WHITE);
    p.galley(r.center() - g.size() / 2.0, g, Color32::WHITE);
    if resp.has_focus() {
        ring(p, rect);
    }
    resp.on_hover_cursor(egui::CursorIcon::PointingHand)
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

const CARD: egui::Vec2 = egui::vec2(250.0, 330.0);

#[derive(PartialEq)]
enum Act {
    None,
    Open,
    Play,
}

/// One library card: click opens the game page; PLAY / SET UP acts directly; Enter on a focused card plays.
fn card(ui: &mut egui::Ui, i: usize, g: &Game, st: Status, cover: Option<&egui::TextureHandle>, grab_focus: bool) -> Act {
    let ctx = ui.ctx().clone();
    let (rect, resp) = ui.allocate_exact_size(CARD, egui::Sense::click());
    if grab_focus {
        resp.request_focus();
    }
    let t = anim(&ctx, ("card", i), ui.rect_contains_pointer(rect) || resp.has_focus(), 0.14);
    let rect = rect.translate(egui::vec2(0.0, -3.0 * t)); // slight lift
    let p = ui.painter_at(rect.expand(6.0));
    p.rect_filled(rect, 0.0, mix(INK_2, Color32::from_rgb(0x13, 0x13, 0x1c), t));
    let band = 82.0;
    let art = egui::Rect::from_min_max(rect.min, egui::pos2(rect.max.x, rect.max.y - band));
    let title = |size: f32, wrap: f32| p.layout(g.name.clone(), egui::FontId::new(size, cond()), PAPER, wrap);
    if let Some(tx) = cover {
        // Fill the art area, cropping the overflow (centred).
        let ts = tx.size_vec2();
        let s = (art.width() / ts.x).max(art.height() / ts.y);
        let uv = egui::vec2(art.width() / s / ts.x, art.height() / s / ts.y);
        let uv = egui::Rect::from_center_size(egui::pos2(0.5, 0.5), uv);
        p.image(tx.id(), art, uv, mix(Color32::from_gray(215), Color32::WHITE, t));
        let gal = title(22.0, art.width() - 24.0);
        let h = gal.size().y + 16.0;
        p.rect_filled(egui::Rect::from_min_max(egui::pos2(art.min.x, art.max.y - h), art.max), 0.0, Color32::from_black_alpha(190));
        p.galley(egui::pos2(art.min.x + 12.0, art.max.y - h + 8.0), gal, PAPER);
    } else {
        p.rect_filled(art, 0.0, INK_3);
        p.rect_filled(egui::Rect::from_min_size(art.min, egui::vec2(5.0, art.height())), 0.0, RED);
        let gal = title(34.0, art.width() - 40.0);
        let y = art.center().y - gal.size().y / 2.0;
        p.galley(egui::pos2(art.min.x + 22.0, y), gal, PAPER);
    }
    // The red bar above the footer grows from a stub to the full width on hover / focus.
    let by = rect.max.y - band;
    p.rect_filled(egui::Rect::from_min_size(egui::pos2(rect.min.x, by), egui::vec2(28.0 + (rect.width() - 28.0) * t, 3.0)), 0.0, RED_HI);
    // Footer row 1: platform badge + status (icon + words).
    let y1 = by + 22.0;
    let badge = p.layout_no_wrap(g.platform.to_uppercase(), egui::FontId::new(14.0, cond()), Color32::WHITE);
    let br = egui::Rect::from_min_size(egui::pos2(rect.min.x + 12.0, y1 - 11.0), badge.size() + egui::vec2(12.0, 4.0));
    p.rect_filled(br, 0.0, RED);
    p.galley(br.min + egui::vec2(6.0, 2.0), badge, Color32::WHITE);
    let (txt, col, ic) = match st {
        Status::Ready => ("Ready", PAPER, Icon::Check),
        Status::NeedsDisc => ("Set up", RED_HI, Icon::Ring),
        Status::NotPlayable => ("Game exe missing", MUTED, Icon::Cross),
    };
    let sg = p.layout_no_wrap(txt.into(), egui::FontId::proportional(15.0), col);
    let sw = sg.size().x;
    p.galley(egui::pos2(rect.max.x - 12.0 - sw, y1 - sg.size().y / 2.0), sg, col);
    icon(&p, egui::pos2(rect.max.x - 12.0 - sw - 11.0, y1), ic, col);
    p.rect_stroke(rect, 0.0, egui::Stroke::new(1.0, mix(LINE, RED_HI, t)), egui::StrokeKind::Inside);
    if resp.has_focus() {
        ring(&p, rect);
    }
    // Footer row 2: PLAY (ready) or SET UP (needs a disc).
    let mut act = Act::None;
    if st != Status::NotPlayable {
        let b = egui::Rect::from_min_size(egui::pos2(rect.min.x + 12.0, rect.max.y - 40.0), egui::vec2(rect.width() - 24.0, 28.0));
        let label = if st == Status::Ready { "Play" } else { "Set up" };
        if red_button(ui, b, id(("cardbtn", i)), label, 18.0, false).clicked() {
            act = if st == Status::Ready { Act::Play } else { Act::Open };
        }
    }
    let enter = resp.has_focus() && ctx.input(|i| i.key_pressed(egui::Key::Enter));
    if act == Act::None && enter {
        act = if st == Status::Ready { Act::Play } else { Act::Open };
    } else if act == Act::None && resp.clicked() && !enter {
        act = Act::Open;
    }
    resp.on_hover_cursor(egui::CursorIcon::PointingHand);
    act
}

/// Gear + "Settings" button (the gear is drawn: the embedded fonts have no gear glyph).
fn gear_button(ui: &mut egui::Ui) -> bool {
    let (rect, resp) = ui.allocate_exact_size(egui::vec2(124.0, 38.0), egui::Sense::click());
    let t = anim(ui.ctx(), "gear", resp.hovered() || resp.has_focus(), 0.12);
    let col = mix(PAPER, RED_HI, t);
    let p = ui.painter();
    p.rect_filled(rect, 0.0, INK_3);
    p.rect_stroke(rect, 0.0, egui::Stroke::new(1.0, mix(LINE, RED_HI, t)), egui::StrokeKind::Inside);
    let c = rect.left_center() + egui::vec2(20.0, 0.0);
    for i in 0..8 {
        let a = i as f32 * std::f32::consts::FRAC_PI_4;
        let d = egui::vec2(a.cos(), a.sin());
        p.line_segment([c + d * 7.0, c + d * 10.5], egui::Stroke::new(3.5, col));
    }
    p.circle_stroke(c, 6.5, egui::Stroke::new(2.5, col));
    let g = p.layout_no_wrap("SETTINGS".into(), egui::FontId::new(16.0, cond()), col);
    p.galley(egui::pos2(rect.min.x + 40.0, rect.center().y - g.size().y / 2.0), g, col);
    if resp.has_focus() {
        ring(p, rect);
    }
    resp.on_hover_cursor(egui::CursorIcon::PointingHand).clicked()
}

fn grid_row(ui: &mut egui::Ui, label: &str, add: impl FnOnce(&mut egui::Ui)) {
    ui.label(label);
    ui.vertical(|ui| {
        ui.spacing_mut().item_spacing.y = 3.0;
        add(ui)
    });
    ui.end_row();
}

fn grid(id: &str) -> egui::Grid {
    egui::Grid::new(id).num_columns(2).min_col_width(130.0).spacing([10.0, 7.0])
}

fn video(ui: &mut egui::Ui, s: &mut GameSettings, c: &mut LauncherConfig) -> bool {
    let mut dirty = false;
    heading(ui, "VIDEO");
    grid("video").show(ui, |ui| {
        grid_row(ui, "Renderer", |ui| {
            if combo(ui, "rend", &mut c.renderer, &[("software", "Software"), ("gpu", "GPU")]) {
                if c.renderer != "gpu" {
                    c.resolution = c.resolution.clamp(1, 4);
                }
                dirty = true;
            }
            hint(ui, "GPU: sharper and faster at high resolution (Windows). Software: most compatible.");
        });
        grid_row(ui, "Internal resolution", |ui| {
            let gpu = c.renderer == "gpu";
            let label = if c.resolution == 0 { "Auto".to_string() } else { format!("{}x", c.resolution) };
            egui::ComboBox::from_id_salt("res").selected_text(label).width(170.0).show_ui(ui, |ui| {
                for n in (if gpu { 0 } else { 1 })..=(if gpu { 9 } else { 4 }) {
                    let l = if n == 0 { "Auto".to_string() } else { format!("{n}x") };
                    dirty |= ui.selectable_value(&mut c.resolution, n, l).changed();
                }
            });
            hint(ui, if s.fps > 60 { "Not used above 60 FPS (frame interpolation)." } else if gpu { "Multiplies the PS1 resolution; higher is sharper and heavier." } else { "Multiplies the PS1 resolution. Software stops at 4x; GPU goes higher." });
        });
        grid_row(ui, "Frame rate", |ui| {
            ui.horizontal(|ui| {
                for f in [30, 60, 90, 120] {
                    dirty |= ui.selectable_value(&mut s.fps, f, format!("{f}")).changed();
                }
            });
            hint(ui, if matches!(s.fps, 30 | 90) { "30 and 90 currently run at 60. 90/120 interpolate between frames: experimental." } else { "90/120 interpolate between frames: experimental, may show glitches." });
        });
        ui.horizontal(|ui| {
            ui.label("Widescreen");
            ui.label(RichText::new("EXPERIMENTAL").family(cond()).size(12.0).extra_letter_spacing(1.5).color(RED_HI));
        });
        ui.vertical(|ui| {
            ui.spacing_mut().item_spacing.y = 3.0;
            dirty |= combo(ui, "ws", &mut s.widescreen, &[("off", "Off (4:3)"), ("16:9", "16:9"), ("21:9", "21:9")]);
            hint(ui, "Wider 3D view; some sprites and fades may glitch. 4:3 recommended.");
        });
        ui.end_row();
        grid_row(ui, "Window mode", |ui| dirty |= combo(ui, "wm", &mut s.window_mode, &[("windowed", "Windowed"), ("fullscreen", "Fullscreen"), ("borderless", "Borderless")]));
    });
    dirty
}

fn audio_general(ui: &mut egui::Ui, s: &mut GameSettings, c: &mut LauncherConfig) -> bool {
    let mut dirty = false;
    heading(ui, "AUDIO");
    grid("audio").show(ui, |ui| {
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
    grid("general").show(ui, |ui| {
        grid_row(ui, "Save folder", |ui| {
            dirty |= path_row(ui, &mut s.save_dir, true, PathState::NotSet, "default location", || rfd::FileDialog::new().pick_folder());
        });
        grid_row(ui, "Game executable", |ui| {
            let auto = if find_game_exe(c).is_some() { PathState::Found } else { PathState::Missing };
            dirty |= path_row(ui, &mut c.game_exe, false, auto, "auto: next to the launcher", || rfd::FileDialog::new().pick_file());
        });
        grid_row(ui, "", |ui| dirty |= ui.checkbox(&mut s.pause_on_focus_loss, "Pause when unfocused").changed());
        grid_row(ui, "", |ui| {
            dirty |= ui.checkbox(&mut s.native, "Native code (recompiled)").on_hover_text(
                "Built from your own disc on this PC the first time you play (about a minute at most), then cached. Off = interpreter.").changed()
        });
        grid_row(ui, "", |ui| dirty |= ui.checkbox(&mut c.check_updates, "Check for updates on start").changed());
        grid_row(ui, "", |ui| dirty |= ui.checkbox(&mut c.reduce_motion, "Reduce motion").changed());
    });
    dirty
}

impl App {
    fn library(&mut self, ui: &mut egui::Ui) -> Option<(usize, Act)> {
        let ready = self.cfg.games.iter().filter(|g| status(&self.cfg, g) == Status::Ready).count();
        let n = self.cfg.games.len();
        ui.horizontal(|ui| {
            heading(ui, "LIBRARY");
            ui.add_space(6.0);
            ui.label(RichText::new(format!("{n} game{} \u{b7} {ready} ready", if n == 1 { "" } else { "s" })).color(MUTED));
        });
        let gap = 14.0;
        let cols = (((ui.available_width() + gap) / (CARD.x + gap)) as usize).max(1);
        let ctx = ui.ctx().clone();
        let mut out = None;
        ui.add_space(3.0);
        egui::Grid::new("lib").spacing([gap, gap]).show(ui, |ui| {
            for (i, g) in self.cfg.games.iter().enumerate() {
                if !g.cover.is_empty() {
                    self.covers.entry(g.cover.clone()).or_insert_with(|| load_cover(&ctx, &g.cover));
                }
                let tex = self.covers.get(&g.cover).and_then(Option::as_ref);
                let a = card(ui, i, g, status(&self.cfg, g), tex, i == 0 && std::mem::take(&mut self.focus_first));
                if a != Act::None {
                    out = Some((i, a));
                }
                if (i + 1) % cols == 0 {
                    ui.end_row();
                }
            }
        });
        out
    }

    fn select(&mut self, i: usize) {
        self.sel = i;
        // text speed, encounters, battle speed and movies are per game: re-read them for this game's serials.
        let g = GameSettings::from_value(&self.doc, &self.cfg.games[i].serials);
        let s = &mut self.settings;
        (s.text_speed, s.encounter_rate, s.battle_speed, s.skip_fmv) = (g.text_speed, g.encounter_rate, g.battle_speed, g.skip_fmv);
        self.capture = None;
    }

    fn go(&mut self, ctx: &egui::Context, page: Page) {
        self.dir = if page == Page::Library { -1.0 } else { 1.0 };
        self.t0 = ctx.input(|i| i.time);
        self.page = page;
    }

    /// Returns (dirty, play).
    fn game_page(&mut self, ui: &mut egui::Ui) -> (bool, bool) {
        let mut dirty = false;
        let s = &mut self.settings;
        let cap = &mut self.capture;
        let (cache, g) = (&mut self.cfg.disc_cache, &mut self.cfg.games[self.sel]);
        let (ctx, scanning, found, secs) = (ui.ctx().clone(), self.scanning, &self.found, self.scan_secs);
        let (mut find, mut folder) = (false, None);
        let name = g.name.clone();
        ui.horizontal(|ui| {
            ui.label(RichText::new(name.clone()).family(cond()).size(26.0).extra_letter_spacing(2.0));
            ui.add_space(6.0);
            ui.label(RichText::new(g.platform.to_uppercase()).family(cond()).size(18.0).color(RED_HI));
        });
        let mut area = egui::ScrollArea::vertical().max_height(ui.available_height() - 66.0).auto_shrink([false, false]);
        if let Some(y) = self.shot.as_ref().and_then(|_| arg("--scroll")).and_then(|y| y.parse().ok()) {
            area = area.vertical_scroll_offset(y); // hidden `--scroll <px>`: screenshots of the lower part
        }
        area.show(ui, |ui| {
            heading(ui, "DISCS");
            hint(ui, &format!("Pick the .cue of your own {name} disc image (Japanese and/or US release). The Language option below chooses which disc boots."));
            ui.add_space(2.0);
            ui.horizontal(|ui| {
                find = ui.add_enabled(!scanning, egui::Button::new(caps("Find my discs", 15.0))).clicked();
                if ui.add_enabled(!scanning, egui::Button::new("Search a folder...")).clicked() {
                    folder = rfd::FileDialog::new().pick_folder();
                }
                if scanning {
                    ui.label(RichText::new("Searching Downloads, Documents and Desktop...").size(13.0).color(MUTED));
                }
            });
            for f in found.iter().filter(|f| f.game == self.sel) {
                let promo = disc::PROMO_SERIALS.contains(&f.disc.serial.as_str());
                let t = format!("{} \u{2014} {} ({}){}", name, disc::Region::of(&f.disc.serial).label(), f.disc.serial, if promo { " \u{b7} Disc 2 promo, not the game disc" } else { "" });
                let ok = f.disc.problem.is_none();
                let ic = if !ok { Icon::Cross } else if promo { Icon::Ring } else { Icon::Check };
                icon_line(ui, ic, if ok { PAPER } else { RED_HI }, &if ok { t } else { format!("{t} \u{b7} incomplete") })
                    .on_hover_text(f.path.to_string_lossy());
            }
            match (scanning, secs, found.iter().any(|f| f.game == self.sel)) {
                (false, Some(s), false) => hint(ui, &format!("No disc found ({s:.1} s). Use Search a folder, or Browse below.")),
                (false, Some(s), true) => hint(ui, &format!("Searched in {s:.1} s. Empty disc fields were filled in.")),
                _ => {}
            }
            ui.add_space(2.0);
            grid("discs").show(ui, |ui| {
                for (code, label) in [("ja", "Japanese disc"), ("en", "English disc")] {
                    grid_row(ui, label, |ui| {
                        let mut p = g.discs.get(code).cloned().unwrap_or_default();
                        let pick = || rfd::FileDialog::new().add_filter("Disc image", &["cue", "iso"]).pick_file();
                        let v = (path_state(&p, false) == PathState::Found).then(|| {
                            let r = disc::identify_cached(cache, std::path::Path::new(p.trim().trim_matches('"')));
                            disc::verdict(g, code, &r)
                        });
                        if path_row_v(ui, &mut p, false, PathState::NotSet, "not set", v, pick) {
                            g.discs.insert(code.into(), p);
                            dirty = true;
                        }
                    });
                }
            });
            heading(ui, "OPTIONS");
            grid("opts").show(ui, |ui| {
                grid_row(ui, "Language", |ui| dirty |= combo(ui, "lang", &mut s.language, &[("ja", "Japanese"), ("en", "English")]));
                grid_row(ui, "Text speed", |ui| dirty |= combo(ui, "ts", &mut s.text_speed, &[("normal", "Normal"), ("fast", "Fast"), ("instant", "Instant")]));
                grid_row(ui, "Encounters", |ui| {
                    dirty |= combo(ui, "enc", &mut s.encounter_rate, &[("normal", "Normal"), ("reduced", "Reduced (about half)"), ("none", "None")]);
                    hint(ui, "Scripted and boss battles still happen.");
                });
                grid_row(ui, "Battle speed", |ui| {
                    ui.horizontal(|ui| {
                        for n in 1..=3u32 {
                            dirty |= ui.selectable_value(&mut s.battle_speed, n, format!("x{n}")).changed();
                        }
                    });
                    hint(ui, "Faster battles; the sound is muted above x1.");
                });
                grid_row(ui, "Movies", |ui| {
                    dirty |= ui.checkbox(&mut s.skip_fmv, "Skip movies").changed();
                    hint(ui, "Start ends any movie.");
                });
                grid_row(ui, "Cover image (PNG)", |ui| {
                    let pick = || rfd::FileDialog::new().add_filter("PNG image", &["png"]).pick_file();
                    dirty |= path_row(ui, &mut g.cover, false, PathState::NotSet, "not set (title card)", pick);
                });
            });
            heading(ui, "HOTKEYS");
            hint(ui, "In game these change the options above and save them. Click a field, then press a key (Esc cancels).");
            hint(ui, "F5-F8, Tab and Esc are used by the game itself.");
            ui.add_space(2.0);
            grid("hotkeys").show(ui, |ui| {
                for (action, _, label) in HOTKEYS {
                    grid_row(ui, label, |ui| {
                        let waiting = *cap == Some(action);
                        let shown = if waiting { "press a key...".to_string() } else { s.hotkeys[action].clone() };
                        if ui.add(egui::Button::new(shown).min_size(egui::vec2(170.0, 0.0)).selected(waiting)).clicked() {
                            *cap = if waiting { None } else { Some(action) };
                        }
                    });
                }
            });
            if let Some(action) = *cap {
                let pressed = ui.input(|i| i.events.iter().find_map(|e| match e {
                    egui::Event::Key { key, pressed: true, repeat: false, .. } => Some(*key),
                    _ => None,
                }));
                if let Some(key) = pressed {
                    match sdl_key_name(key.name()) {
                        _ if key == egui::Key::Escape => *cap = None,
                        Some(k) if !RESERVED_KEYS.contains(&k.as_str()) => {
                            // a key already bound to another action swaps places with this one's old key
                            let old = s.hotkeys[action].clone();
                            if let Some(other) = s.hotkeys.iter().find(|(a, v)| **v == k && **a != action).map(|(a, _)| a.clone()) {
                                s.hotkeys.insert(other, old);
                            }
                            s.hotkeys.insert(action.to_string(), k);
                            *cap = None;
                            dirty = true;
                        }
                        _ => {} // reserved or not an SDL key: keep waiting
                    }
                }
            }
        });
        ui.add_space(4.0);
        let r = ui.allocate_space(egui::vec2(ui.available_width(), 46.0)).1;
        let play = red_button(ui, r, id("play"), "Play", 24.0, true).clicked();
        if let Some(d) = folder {
            let d = d.to_string_lossy().into_owned();
            if !self.cfg.scan_dirs.contains(&d) {
                self.cfg.scan_dirs.push(d);
            }
            find = true;
            dirty = true;
        }
        if find {
            self.start_scan(&ctx);
        }
        (dirty, play)
    }

    fn settings_page(&mut self, ui: &mut egui::Ui) -> bool {
        let (s, c) = (&mut self.settings, &mut self.cfg);
        ui.label(RichText::new("SETTINGS").family(cond()).size(26.0).extra_letter_spacing(2.0));
        ui.label(RichText::new("Shared by all games").color(MUTED));
        let mut dirty = false;
        egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            if ui.available_width() >= 800.0 {
                ui.columns(2, |col| {
                    dirty |= video(&mut col[0], s, c);
                    dirty |= audio_general(&mut col[1], s, c);
                });
            } else {
                dirty |= video(ui, s, c);
                dirty |= audio_general(ui, s, c);
            }
        });
        dirty
    }

    /// Hidden `--shot <file.png>`: save the window's own pixels after a few frames, then quit.
    fn screenshot(&mut self, ctx: &egui::Context) {
        let Some(path) = self.shot.clone() else { return };
        if self.scanning {
            return; // the thread repaints when it is done
        }
        self.frames += 1;
        ctx.send_viewport_cmd(ViewportCommand::Focus); // focus rings only draw in a focused window
        ctx.request_repaint();
        if self.frames == 8 {
            ctx.request_screenshot(move |img| {
                let save = || -> Option<()> {
                    let mut enc = png::Encoder::new(std::io::BufWriter::new(std::fs::File::create(&path).ok()?), img.width() as u32, img.height() as u32);
                    enc.set_color(png::ColorType::Rgba);
                    enc.set_depth(png::BitDepth::Eight);
                    let px: Vec<u8> = img.pixels.iter().flat_map(|c| c.to_array()).collect();
                    enc.write_header().ok()?.write_image_data(&px).ok()
                };
                let _ = save();
                std::process::exit(0);
            });
        }
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        REDUCE.store(self.cfg.reduce_motion, Ordering::Relaxed);
        let mut dirty = false;
        let mut play = false;
        let mut start: Option<update::Release> = None;
        while let Ok(m) = self.rx.try_recv() {
            match m {
                Msg::Found(r) if cfg!(debug_assertions) && std::env::var_os("PRM_UPDATE_AUTO").is_some() => start = Some(r),
                Msg::Found(r) => self.upd = Upd::Available(r),
                Msg::Failed(e) => self.upd = Upd::Failed(e),
                Msg::Native(l) => {
                    if let Some(n) = &mut self.native {
                        n.0 = l.progress;
                        n.1 = l.message;
                    }
                }
                Msg::NativeEnd(err) => {
                    if let Some((_, _, exe, args)) = self.native.take() {
                        if let Some(e) = err {
                            // Shown when the launcher comes back after the game (the game runs interpreted).
                            let _ = std::fs::create_dir_all(log_dir()).and_then(|_| std::fs::write(log_dir().join("last_error.txt"),
                                format!("Native code could not be prepared ({e}); the game ran with the interpreter.")));
                        }
                        self.launch(&ctx, exe, args);
                    }
                }
                Msg::Scan(found, cache, secs) => {
                    self.cfg.disc_cache.extend(cache);
                    self.found = found;
                    self.scan_secs = Some(secs);
                    self.scanning = false;
                    self.cfg.scanned = true;
                    self.apply_found();
                    self.dirty = true; // not the local `dirty`: the page below overwrites it
                }
                Msg::Done => {
                    // Files are swapped: start the new launcher and let this one go.
                    let _ = update::relaunch_exe().map(|me| std::process::Command::new(me).spawn());
                    ctx.send_viewport_cmd(ViewportCommand::Close);
                }
            }
        }
        self.screenshot(&ctx);
        if self.page != Page::Library && ctx.input(|i| i.key_pressed(egui::Key::Escape)) && !ctx.egui_wants_keyboard_input() && !egui::Popup::is_any_open(&ctx) {
            { self.error = None; self.go(&ctx, Page::Library); }
        }
        egui::CentralPanel::default().show(ui, |ui| {
            egui::Frame::new().inner_margin(egui::Margin::symmetric(20, 12)).show(ui, |ui| {
                ui.horizontal(|ui| {
                    if let Some(t) = &self.logo {
                        ui.add(egui::Image::new(t).fit_to_exact_size(egui::vec2(48.0, 48.0)));
                    }
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 0.0;
                        ui.label(RichText::new("PersonaRM").family(cond()).size(30.0).extra_letter_spacing(4.0));
                        ui.label(RichText::new(format!("Release v{}", update::VERSION.trim_start_matches('v'))).color(MUTED));
                    });
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if self.page == Page::Library {
                            if gear_button(ui) {
                                { self.error = None; self.go(&ctx, Page::Settings); }
                            }
                        } else if ui.add(egui::Button::new(caps("Back", 16.0))).on_hover_text("Esc").clicked() {
                            { self.error = None; self.go(&ctx, Page::Library); }
                        }
                    });
                });
                ui.separator();
                if let Some(e) = &self.error {
                    ui.label(RichText::new(e).color(RED_HI));
                }
                if let Some((p, msg, _, _)) = &self.native {
                    // Only worth a bar when there is work: a ready cache answers within a moment.
                    ui.label(RichText::new("Preparing native code for your disc\u{2026} (first time only)").color(PAPER));
                    ui.add(egui::ProgressBar::new(*p).text(msg.as_str()).animate(true));
                    ctx.request_repaint_after(std::time::Duration::from_millis(100));
                }
                let bt = anim(&ctx, "banner", matches!(self.upd, Upd::Available(_)), 0.18);
                match &self.upd {
                    Upd::None => {}
                    Upd::Working => {
                        ui.label(RichText::new("Downloading the update...").color(MUTED));
                    }
                    Upd::Failed(e) => {
                        ui.label(RichText::new(format!("Update failed: {e}")).color(RED_HI).small());
                    }
                    Upd::Available(r) => {
                        // Slides down from under the header: a clipped child shifted up by (1 - t) * h.
                        let h = 40.0;
                        let (slot, _) = ui.allocate_exact_size(egui::vec2(ui.available_width(), (h + 6.0) * bt), egui::Sense::hover());
                        let mut c = ui.new_child(egui::UiBuilder::new().max_rect(egui::Rect::from_min_size(slot.min - egui::vec2(0.0, h * (1.0 - bt)), egui::vec2(slot.width(), h))));
                        c.set_clip_rect(slot);
                        c.multiply_opacity(bt);
                        let br = c.max_rect();
                        c.painter().rect_filled(br, 0.0, INK_3);
                        c.painter().rect_stroke(br, 0.0, egui::Stroke::new(1.0, RED_DEEP), egui::StrokeKind::Inside);
                        c.painter().rect_filled(egui::Rect::from_min_size(br.min, egui::vec2(4.0, h)), 0.0, RED_HI);
                        let txt = c.painter().layout_no_wrap(format!("Update available   v{}  to  {}", update::VERSION.trim_start_matches('v'), r.tag), egui::FontId::new(17.0, cond()), PAPER);
                        c.painter().galley(egui::pos2(br.min.x + 16.0, br.center().y - txt.size().y / 2.0), txt, PAPER);
                        let b = egui::Rect::from_center_size(egui::pos2(br.max.x - 52.0, br.center().y), egui::vec2(88.0, 28.0));
                        if red_button(&mut c, b, id("upd"), "Update", 15.0, true).clicked() {
                            start = Some(r.clone());
                        }
                    }
                }
                // Page change: the new page slides in 28 px (from the right going deeper, from the left going back) and fades up.
                let e = if REDUCE.load(Ordering::Relaxed) { 1.0 } else { ((ctx.input(|i| i.time) - self.t0) as f32 / 0.18).clamp(0.0, 1.0) };
                if e < 1.0 {
                    ctx.request_repaint();
                }
                let off = (1.0 - egui::emath::easing::cubic_out(e)) * 28.0 * self.dir;
                let area = ui.available_rect_before_wrap();
                let mut open = None;
                ui.scope_builder(egui::UiBuilder::new().max_rect(area.translate(egui::vec2(off, 0.0))), |ui| {
                    ui.multiply_opacity(e);
                    match self.page {
                        Page::Library => egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| open = self.library(ui)).inner,
                        Page::Game => (dirty, play) = self.game_page(ui),
                        Page::Settings => dirty = self.settings_page(ui),
                    }
                });
                if let Some((i, a)) = open {
                    self.select(i);
                    match a {
                        Act::Play => play = true,
                        _ => { self.error = None; self.go(&ctx, Page::Game) }
                    }
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
        if play && self.native.is_none() {
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
    if let Some(me) = update::relaunch_exe() {
        let _ = std::process::Command::new(me).spawn();
    }
}

fn main() -> eframe::Result {
    let argv: Vec<String> = std::env::args().collect();
    // Old installs start `launcher.exe` (same binary as PersonaRM.exe): hand over so they land on the new name.
    if let Some(new) = std::env::current_exe().ok().and_then(|me| update::shim_target(&me)) {
        if std::process::Command::new(new).args(&argv[1..]).spawn().is_ok() {
            return Ok(());
        }
    }
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
        .with_inner_size([960.0, 740.0])
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
