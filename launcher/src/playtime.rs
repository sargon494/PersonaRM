//! Playtime per game: written by the windowless watcher (`--run`) when a session ends, read by the UI.
//! Lives in its own `playtime.json` so a damaged file can never break `launcher.json`. Time is wall time
//! while the game process is open (a window paused for lost focus still counts). No UI code.

use crate::model::{launcher_dir, strip_bom, write_atomic};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Sessions shorter than this (crash on start, quick quit) are counted apart and add no time.
pub const MIN_SECS: u64 = 10;
/// Recent sessions kept per game.
pub const KEEP: usize = 50;

pub fn playtime_json() -> PathBuf {
    launcher_dir().join("playtime.json")
}

#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
#[serde(default)]
pub struct Session {
    /// Unix seconds at start; the end is `start + secs`.
    pub start: u64,
    pub secs: u64,
    /// Serial of the disc that was launched ("SLPS-02785"), "" if it could not be read.
    pub serial: String,
    /// The game exited with status 0 (false = crash or error exit).
    pub ok: bool,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
#[serde(default)]
pub struct Stats {
    pub secs: u64,
    pub sessions: u32,
    /// Sessions under `MIN_SECS`: counted here only.
    pub short: u32,
    pub longest: u64,
    /// Unix seconds of the first session start and the last session end (0 = none).
    pub first: u64,
    pub last: u64,
}

impl Stats {
    fn add(&mut self, s: &Session) {
        if s.secs < MIN_SECS {
            self.short += 1;
            return;
        }
        self.secs += s.secs;
        self.sessions += 1;
        self.longest = self.longest.max(s.secs);
        if self.first == 0 || s.start < self.first {
            self.first = s.start;
        }
        self.last = self.last.max(s.start + s.secs);
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
#[serde(default)]
pub struct GameStats {
    #[serde(flatten)]
    pub total: Stats,
    /// Per disc serial (so Japan and US are told apart).
    pub discs: BTreeMap<String, Stats>,
    /// Oldest first, at most `KEEP`.
    pub recent: Vec<Session>,
}

/// Keyed by the library game name.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
#[serde(default)]
pub struct Playtime {
    pub games: BTreeMap<String, GameStats>,
}

impl Playtime {
    pub fn record(&mut self, game: &str, s: Session) {
        let g = self.games.entry(game.to_string()).or_default();
        g.total.add(&s);
        g.discs.entry(s.serial.clone()).or_default().add(&s);
        g.recent.push(s);
        if g.recent.len() > KEEP {
            g.recent.drain(..g.recent.len() - KEEP);
        }
    }
}

/// Missing file = empty. A file that does not parse is moved to `.corrupt` (kept for the user) and
/// the stats start over; it is never an error and never touches `launcher.json`.
pub fn load_from(path: &Path) -> Playtime {
    let Ok(t) = std::fs::read_to_string(path) else {
        return Playtime::default();
    };
    serde_json::from_str(strip_bom(&t)).unwrap_or_else(|_| {
        let _ = std::fs::rename(path, path.with_extension("json.corrupt"));
        Playtime::default()
    })
}

pub fn save_to(path: &Path, p: &Playtime) -> std::io::Result<()> {
    write_atomic(path, &serde_json::to_string_pretty(p).unwrap())
}

pub fn load() -> Playtime {
    load_from(&playtime_json())
}
pub fn save(p: &Playtime) -> std::io::Result<()> {
    save_to(&playtime_json(), p)
}

// ── time ──

pub fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

/// Seconds east of UTC for the local zone (Windows API; elsewhere UTC, shortcut: day names off by the offset).
pub fn tz_offset() -> i64 {
    #[cfg(windows)]
    {
        #[repr(C)]
        struct Tzi {
            bias: i32,
            _a: [u16; 32],
            _b: [u16; 8],
            std_bias: i32,
            _c: [u16; 32],
            _d: [u16; 8],
            dst_bias: i32,
        }
        unsafe extern "system" {
            fn GetTimeZoneInformation(p: *mut Tzi) -> u32;
        }
        // SAFETY: plain C struct of integers, filled by the call.
        let mut t: Tzi = unsafe { std::mem::zeroed() };
        let r = unsafe { GetTimeZoneInformation(&mut t) };
        let b = t.bias
            + match r {
                1 => t.std_bias,
                2 => t.dst_bias,
                _ => 0,
            };
        return -(b as i64) * 60;
    }
    #[allow(unreachable_code)]
    0
}

/// "45 s", "12 min", "3 h", "12 h 30 min".
pub fn fmt_dur(s: u64) -> String {
    match s {
        0..=59 => format!("{s} s"),
        60..=3599 => format!("{} min", s / 60),
        _ if s % 3600 / 60 == 0 => format!("{} h", s / 3600),
        _ => format!("{} h {} min", s / 3600, s % 3600 / 60),
    }
}

const MONTHS: [&str; 12] = [
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
];

/// (year, month, day) of a day count since 1970-01-01.
fn civil(days: i64) -> (i64, usize, i64) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (yoe + era * 400 + (m <= 2) as i64, m as usize, d)
}

/// "today", "yesterday", "3 days ago", then "3 Mar 2026" from 30 days on.
pub fn rel_date(ts: u64, now: u64, tz: i64) -> String {
    let day = |t: i64| (t + tz).div_euclid(86_400);
    let ago = day(now as i64) - day(ts as i64);
    match ago {
        i64::MIN..=0 => "today".into(),
        1 => "yesterday".into(),
        2..=29 => format!("{ago} days ago"),
        _ => {
            let (y, m, d) = civil(day(ts as i64));
            format!("{d} {} {y}", MONTHS[m - 1])
        }
    }
}

/// "21:05" in local time.
pub fn clock(ts: u64, tz: i64) -> String {
    let s = (ts as i64 + tz).rem_euclid(86_400);
    format!("{:02}:{:02}", s / 3600, s % 3600 / 60)
}

/// Muted line of a library card.
pub fn card_line(g: Option<&GameStats>, now: u64, tz: i64) -> String {
    match g.filter(|g| g.total.sessions > 0) {
        Some(g) => format!(
            "{} \u{b7} last played {}",
            fmt_dur(g.total.secs),
            rel_date(g.total.last, now, tz)
        ),
        None => "Not played yet".into(),
    }
}

/// "JP" / "US" / "EU" for a serial ("" when unknown).
pub fn tag(serial: &str) -> &'static str {
    match crate::disc::Region::of(serial) {
        crate::disc::Region::Japan => "JP",
        crate::disc::Region::Us => "US",
        crate::disc::Region::Europe => "EU",
        _ => "",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(start: u64, secs: u64, serial: &str) -> Session {
        Session {
            start,
            secs,
            serial: serial.into(),
            ok: true,
        }
    }

    #[test]
    fn durations() {
        assert_eq!(fmt_dur(0), "0 s");
        assert_eq!(fmt_dur(59), "59 s");
        assert_eq!(fmt_dur(60), "1 min");
        assert_eq!(fmt_dur(3599), "59 min");
        assert_eq!(fmt_dur(3600), "1 h");
        assert_eq!(fmt_dur(45_000), "12 h 30 min");
    }

    #[test]
    fn relative_dates() {
        let now = 1_700_000_000; // 2023-11-14 22:13:20 UTC
        assert_eq!(rel_date(now - 60, now, 0), "today");
        assert_eq!(rel_date(now + 5, now, 0), "today"); // clock skew
        assert_eq!(rel_date(now - 86_400, now, 0), "yesterday");
        assert_eq!(rel_date(now - 3 * 86_400, now, 0), "3 days ago");
        assert_eq!(rel_date(now - 29 * 86_400, now, 0), "29 days ago");
        assert_eq!(rel_date(now - 30 * 86_400, now, 0), "15 Oct 2023");
        // 2 h ago: the same UTC day, but the day before in a zone where it is already past midnight
        assert_eq!(rel_date(now - 2 * 3600, now, 0), "today");
        assert_eq!(rel_date(now - 2 * 3600, now, 3 * 3600), "yesterday");
        assert_eq!(civil(0), (1970, 1, 1));
        assert_eq!(civil(19_782), (2024, 2, 29));
        assert_eq!(clock(1_700_000_000, 3600), "23:13");
    }

    #[test]
    fn aggregation_and_short_rule() {
        let mut p = Playtime::default();
        p.record("G", s(1000, 600, "SLPS-02785"));
        p.record("G", s(5000, 1200, "SLUS-01158"));
        p.record("G", s(9000, 3, "SLUS-01158")); // quick quit
        let g = &p.games["G"];
        assert_eq!(
            (
                g.total.secs,
                g.total.sessions,
                g.total.short,
                g.total.longest
            ),
            (1800, 2, 1, 1200)
        );
        assert_eq!((g.total.first, g.total.last), (1000, 6200));
        assert_eq!(g.discs["SLPS-02785"].secs, 600);
        assert_eq!(
            (g.discs["SLUS-01158"].secs, g.discs["SLUS-01158"].short),
            (1200, 1)
        );
        assert_eq!(g.recent.len(), 3); // the short one is still listed
        let mut crash = s(20_000, 90, "SLPS-02785");
        crash.ok = false;
        p.record("G", crash);
        assert_eq!(p.games["G"].total.sessions, 3); // a crash still counts
        assert!(!p.games["G"].recent[3].ok);
    }

    #[test]
    fn recent_is_capped() {
        let mut p = Playtime::default();
        for i in 0..70 {
            p.record("G", s(1000 + i * 100, 60, "A"));
        }
        let g = &p.games["G"];
        assert_eq!(
            (g.recent.len(), g.recent[0].start, g.total.sessions),
            (KEEP, 1000 + 20 * 100, 70)
        );
    }

    #[test]
    fn card_text() {
        let mut p = Playtime::default();
        assert_eq!(card_line(p.games.get("G"), 0, 0), "Not played yet");
        p.record("G", s(1_700_000_000 - 86_400, 45_000, "A"));
        assert_eq!(
            card_line(p.games.get("G"), 1_700_000_000 + 45_000, 0),
            "12 h 30 min \u{b7} last played yesterday"
        );
    }

    #[test]
    fn io_atomic_corrupt_and_missing() {
        let dir = std::env::temp_dir().join(format!("prm_pt_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let f = dir.join("playtime.json");
        assert_eq!(load_from(&f), Playtime::default()); // old launcher: no file
        let mut p = Playtime::default();
        p.record("G", s(1000, 600, "A"));
        save_to(&f, &p).unwrap();
        assert!(!f.with_extension("json.tmp").exists());
        assert_eq!(load_from(&f), p);
        std::fs::write(&f, "{ not json").unwrap();
        assert_eq!(load_from(&f), Playtime::default());
        assert!(f.with_extension("json.corrupt").is_file() && !f.exists());
        std::fs::write(&f, "\u{feff}{\"games\":{\"G\":{\"secs\":5}}}").unwrap(); // BOM + missing fields
        assert_eq!(load_from(&f).games["G"].total.secs, 5);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
