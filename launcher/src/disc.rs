//! Disc identification: reads the data track of a .cue / .bin / .iso (ISO 9660 primary volume descriptor,
//! SYSTEM.CNF, BOOT line) to get the serial, checks the dump is complete and finds discs on disk.
//! Only a handful of sectors are read; nothing hashes a whole disc. No UI code, all unit-testable.

use crate::model::Game;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Disc {
    /// "SLUS-01158".
    pub serial: String,
    /// Set when the serial could be read but the dump is not whole (truncated, odd size...).
    pub problem: Option<String>,
}

/// Cached answer of `identify`, valid while `stamp` (size + mtime of the cue and every file it names) matches.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Cached {
    pub stamp: String,
    pub result: Result<Disc, String>,
}

pub type Cache = BTreeMap<String, Cached>;

/// What the launcher knows about a serial (Persona 2 only; other games just get their region from the prefix).
pub const PROMO_SERIALS: [&str; 1] = ["SLPS-02786"];

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Region {
    Japan,
    Us,
    Europe,
    Other,
}

impl Region {
    pub fn of(serial: &str) -> Self {
        match serial.get(..4) {
            Some("SLPS" | "SLPM" | "SCPS" | "SCPM" | "SIPS") => Region::Japan,
            Some("SLUS" | "SCUS") => Region::Us,
            Some("SLES" | "SCES" | "SCED") => Region::Europe,
            _ => Region::Other,
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            Region::Japan => "Japan",
            Region::Us => "US",
            Region::Europe => "Europe",
            Region::Other => "Unknown region",
        }
    }
    /// Launcher language slot this region belongs to.
    pub fn slot(self) -> Option<&'static str> {
        match self {
            Region::Japan => Some("ja"),
            Region::Us | Region::Europe => Some("en"),
            Region::Other => None,
        }
    }
}

// ── cue ──

struct Track {
    file: usize,
    audio: bool,
    /// INDEX 01 within its file, in sectors.
    start: u64,
}

struct Cue {
    files: Vec<PathBuf>,
    tracks: Vec<Track>,
}

fn parse_cue(path: &Path) -> Result<Cue, String> {
    let text = std::fs::read(path).map_err(|e| format!("Cannot read the .cue ({e})"))?;
    let text = String::from_utf8_lossy(&text);
    let dir = path.parent().unwrap_or(Path::new("."));
    let mut cue = Cue {
        files: vec![],
        tracks: vec![],
    };
    for line in text.trim_start_matches('\u{feff}').lines() {
        let l = line.trim();
        let up = l.to_ascii_uppercase();
        if up.starts_with("FILE ") {
            let rest = l[5..].trim();
            let name = match rest.strip_prefix('"') {
                Some(r) => r.split('"').next().unwrap_or(""),
                None => rest.split_whitespace().next().unwrap_or(""),
            };
            cue.files.push(dir.join(name));
        } else if up.starts_with("TRACK ") && !cue.files.is_empty() {
            cue.tracks.push(Track {
                file: cue.files.len() - 1,
                audio: up.contains("AUDIO"),
                start: 0,
            });
        } else if up.starts_with("INDEX 01") {
            let p: Vec<u64> = l[8..]
                .trim()
                .split(':')
                .filter_map(|n| n.parse().ok())
                .collect();
            if let (Some(t), [m, s, f]) = (cue.tracks.last_mut(), p.as_slice()) {
                t.start = (m * 60 + s) * 75 + f;
            }
        }
    }
    if cue.files.is_empty() {
        return Err("Not a .cue sheet (no FILE line)".into());
    }
    Ok(cue)
}

fn is_cue(p: &Path) -> bool {
    p.extension().is_some_and(|e| e.eq_ignore_ascii_case("cue"))
}

// ── ISO 9660 ──

/// (sector size, user-data offset inside a sector): plain ISO, raw Mode 2 Form 1, raw Mode 1 - same order as the game.
const LAYOUTS: [(u64, u64); 3] = [(2048, 0), (2352, 24), (2352, 16)];

fn read_sector(
    f: &mut std::fs::File,
    start: u64,
    (ss, off): (u64, u64),
    lba: u64,
) -> std::io::Result<[u8; 2048]> {
    let mut b = [0u8; 2048];
    f.seek(SeekFrom::Start((start + lba) * ss + off))?;
    f.read_exact(&mut b)?;
    Ok(b)
}

fn u32le(b: &[u8], at: usize) -> u64 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]]) as u64
}

/// Serial from the text of SYSTEM.CNF: `BOOT = cdrom:\SLUS_011.58;1` -> `SLUS-01158`.
fn serial_from_cnf(cnf: &str) -> Result<String, String> {
    let line = cnf
        .lines()
        .map(str::trim)
        .find(|l| {
            l.to_ascii_uppercase().starts_with("BOOT")
                && !l.to_ascii_uppercase().starts_with("BOOTSTRAP")
        })
        .ok_or("Not a PlayStation disc (no BOOT line)")?;
    let path = line.split_once('=').map_or("", |x| x.1).trim();
    let name = path
        .rsplit(['\\', '/', ':'])
        .next()
        .unwrap_or("")
        .split(';')
        .next()
        .unwrap_or("");
    let s = name.to_ascii_uppercase().replace('_', "-").replace('.', "");
    let ok = s.len() == 10
        && s.as_bytes()[4] == b'-'
        && s[..4].bytes().all(|c| c.is_ascii_uppercase())
        && s[5..].bytes().all(|c| c.is_ascii_digit());
    if ok {
        Ok(s)
    } else {
        Err(format!("Not a PlayStation disc (boot file \"{name}\")"))
    }
}

/// Reads the serial of the ISO 9660 volume starting at sector `start` of `f`; also the volume size in sectors.
fn read_serial(f: &mut std::fs::File, start: u64) -> Result<(String, u64), String> {
    let layout = LAYOUTS
        .into_iter()
        .find(|&l| read_sector(f, start, l, 16).is_ok_and(|b| &b[1..6] == b"CD001"))
        .ok_or("Not a disc image (no ISO 9660 volume)")?;
    let pvd = read_sector(f, start, layout, 16).map_err(|e| e.to_string())?;
    let vol = u32le(&pvd, 80);
    let (root, rsize) = (u32le(&pvd, 156 + 2), u32le(&pvd, 156 + 10));
    let trunc = |_| "Disc image incomplete (file is cut short)".to_string();
    let mut cnf = None;
    'dir: for s in 0..rsize.div_ceil(2048).min(64) {
        let sec = read_sector(f, start, layout, root + s).map_err(trunc)?;
        let mut p = 0;
        while p + 34 <= 2048 && sec[p] != 0 {
            let (len, nlen) = (sec[p] as usize, sec[p + 32] as usize);
            if p + 33 + nlen <= 2048
                && sec[p + 33..p + 33 + nlen]
                    .to_ascii_uppercase()
                    .starts_with(b"SYSTEM.CNF")
            {
                cnf = Some((u32le(&sec, p + 2), u32le(&sec, p + 10)));
                break 'dir;
            }
            p += len;
        }
    }
    let (lba, size) = cnf.ok_or("Not a PlayStation disc (no SYSTEM.CNF)")?;
    let mut text = Vec::new();
    for s in 0..size.div_ceil(2048).min(2) {
        text.extend(read_sector(f, start, layout, lba + s).map_err(trunc)?);
    }
    text.truncate(size.min(4096) as usize);
    Ok((serial_from_cnf(&String::from_utf8_lossy(&text))?, vol))
}

/// Sector size of the layout that works for `f` (for the whole-sectors check).
fn sector_size(f: &mut std::fs::File, start: u64) -> u64 {
    LAYOUTS
        .into_iter()
        .find(|&l| read_sector(f, start, l, 16).is_ok_and(|b| &b[1..6] == b"CD001"))
        .map_or(2352, |l| l.0)
}

/// Identify the disc behind a .cue, .bin or .iso. `Err` = not usable at all (missing file, not a PS1 disc).
pub fn identify(path: &Path) -> Result<Disc, String> {
    let name = |p: &Path| {
        p.file_name()
            .map_or(String::new(), |n| n.to_string_lossy().into_owned())
    };
    // (file with the data track, its start sector, every file that must exist)
    let (data, start, all) = if is_cue(path) {
        let cue = parse_cue(path)?;
        if let Some(m) = cue.files.iter().find(|f| !f.is_file()) {
            return Err(format!("Disc image incomplete (missing {})", name(m)));
        }
        let t = cue
            .tracks
            .iter()
            .find(|t| !t.audio)
            .ok_or("No data track in the .cue")?;
        (
            cue.files[t.file].clone(),
            t.start,
            (cue.files.clone(), cue.tracks),
        )
    } else {
        if !path.is_file() {
            return Err("File missing".into());
        }
        (path.to_path_buf(), 0, (vec![path.to_path_buf()], vec![]))
    };
    let mut f =
        std::fs::File::open(&data).map_err(|e| format!("Cannot open {} ({e})", name(&data)))?;
    let ss = sector_size(&mut f, start);
    let mut problem = None;
    for (i, file) in all.0.iter().enumerate() {
        let len = std::fs::metadata(file).map_or(0, |m| m.len());
        let sectors = len / ss;
        if len % ss != 0 && problem.is_none() {
            problem = Some(format!(
                "{} is not a whole number of sectors (cut short?)",
                name(file)
            ));
        }
        if let Some(t) = all
            .1
            .iter()
            .find(|t| t.file == i && t.start >= sectors && sectors > 0)
        {
            problem.get_or_insert(format!(
                "{} ends before track starts at sector {}",
                name(file),
                t.start
            ));
        }
    }
    let serial = read_serial(&mut f, start);
    let (serial, vol) = match serial {
        Ok(x) => x,
        Err(e) => return Err(problem.map_or(e, |p| format!("Disc image incomplete ({p})"))),
    };
    let avail = (f.metadata().map_or(0, |m| m.len()) / ss).saturating_sub(start);
    if vol > avail && problem.is_none() {
        problem = Some(format!("{} has {avail} of {vol} sectors", name(&data)));
    }
    Ok(Disc { serial, problem })
}

fn stamp(path: &Path) -> String {
    let one = |p: &Path| {
        std::fs::metadata(p).map_or("-".to_string(), |m| {
            let t = m
                .modified()
                .ok()
                .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                .map_or(0, |d| d.as_secs());
            format!("{}:{t}", m.len())
        })
    };
    let mut s = one(path);
    if is_cue(path) {
        for f in parse_cue(path).map(|c| c.files).unwrap_or_default() {
            s += &format!("|{}", one(&f));
        }
    }
    s
}

/// `identify` with the by-path cache: unchanged files (size and mtime) are not read again.
pub fn identify_cached(cache: &mut Cache, path: &Path) -> Result<Disc, String> {
    if !path.is_file() {
        return identify(path); // typed-in paths that do not exist must not fill the cache
    }
    let key = path.to_string_lossy().into_owned();
    let st = stamp(path);
    if let Some(c) = cache.get(&key).filter(|c| c.stamp == st) {
        return c.result.clone();
    }
    let result = identify(path);
    cache.insert(
        key,
        Cached {
            stamp: st,
            result: result.clone(),
        },
    );
    result
}

// ── what the launcher says about a disc ──

/// Slot this disc belongs to for `game`: "ja", "en", or None (promo, other region, other game).
pub fn slot_of(game: &Game, d: &Disc) -> Option<&'static str> {
    (game.serials.contains(&d.serial) && !PROMO_SERIALS.contains(&d.serial.as_str()))
        .then(|| Region::of(&d.serial).slot())
        .flatten()
}

/// (ok, text) shown next to a disc path of language slot `slot`.
pub fn verdict(game: &Game, slot: &str, r: &Result<Disc, String>) -> (bool, String) {
    let d = match r {
        Err(e) => return (false, e.clone()),
        Ok(d) => d,
    };
    if let Some(p) = &d.problem {
        return (false, format!("Disc image incomplete ({p})"));
    }
    let short = game.name.split(':').next().unwrap_or(&game.name);
    if !game.serials.contains(&d.serial) {
        return (false, format!("Not {short} (found {})", d.serial));
    }
    if PROMO_SERIALS.contains(&d.serial.as_str()) {
        return (
            false,
            format!("This is the Disc 2 promo ({}), not the game disc", d.serial),
        );
    }
    let region = Region::of(&d.serial);
    if region.slot().is_some_and(|s| s != slot) {
        let field = if slot == "ja" { "English" } else { "Japanese" };
        return (
            false,
            format!("{} release: use the {field} disc field", region.label()),
        );
    }
    (true, format!("{} release ({})", region.label(), d.serial))
}

// ── scan ──

#[derive(Clone, Debug, PartialEq)]
pub struct Found {
    pub path: PathBuf,
    pub game: usize,
    pub disc: Disc,
}

fn skip_dir(name: &str) -> bool {
    name.starts_with('.')
        || name.starts_with('$')
        || [
            "node_modules",
            "target",
            "AppData",
            "Windows",
            "Program Files",
            "Program Files (x86)",
        ]
        .contains(&name)
}

/// Downloads, Documents and Desktop of the user (and of their OneDrive), then `extra` folders.
pub fn default_roots(extra: &[String]) -> Vec<PathBuf> {
    let home = std::env::var_os(if cfg!(windows) { "USERPROFILE" } else { "HOME" })
        .map(PathBuf::from)
        .unwrap_or_default();
    let mut v: Vec<PathBuf> = vec![];
    for base in [home.clone(), home.join("OneDrive")] {
        v.extend(["Downloads", "Documents", "Desktop"].map(|d| base.join(d)));
    }
    v.extend(extra.iter().map(PathBuf::from));
    let mut seen = vec![];
    v.retain(|p| {
        p.is_dir() && !seen.contains(p) && {
            seen.push(p.clone());
            true
        }
    });
    v
}

/// .cue / .iso files under `roots`, `depth` folders deep, giving up after `limit` time.
pub fn find_images(roots: &[PathBuf], depth: u32, deadline: std::time::Instant) -> Vec<PathBuf> {
    let mut out = vec![];
    let mut todo: Vec<(PathBuf, u32)> = roots.iter().map(|r| (r.clone(), 0)).collect();
    while let Some((dir, d)) = todo.pop() {
        if std::time::Instant::now() > deadline {
            break;
        }
        let Ok(rd) = std::fs::read_dir(&dir) else {
            continue;
        };
        for e in rd.flatten() {
            let (p, name) = (e.path(), e.file_name().to_string_lossy().into_owned());
            let Ok(t) = e.file_type() else { continue };
            if t.is_dir() {
                if d < depth && !skip_dir(&name) {
                    todo.push((p, d + 1));
                }
            } else if p
                .extension()
                .is_some_and(|x| x.eq_ignore_ascii_case("cue") || x.eq_ignore_ascii_case("iso"))
            {
                out.push(p);
            }
        }
    }
    out.sort();
    out.dedup();
    out
}

/// Identify every image under the default folders plus `extra`; keep the ones of a known game
/// (the promo disc too, so it can be listed). New cache entries are written into `cache`.
pub fn scan(games: &[Game], extra: &[String], cache: &mut Cache) -> Vec<Found> {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
    let mut found = vec![];
    for path in find_images(&default_roots(extra), 4, deadline) {
        if let Ok(disc) = identify_cached(cache, &path) {
            if let Some(game) = games.iter().position(|g| g.serials.contains(&disc.serial)) {
                found.push(Found { path, game, disc });
            }
        }
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    const RAW: usize = 2352;

    /// Plain ISO of `sectors` sectors: PVD (16), root dir (18) with SYSTEM.CNF (19).
    fn make_iso(boot: &str, sectors: usize) -> Vec<u8> {
        let mut iso = vec![0u8; sectors * 2048];
        let pvd = &mut iso[16 * 2048..17 * 2048];
        pvd[0] = 1;
        pvd[1..6].copy_from_slice(b"CD001");
        pvd[80..84].copy_from_slice(&(sectors as u32).to_le_bytes());
        pvd[158..162].copy_from_slice(&18u32.to_le_bytes()); // root record: extent
        pvd[166..170].copy_from_slice(&2048u32.to_le_bytes()); // root record: size
        let cnf = format!("BOOT = {boot}\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n");
        let root = &mut iso[18 * 2048..19 * 2048];
        root[0] = 34;
        root[2..6].copy_from_slice(&19u32.to_le_bytes());
        root[10..14].copy_from_slice(&(cnf.len() as u32).to_le_bytes());
        root[32] = 12;
        root[33..45].copy_from_slice(b"SYSTEM.CNF;1");
        iso[19 * 2048..19 * 2048 + cnf.len()].copy_from_slice(cnf.as_bytes());
        iso
    }

    /// The same ISO as Mode 2 Form 1 raw sectors (sync, header, subheader, data, padding).
    fn to_raw(iso: &[u8]) -> Vec<u8> {
        let mut raw = vec![];
        for s in iso.chunks(2048) {
            let mut sec = vec![0u8; RAW];
            sec[1..11].fill(0xff);
            sec[15] = 2;
            sec[24..24 + 2048].copy_from_slice(s);
            raw.extend(sec);
        }
        raw
    }

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("prm-disc-test-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn cue(dir: &Path, bin: &[u8]) -> PathBuf {
        std::fs::write(dir.join("d.bin"), bin).unwrap();
        let c = dir.join("d.cue");
        std::fs::write(
            &c,
            "FILE \"d.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n",
        )
        .unwrap();
        c
    }

    #[test]
    fn serial_formats() {
        for (boot, want) in [
            ("cdrom:\\SLUS_011.58;1", "SLUS-01158"),
            ("cdrom:SLPS_027.85;1", "SLPS-02785"),
            ("cdrom0:\\slps_027.86", "SLPS-02786"),
        ] {
            assert_eq!(
                serial_from_cnf(&format!("BOOT = {boot}\r\n")).as_deref(),
                Ok(want),
                "{boot}"
            );
        }
        assert!(serial_from_cnf("TCB = 4").is_err());
        assert!(serial_from_cnf("BOOT = cdrom:\\GAME.EXE;1").is_err());
    }

    #[test]
    fn iso_and_raw_bin_and_cue() {
        let dir = tmp("ok");
        let iso = make_iso("cdrom:\\SLUS_011.58;1", 24);
        std::fs::write(dir.join("a.iso"), &iso).unwrap();
        let want = Disc {
            serial: "SLUS-01158".into(),
            problem: None,
        };
        assert_eq!(identify(&dir.join("a.iso")), Ok(want.clone()));
        let c = cue(&dir, &to_raw(&iso));
        assert_eq!(identify(&c), Ok(want.clone()));
        assert_eq!(identify(&dir.join("d.bin")), Ok(want));
    }

    #[test]
    fn incomplete_dumps() {
        let dir = tmp("bad");
        let raw = to_raw(&make_iso("cdrom:\\SLPS_027.85;1", 24));
        // missing .bin
        let c = cue(&dir, &raw);
        std::fs::remove_file(dir.join("d.bin")).unwrap();
        assert_eq!(
            identify(&c),
            Err("Disc image incomplete (missing d.bin)".into())
        );
        // tail cut at a sector boundary: still readable, but flagged
        cue(&dir, &raw[..22 * RAW]);
        let d = identify(&c).unwrap();
        assert_eq!(d.serial, "SLPS-02785");
        assert!(d.problem.unwrap().contains("22 of 24"));
        // cut in the middle of a sector
        cue(&dir, &raw[..22 * RAW + 100]);
        assert!(identify(&c).unwrap().problem.is_some());
        // cut before SYSTEM.CNF: serial unreadable, reported as incomplete
        cue(&dir, &raw[..19 * RAW]);
        assert!(
            identify(&c)
                .unwrap_err()
                .starts_with("Disc image incomplete")
        );
        // not a disc at all
        cue(&dir, &vec![7u8; 40 * RAW]);
        assert!(identify(&c).unwrap_err().starts_with("Not a disc image"));
    }

    #[test]
    fn cache_reuses_until_file_changes() {
        let dir = tmp("cache");
        let c = cue(&dir, &to_raw(&make_iso("cdrom:\\SLUS_011.58;1", 24)));
        let mut cache = Cache::new();
        let a = identify_cached(&mut cache, &c).unwrap();
        // poison the entry: an unchanged file must return it without reading
        cache.values_mut().for_each(|e| {
            e.result = Ok(Disc {
                serial: "SLUS-99999".into(),
                problem: None,
            })
        });
        assert_eq!(
            identify_cached(&mut cache, &c).unwrap().serial,
            "SLUS-99999"
        );
        // changed size: re-read
        std::fs::write(
            dir.join("d.bin"),
            to_raw(&make_iso("cdrom:\\SLUS_011.58;1", 25)),
        )
        .unwrap();
        assert_eq!(identify_cached(&mut cache, &c).unwrap().serial, a.serial);
    }

    #[test]
    fn verdicts_and_slots() {
        let g = crate::model::LauncherConfig::default().games.remove(0);
        let d = |s: &str| {
            Ok(Disc {
                serial: s.into(),
                problem: None,
            })
        };
        assert_eq!(
            verdict(&g, "en", &d("SLUS-01158")),
            (true, "US release (SLUS-01158)".into())
        );
        assert_eq!(
            verdict(&g, "ja", &d("SLPS-02785")),
            (true, "Japan release (SLPS-02785)".into())
        );
        assert_eq!(
            verdict(&g, "ja", &d("SLUS-00123")),
            (false, "Not Persona 2 (found SLUS-00123)".into())
        );
        assert!(verdict(&g, "ja", &d("SLPS-02786")).1.contains("promo"));
        assert!(
            verdict(&g, "en", &d("SLPS-02785"))
                .1
                .contains("use the Japanese")
        ); // JP disc in the English slot
        let bad = Ok(Disc {
            serial: "SLUS-01158".into(),
            problem: Some("x".into()),
        });
        assert!(!verdict(&g, "en", &bad).0);
        assert_eq!(
            slot_of(
                &g,
                &Disc {
                    serial: "SLPS-02786".into(),
                    problem: None
                }
            ),
            None
        );
        assert_eq!(
            slot_of(
                &g,
                &Disc {
                    serial: "SLPS-02785".into(),
                    problem: None
                }
            ),
            Some("ja")
        );
    }

    #[test]
    fn scan_finds_known_games_only() {
        let dir = tmp("scan");
        std::fs::create_dir_all(dir.join("a/b")).unwrap();
        std::fs::write(
            dir.join("a/b/p2.iso"),
            make_iso("cdrom:\\SLUS_011.58;1", 24),
        )
        .unwrap();
        std::fs::write(dir.join("other.iso"), make_iso("cdrom:\\SLUS_000.01;1", 24)).unwrap();
        let imgs = find_images(
            &[dir.clone()],
            4,
            std::time::Instant::now() + std::time::Duration::from_secs(5),
        );
        assert_eq!(imgs.len(), 2);
        assert!(
            find_images(
                &[dir.clone()],
                1,
                std::time::Instant::now() + std::time::Duration::from_secs(5)
            )
            .len()
                == 1
        );
        let games = crate::model::LauncherConfig::default().games;
        let mut cache = Cache::new();
        let found: Vec<Found> = imgs
            .into_iter()
            .filter_map(|p| identify_cached(&mut cache, &p).ok().map(|d| (p, d)))
            .filter(|(_, d)| games[0].serials.contains(&d.serial))
            .map(|(path, disc)| Found {
                path,
                game: 0,
                disc,
            })
            .collect();
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].disc.serial, "SLUS-01158");
    }
}
