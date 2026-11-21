//! Launcher data model: its own config (launcher.json), the game settings it edits in the game's
//! settings.json (unknown keys preserved), and the command line used to start the game.
//! No UI code here so everything is unit-testable.

use serde::{Deserialize, Serialize};
use serde_json::{Map, Value, json};
use std::collections::BTreeMap;
use std::path::PathBuf;

/// One entry of the library. Data-driven: a future PSP/Nintendo entry only needs a different
/// `exe`/args scheme; for now every game is started by `personarm_sdl <cue>`.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Game {
    pub name: String,
    /// Disc serials of this game, used to write the per-game `game.<serial>` settings section.
    pub serials: Vec<String>,
    /// Language ("ja" / "en") -> disc image (.cue), chosen by the user.
    #[serde(default)]
    pub discs: BTreeMap<String, String>,
    /// Platform badge shown on the card ("PS1", later "PSP", ...).
    #[serde(default = "default_platform")]
    pub platform: String,
    /// Optional cover image (PNG) picked by the user; empty = drawn title card. Never shipped.
    #[serde(default)]
    pub cover: String,
    /// Executable that runs this game; empty = the launcher's `game_exe` / `personarm_sdl` next to it.
    #[serde(default)]
    pub exe: String,
    /// Extra command-line arguments appended after the disc and the launcher's own ones.
    #[serde(default)]
    pub args: Vec<String>,
}

fn default_platform() -> String { "PS1".into() }

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Status {
    Ready,
    NeedsDisc,
    NotPlayable,
}

/// Executable for `g`: its own `exe` if set and present, else the shared one.
pub fn game_exe_for(cfg: &LauncherConfig, g: &Game) -> Option<PathBuf> {
    if !g.exe.is_empty() {
        let p = PathBuf::from(&g.exe);
        return p.is_file().then_some(p);
    }
    find_game_exe(cfg)
}

/// State of a path the user typed: shown next to every path field.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum PathState {
    Found,
    Missing,
    NotSet,
}

/// Empty = not set; otherwise it must exist as a file (or a folder when `dir`). Pasted quotes are ignored.
pub fn path_state(p: &str, dir: bool) -> PathState {
    let p = p.trim().trim_matches('"');
    if p.is_empty() {
        return PathState::NotSet;
    }
    let path = std::path::Path::new(p);
    if if dir { path.is_dir() } else { path.is_file() } { PathState::Found } else { PathState::Missing }
}

pub fn status(cfg: &LauncherConfig, g: &Game) -> Status {
    if game_exe_for(cfg, g).is_none() {
        Status::NotPlayable
    } else if g.discs.values().any(|d| path_state(d, false) == PathState::Found) {
        Status::Ready
    } else {
        Status::NeedsDisc
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct LauncherConfig {
    /// Path to personarm_sdl; empty = look next to the launcher.
    pub game_exe: String,
    /// "software" | "gpu" (CLI only in the game).
    pub renderer: String,
    /// Internal resolution multiplier; 0 = auto (gpu only).
    pub resolution: u32,
    /// Look for a newer release on start (one background request, silent when offline).
    pub check_updates: bool,
    /// Make every launcher transition instant.
    pub reduce_motion: bool,
    pub games: Vec<Game>,
    /// Extra folders the user picked for "Find my discs".
    pub scan_dirs: Vec<String>,
    /// The automatic first-launch disc search already ran.
    pub scanned: bool,
    /// Disc identification by path (valid while size and mtime match): see `disc::identify_cached`.
    pub disc_cache: crate::disc::Cache,
}

impl Default for LauncherConfig {
    fn default() -> Self {
        Self {
            game_exe: String::new(),
            renderer: "software".into(),
            resolution: 1,
            check_updates: true,
            reduce_motion: false,
            scan_dirs: Vec::new(),
            scanned: false,
            disc_cache: Default::default(),
            games: vec![Game {
                name: "Persona 2: Eternal Punishment".into(),
                serials: ["SLPS-02785", "SLPS-02786", "SLUS-01158", "SLUS-01339"].map(String::from).to_vec(),
                discs: BTreeMap::new(),
                platform: default_platform(),
                cover: String::new(),
                exe: String::new(),
                args: Vec::new(),
            }],
        }
    }
}

/// In-game hotkeys (`input.hotkeys` of settings.json): action, default key, label.
pub const HOTKEYS: [(&str, &str, &str); 4] = [
    ("encounter_rate", "F1", "Encounters"),
    ("battle_speed", "F2", "Battle speed"),
    ("skip_fmv", "F3", "Skip movies"),
    ("text_speed", "F4", "Text speed"),
];

/// Keys the game itself uses (save states, fast-forward, quit): not offered for hotkeys.
pub const RESERVED_KEYS: [&str; 6] = ["F5", "F6", "F7", "F8", "Tab", "Escape"];

/// SDL scancode name for the name egui gives a key (`Key::name`), or None when SDL has no such key.
pub fn sdl_key_name(egui_name: &str) -> Option<String> {
    Some(match egui_name {
        "Enter" => "Return".into(),
        "ArrowUp" => "Up".into(),
        "ArrowDown" => "Down".into(),
        "ArrowLeft" => "Left".into(),
        "ArrowRight" => "Right".into(),
        "Minus" => "-".into(),
        "Plus" | "Equals" => "=".into(),
        "Period" => ".".into(),
        "Comma" => ",".into(),
        "Slash" => "/".into(),
        "Semicolon" => ";".into(),
        "Quote" => "'".into(),
        "OpenBracket" => "[".into(),
        "CloseBracket" => "]".into(),
        n if n.len() == 1 && n.chars().all(|c| c.is_ascii_alphanumeric()) => n.to_ascii_uppercase(),
        n if ["Space", "Backspace", "Delete", "Insert", "Home", "End", "PageUp", "PageDown"].contains(&n) => n.into(),
        n if n.starts_with('F') && n[1..].parse::<u32>().is_ok_and(|k| (1..=12).contains(&k)) => n.into(),
        _ => return None,
    })
}

/// The subset of the game's settings.json the launcher edits.
#[derive(Clone, Debug, PartialEq)]
pub struct GameSettings {
    pub language: String,
    pub fps: u32,
    pub window_mode: String,
    pub widescreen: String,
    pub volume: u32,
    pub mute: bool,
    pub text_speed: String,
    /// Per game, like `text_speed`: normal | reduced | none.
    pub encounter_rate: String,
    /// 1..3.
    pub battle_speed: u32,
    pub skip_fmv: bool,
    /// Action -> SDL key name, all of `HOTKEYS`.
    pub hotkeys: BTreeMap<String, String>,
    pub save_dir: String,
    pub pause_on_focus_loss: bool,
    /// Native (recompiled) code: per-game `recomp` key, on unless set to false.
    pub native: bool,
}

impl Default for GameSettings {
    fn default() -> Self {
        Self {
            language: "ja".into(),
            fps: 60,
            window_mode: "windowed".into(),
            widescreen: "off".into(),
            volume: 100,
            mute: false,
            text_speed: "normal".into(),
            encounter_rate: "normal".into(),
            battle_speed: 1,
            skip_fmv: false,
            hotkeys: HOTKEYS.iter().map(|h| (h.0.to_string(), h.1.to_string())).collect(),
            save_dir: String::new(),
            pause_on_focus_loss: true,
            native: true,
        }
    }
}

fn get<'a>(doc: &'a Value, path: &[&str]) -> Option<&'a Value> {
    path.iter().try_fold(doc, |v, k| v.get(k))
}

/// Set `path` in `doc`, creating (or replacing non-object) parents; sibling keys are untouched.
fn set(doc: &mut Value, path: &[&str], val: Value) {
    let mut cur = doc;
    for k in &path[..path.len() - 1] {
        if !cur.is_object() {
            *cur = Value::Object(Map::new());
        }
        cur = cur.as_object_mut().unwrap().entry(*k).or_insert_with(|| Value::Object(Map::new()));
    }
    if !cur.is_object() {
        *cur = Value::Object(Map::new());
    }
    cur.as_object_mut().unwrap().insert(path[path.len() - 1].into(), val);
}

impl GameSettings {
    /// Read the known keys; anything missing or of the wrong type keeps its default.
    pub fn from_value(doc: &Value, serials: &[String]) -> Self {
        let d = Self::default();
        let s = |p: &[&str], def: &str| get(doc, p).and_then(Value::as_str).unwrap_or(def).to_string();
        let u = |p: &[&str], def: u32| get(doc, p).and_then(Value::as_u64).map_or(def, |n| n as u32);
        let b = |p: &[&str], def: bool| get(doc, p).and_then(Value::as_bool).unwrap_or(def);
        let text_speed = serials
            .iter()
            .find_map(|id| get(doc, &["game", id, "text_speed"]).and_then(Value::as_str))
            .unwrap_or(&d.text_speed)
            .to_string();
        let native = serials.iter().find_map(|id| get(doc, &["game", id, "recomp"]).and_then(Value::as_bool)).unwrap_or(d.native);
        let game = |k: &str| serials.iter().find_map(|id| get(doc, &["game", id, k]));
        let mut hotkeys = d.hotkeys.clone();
        for (action, key) in hotkeys.iter_mut() {
            if let Some(k) = get(doc, &["input", "hotkeys", action]).and_then(Value::as_str).filter(|k| !k.is_empty()) {
                *key = k.to_string();
            }
        }
        Self {
            encounter_rate: game("encounter_rate").and_then(Value::as_str).unwrap_or(&d.encounter_rate).to_string(),
            battle_speed: game("battle_speed").and_then(Value::as_u64).filter(|n| (1..=3).contains(n)).map_or(d.battle_speed, |n| n as u32),
            skip_fmv: game("skip_fmv").and_then(Value::as_bool).unwrap_or(d.skip_fmv),
            hotkeys,
            native,
            language: s(&["general", "language"], &d.language),
            fps: u(&["video", "fps_target"], d.fps),
            window_mode: s(&["video", "window_mode"], &d.window_mode),
            widescreen: s(&["video", "widescreen"], &d.widescreen),
            volume: u(&["audio", "master_volume"], d.volume).min(100),
            mute: b(&["audio", "mute"], d.mute),
            text_speed,
            save_dir: s(&["general", "save_dir"], ""),
            pause_on_focus_loss: b(&["general", "pause_on_focus_loss"], d.pause_on_focus_loss),
        }
    }

    /// Write the known keys into `doc`; every other key at every level is preserved.
    /// `resolution` goes to `video.resolution_scale` only when the game file accepts it (1..4).
    pub fn apply(&self, doc: &mut Value, serials: &[String], resolution: u32) {
        if !doc.is_object() {
            *doc = Value::Object(Map::new());
        }
        if doc.get("version").is_none() {
            set(doc, &["version"], json!(1));
        }
        set(doc, &["general", "language"], json!(self.language));
        set(doc, &["general", "save_dir"], json!(self.save_dir));
        set(doc, &["general", "pause_on_focus_loss"], json!(self.pause_on_focus_loss));
        set(doc, &["video", "fps_target"], json!(self.fps));
        set(doc, &["video", "window_mode"], json!(self.window_mode));
        set(doc, &["video", "widescreen"], json!(self.widescreen));
        if (1..=4).contains(&resolution) {
            set(doc, &["video", "resolution_scale"], json!(resolution));
        }
        set(doc, &["audio", "master_volume"], json!(self.volume));
        set(doc, &["audio", "mute"], json!(self.mute));
        for id in serials {
            set(doc, &["game", id, "text_speed"], json!(self.text_speed));
            set(doc, &["game", id, "recomp"], json!(self.native));
            set(doc, &["game", id, "encounter_rate"], json!(self.encounter_rate));
            set(doc, &["game", id, "battle_speed"], json!(self.battle_speed));
            set(doc, &["game", id, "skip_fmv"], json!(self.skip_fmv));
        }
        for (action, key) in &self.hotkeys {
            set(doc, &["input", "hotkeys", action], json!(key));
        }
    }
}

/// Arguments for `personarm_sdl`. Renderer and scale are CLI-only (the file accepts 1..4 and no renderer).
pub fn build_args(settings: &str, cue: &str, cfg: &LauncherConfig, s: &GameSettings) -> Vec<String> {
    let mut a: Vec<String> = vec![cue.into(), "--settings".into(), settings.into(), "--renderer".into(), cfg.renderer.clone()];
    // The host rejects an internal resolution above 1x together with frame interpolation (fps > 60).
    if s.fps <= 60 {
        let gpu = cfg.renderer == "gpu";
        let r = match cfg.resolution {
            0 if gpu => "auto".to_string(),
            n => n.clamp(1, if gpu { 9 } else { 4 }).to_string(),
        };
        a.extend(["--resolution-scale".into(), r]);
    }
    a
}

/// Disc for the selected language, if the user set one.
pub fn disc_for<'a>(g: &'a Game, language: &str) -> Option<&'a str> {
    g.discs.get(language).map(|p| p.trim().trim_matches('"')).filter(|p| !p.is_empty())
}

// ── paths ──

/// Per-user data root, same one the game uses (`%APPDATA%` on Windows).
fn data_root() -> PathBuf {
    let var = |k: &str| std::env::var_os(k).map(PathBuf::from);
    if cfg!(windows) {
        var("APPDATA").unwrap_or_default()
    } else if let Some(x) = var("XDG_DATA_HOME") {
        x
    } else if cfg!(target_os = "macos") {
        var("HOME").unwrap_or_default().join("Library/Application Support")
    } else {
        var("HOME").unwrap_or_default().join(".local/share")
    }
}

pub fn launcher_dir() -> PathBuf { data_root().join("PersonaRM") }
pub fn launcher_json() -> PathBuf { launcher_dir().join("launcher.json") }
pub fn log_dir() -> PathBuf { launcher_dir().join("logs") }
/// Same file as `SDL_GetPrefPath("PersonaRM","PersonaRM")/settings.json`.
pub fn game_settings_json() -> PathBuf { launcher_dir().join("PersonaRM").join("settings.json") }

pub fn exe_name() -> &'static str { if cfg!(windows) { "personarm_sdl.exe" } else { "personarm_sdl" } }

/// `game_exe` if it exists, else `personarm_sdl` next to the launcher.
pub fn find_game_exe(cfg: &LauncherConfig) -> Option<PathBuf> {
    let p = PathBuf::from(&cfg.game_exe);
    if !cfg.game_exe.is_empty() && p.is_file() {
        return Some(p);
    }
    let next = std::env::current_exe().ok()?.parent()?.join(exe_name());
    next.is_file().then_some(next)
}

/// `personarm_native` next to the game executable: prepares the native code for a disc.
pub fn native_tool_for(game_exe: &std::path::Path) -> PathBuf {
    game_exe.with_file_name(if cfg!(windows) { "personarm_native.exe" } else { "personarm_native" })
}

/// One `personarm_native --json` progress line.
#[derive(Debug, PartialEq)]
pub struct NativeLine {
    pub stage: String,
    pub progress: f32,
    pub message: String,
    /// Final line only: whether the native code is ready.
    pub ok: Option<bool>,
}

pub fn parse_native_line(line: &str) -> Option<NativeLine> {
    let v: Value = serde_json::from_str(line.trim()).ok()?;
    let stage = v.get("stage")?.as_str()?.to_string();
    Some(NativeLine {
        progress: v.get("progress").and_then(Value::as_f64).unwrap_or(if stage == "done" { 1.0 } else { 0.0 }) as f32,
        message: v.get("message").and_then(Value::as_str).unwrap_or("").to_string(),
        ok: v.get("ok").and_then(Value::as_bool),
        stage,
    })
}

// ── io ──

/// Hand-edited files (Windows PowerShell, Notepad) often start with a UTF-8 BOM.
fn strip_bom(t: &str) -> &str { t.strip_prefix('\u{feff}').unwrap_or(t) }

pub fn load_launcher()-> LauncherConfig {
    std::fs::read_to_string(launcher_json()).ok().and_then(|t| serde_json::from_str(strip_bom(&t)).ok()).unwrap_or_default()
}

fn write_atomic(path: &std::path::Path, text: &str) -> std::io::Result<()> {
    if let Some(d) = path.parent() {
        std::fs::create_dir_all(d)?;
    }
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, text)?;
    std::fs::rename(&tmp, path)
}

pub fn save_launcher(c: &LauncherConfig) -> std::io::Result<()> {
    write_atomic(&launcher_json(), &serde_json::to_string_pretty(c).unwrap())
}

/// The game's settings document; missing or malformed file = empty object (never an error).
pub fn load_game_doc() -> Value {
    std::fs::read_to_string(game_settings_json())
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(strip_bom(&t)).ok())
        .filter(Value::is_object)
        .unwrap_or_else(|| json!({}))
}

pub fn save_game_doc(doc: &Value) -> std::io::Result<()> {
    write_atomic(&game_settings_json(), &serde_json::to_string_pretty(doc).unwrap())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn serials() -> Vec<String> { vec!["SLPS-02785".into(), "SLUS-01158".into()] }

    #[test]
    fn native_setting_defaults_on_and_roundtrips() {
        assert!(GameSettings::from_value(&json!({}), &serials()).native);
        let mut s = GameSettings::default();
        s.native = false;
        let mut doc = json!({"game": {"SLPS-02785": {"mods": "x"}}});
        s.apply(&mut doc, &serials(), 0);
        assert_eq!(doc["game"]["SLPS-02785"]["recomp"], json!(false));
        assert_eq!(doc["game"]["SLPS-02785"]["mods"], json!("x"));
        assert!(!GameSettings::from_value(&doc, &serials()).native);
    }

    #[test]
    fn native_progress_lines() {
        let l = parse_native_line(r#"{"stage":"compile","progress":0.5,"message":"Compiling resident"}"#).unwrap();
        assert_eq!((l.stage.as_str(), l.progress, l.message.as_str(), l.ok), ("compile", 0.5, "Compiling resident", None));
        let d = parse_native_line(r#"{"stage":"done","ok":true,"cached":true,"dir":"x"}"#).unwrap();
        assert_eq!((d.progress, d.ok), (1.0, Some(true)));
        let e = parse_native_line(r#"{"stage":"error","ok":false,"message":"no FILEPOS.DAT"}"#).unwrap();
        assert_eq!((e.ok, e.message.as_str()), (Some(false), "no FILEPOS.DAT"));
        assert!(parse_native_line("garbage").is_none());
        assert_eq!(native_tool_for(std::path::Path::new("a/personarm_sdl.exe")).parent(), Some(std::path::Path::new("a")));
    }

    #[test]
    fn apply_preserves_unknown_keys() {
        let mut doc = json!({
            "version": 1,
            "launcher": {"x": 1},
            "video": {"window_scale": 5, "future_key": "keep", "fps_target": 30},
            "input": {"keyboard": {"cross": ["X"]}},
            "game": {"SLPS-02785": {"memcard1": "a.mcd", "text_speed": "fast"}, "OTHER": {"a": 1}}
        });
        let s = GameSettings { fps: 120, language: "en".into(), text_speed: "instant".into(), mute: true, volume: 40, ..Default::default() };
        s.apply(&mut doc, &serials(), 3);
        assert_eq!(doc["launcher"]["x"], 1);
        assert_eq!(doc["video"]["window_scale"], 5);
        assert_eq!(doc["video"]["future_key"], "keep");
        assert_eq!(doc["input"]["keyboard"]["cross"][0], "X");
        assert_eq!(doc["game"]["SLPS-02785"]["memcard1"], "a.mcd");
        assert_eq!(doc["game"]["OTHER"]["a"], 1);
        assert_eq!(doc["video"]["fps_target"], 120);
        assert_eq!(doc["video"]["resolution_scale"], 3);
        assert_eq!(doc["general"]["language"], "en");
        assert_eq!(doc["audio"]["master_volume"], 40);
        assert_eq!(doc["audio"]["mute"], true);
        assert_eq!(doc["game"]["SLPS-02785"]["text_speed"], "instant");
        assert_eq!(doc["game"]["SLUS-01158"]["text_speed"], "instant");
    }

    #[test]
    fn gameplay_options_and_hotkeys_roundtrip() {
        let mut doc = json!({
            "input": {"hotkeys": {"skip_fmv": "F9", "future": "X"}},
            "game": {"SLPS-02785": {"note": "keep", "encounter_rate": "reduced", "battle_speed": 3, "skip_fmv": true}}
        });
        let mut s = GameSettings::from_value(&doc, &serials());
        assert_eq!((s.encounter_rate.as_str(), s.battle_speed, s.skip_fmv), ("reduced", 3, true));
        assert_eq!(s.hotkeys["skip_fmv"], "F9");
        assert_eq!(s.hotkeys["text_speed"], "F4");
        s.encounter_rate = "none".into();
        s.battle_speed = 2;
        s.hotkeys.insert("battle_speed".into(), "G".into());
        s.apply(&mut doc, &serials(), 0);
        assert_eq!(doc["game"]["SLPS-02785"]["note"], "keep");
        assert_eq!(doc["game"]["SLPS-02785"]["encounter_rate"], "none");
        assert_eq!(doc["game"]["SLUS-01158"]["battle_speed"], 2);
        assert_eq!(doc["game"]["SLUS-01158"]["skip_fmv"], true);
        assert_eq!(doc["input"]["hotkeys"]["battle_speed"], "G");
        assert_eq!(doc["input"]["hotkeys"]["future"], "X");
        assert_eq!(GameSettings::from_value(&doc, &serials()), s);
        // a bad value falls back to the default
        let bad = json!({"game": {"SLPS-02785": {"battle_speed": 9, "encounter_rate": 4}}});
        assert_eq!(GameSettings::from_value(&bad, &serials()).battle_speed, 1);
        assert_eq!(GameSettings::from_value(&bad, &serials()).encounter_rate, "normal");
    }

    #[test]
    fn egui_key_names_map_to_sdl() {
        for (e, sdl) in [("F1", "F1"), ("Enter", "Return"), ("a", "A"), ("A", "A"), ("5", "5"), ("ArrowLeft", "Left"), ("Space", "Space")] {
            assert_eq!(sdl_key_name(e).as_deref(), Some(sdl), "{e}");
        }
        assert_eq!(sdl_key_name("F13"), None);
        assert_eq!(sdl_key_name("Copy"), None);
    }

    #[test]
    fn apply_to_empty_or_garbage_and_roundtrip() {
        for mut doc in [json!({}), json!(null), json!([1]), json!({"video": 7})] {
            let s = GameSettings { widescreen: "21:9".into(), window_mode: "borderless".into(), ..Default::default() };
            s.apply(&mut doc, &serials(), 0);
            assert_eq!(doc["version"], 1);
            assert!(doc["video"].get("resolution_scale").is_none(), "auto/>4 is CLI-only");
            assert_eq!(GameSettings::from_value(&doc, &serials()), s);
        }
    }

    #[test]
    fn high_resolution_not_written_to_file() {
        let mut doc = json!({"video": {"resolution_scale": 2}});
        GameSettings::default().apply(&mut doc, &serials(), 9);
        assert_eq!(doc["video"]["resolution_scale"], 2);
    }

    #[test]
    fn args() {
        let mut c = LauncherConfig { renderer: "gpu".into(), resolution: 0, ..Default::default() };
        let s = GameSettings::default();
        assert_eq!(build_args("S.json", "d.cue", &c, &s), ["d.cue", "--settings", "S.json", "--renderer", "gpu", "--resolution-scale", "auto"]);
        c.resolution = 9;
        assert_eq!(build_args("S", "d", &c, &s)[6], "9");
        c.renderer = "software".into();
        assert_eq!(build_args("S", "d", &c, &s)[6], "4"); // software caps at 4x
        c.resolution = 0;
        assert_eq!(build_args("S", "d", &c, &s)[6], "1");
        let fast = GameSettings { fps: 120, ..Default::default() };
        assert_eq!(build_args("S", "d", &c, &fast), ["d", "--settings", "S", "--renderer", "software"]);
    }

    #[test]
    fn old_launcher_json_still_loads() {
        let old = r#"{"game_exe":"x.exe","renderer":"gpu","resolution":2,"check_updates":false,
            "games":[{"name":"P2","serials":["SLPS-02785"],"discs":{"ja":"a.cue"}}]}"#;
        let c: LauncherConfig = serde_json::from_str(old).unwrap();
        let g = &c.games[0];
        assert_eq!((g.platform.as_str(), g.cover.as_str(), g.exe.as_str()), ("PS1", "", ""));
        assert!(g.args.is_empty());
        assert_eq!(disc_for(g, "ja"), Some("a.cue"));
        assert_eq!(c.renderer, "gpu");
    }

    #[test]
    fn status_values() {
        let mut c = LauncherConfig::default();
        let mut g = c.games.remove(0);
        g.exe = "does/not/exist".into();
        assert_eq!(status(&c, &g), Status::NotPlayable);
        g.exe = std::env::current_exe().unwrap().to_string_lossy().into_owned();
        assert_eq!(status(&c, &g), Status::NeedsDisc);
        g.discs.insert("ja".into(), "missing.cue".into());
        assert_eq!(status(&c, &g), Status::NeedsDisc); // a path that does not exist is not a disc
        g.discs.insert("ja".into(), g.exe.clone());
        assert_eq!(status(&c, &g), Status::Ready);
    }

    #[test]
    fn path_states() {
        let exe = std::env::current_exe().unwrap();
        let (f, d) = (exe.to_string_lossy().into_owned(), exe.parent().unwrap().to_string_lossy().into_owned());
        assert_eq!(path_state("", false), PathState::NotSet);
        assert_eq!(path_state("  ", true), PathState::NotSet);
        assert_eq!(path_state(&f, false), PathState::Found);
        assert_eq!(path_state(&format!("\"{f}\""), false), PathState::Found); // "Copy as path" quotes
        assert_eq!(path_state(&d, false), PathState::Missing); // a folder is not a file
        assert_eq!(path_state(&d, true), PathState::Found);
        assert_eq!(path_state("no/such/file.cue", false), PathState::Missing);
    }

    #[test]
    fn disc_lookup() {
        let mut g = LauncherConfig::default().games.remove(0);
        g.discs.insert("ja".into(), "a.cue".into());
        g.discs.insert("en".into(), String::new());
        assert_eq!(disc_for(&g, "ja"), Some("a.cue"));
        assert_eq!(disc_for(&g, "en"), None);
        g.discs.insert("en".into(), "\"b.cue\"".into());
        assert_eq!(disc_for(&g, "en"), Some("b.cue"));
    }
}
