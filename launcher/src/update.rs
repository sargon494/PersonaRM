//! Self-update from GitHub Releases. No UI code, so everything here is unit-testable.
//!
//! Security model: the only trusted input is the release JSON from the fixed API URL. The zip and
//! its `.sha256` are fetched only from github.com / githubusercontent.com HTTPS URLs found in that
//! JSON (every redirect hop is checked too), the zip is refused unless its SHA-256 matches, and
//! extraction refuses paths that escape the staging folder.

use sha2::{Digest, Sha256};
use std::io::{Cursor, Read, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

/// Version of this build: CI bakes the tag in through `PRM_VERSION`, otherwise Cargo's.
pub const VERSION: &str = match option_env!("PRM_VERSION") {
    Some(v) => v,
    None => env!("CARGO_PKG_VERSION"),
};

const API: &str = "https://api.github.com/repos/sargon494/PersonaRM/releases/latest";
const MAX_ZIP: u64 = 256 << 20;
/// Executable name since the rename. Old installs run `launcher.exe`, which the zip still ships (same
/// binary) so their updater keeps accepting it; see docs/LAUNCHER.md for when it can go.
pub const NEW_EXE: &str = if cfg!(windows) {
    "PersonaRM.exe"
} else {
    "PersonaRM"
};
const OLD_EXE: &str = "launcher.exe";
const STAGE: &str = ".prm-update";
const MANIFEST: &str = ".prm-update-old.txt";

#[derive(Clone, Debug, PartialEq)]
pub struct Release {
    pub tag: String,
    pub zip_url: String,
    pub sha_url: String,
}

/// "v1.2.3" / "1.2.3" -> (1,2,3). Anything else (pre-releases, junk) is None, i.e. never an update.
pub fn parse_version(s: &str) -> Option<(u64, u64, u64)> {
    let mut it = s.trim().strip_prefix('v').unwrap_or(s.trim()).split('.');
    let mut n = || it.next()?.parse::<u64>().ok();
    let v = (n()?, n()?, n()?);
    it.next().is_none().then_some(v)
}

pub fn is_newer(tag: &str, current: &str) -> bool {
    matches!((parse_version(tag), parse_version(current)), (Some(a), Some(b)) if a > b)
}

/// Debug builds only: `PRM_UPDATE_API` points the check at a local fake server (127.0.0.1).
fn local_override() -> Option<String> {
    if cfg!(debug_assertions) {
        std::env::var("PRM_UPDATE_API").ok()
    } else {
        None
    }
}

fn host_of(url: &str) -> Option<(&str, &str)> {
    let (scheme, rest) = url.split_once("://")?;
    let auth = rest.split(['/', '?', '#']).next()?;
    Some((scheme, auth))
}

fn local_ok(url: &str) -> bool {
    local_override().is_some()
        && host_of(url)
            .is_some_and(|(s, a)| s == "http" && a.starts_with("127.0.0.1:") && !a.contains('@'))
}

/// Download URLs must be https on github.com or GitHub's release CDN (no port, no userinfo).
pub fn asset_url_ok(url: &str) -> bool {
    let https = host_of(url).is_some_and(|(s, a)| {
        s == "https"
            && matches!(
                a,
                "github.com"
                    | "objects.githubusercontent.com"
                    | "release-assets.githubusercontent.com"
            )
    });
    https || local_ok(url)
}

fn api_url_ok(url: &str) -> bool {
    host_of(url).is_some_and(|(s, a)| s == "https" && a == "api.github.com") || local_ok(url)
}

/// GET with the redirects followed by hand so every hop passes `ok`.
fn fetch(
    url: &str,
    ok: fn(&str) -> bool,
    limit: u64,
    timeout: Duration,
) -> Result<Vec<u8>, String> {
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .timeout_global(Some(timeout))
        .max_redirects(0)
        .http_status_as_error(false)
        .user_agent("PersonaRM-launcher")
        .build()
        .into();
    let mut url = url.to_string();
    for _ in 0..5 {
        if !ok(&url) {
            return Err(format!("refused URL {url}"));
        }
        let mut resp = agent
            .get(&url)
            .header("Accept", "application/vnd.github+json")
            .call()
            .map_err(|e| e.to_string())?;
        let st = resp.status().as_u16();
        if (300..400).contains(&st) {
            url = resp
                .headers()
                .get("location")
                .and_then(|v| v.to_str().ok())
                .ok_or("redirect without location")?
                .to_string();
            continue;
        }
        if st != 200 {
            return Err(format!("HTTP {st}"));
        }
        return resp
            .body_mut()
            .with_config()
            .limit(limit)
            .read_to_vec()
            .map_err(|e| e.to_string());
    }
    Err("too many redirects".into())
}

pub fn parse_release(json: &[u8]) -> Option<Release> {
    let v: serde_json::Value = serde_json::from_slice(json).ok()?;
    let tag = v["tag_name"].as_str()?.to_string();
    let suffix = if cfg!(windows) {
        "windows-x64.zip"
    } else {
        "linux-x64.zip"
    };
    let url_of = |end: &str| {
        v["assets"].as_array()?.iter().find_map(|a| {
            let n = a["name"].as_str()?;
            n.ends_with(end)
                .then(|| a["browser_download_url"].as_str().map(String::from))
                .flatten()
        })
    };
    Some(Release {
        tag,
        zip_url: url_of(suffix)?,
        sha_url: url_of(&format!("{suffix}.sha256"))?,
    })
}

/// Newer release than this build, or None (up to date, offline, or any error: all silent).
pub fn check() -> Option<Release> {
    let url = local_override().unwrap_or_else(|| API.to_string());
    if !api_url_ok(&url) {
        return None;
    }
    let r = parse_release(&fetch(&url, api_url_ok, 1 << 20, Duration::from_secs(5)).ok()?)?;
    (is_newer(&r.tag, VERSION) && asset_url_ok(&r.zip_url) && asset_url_ok(&r.sha_url)).then_some(r)
}

/// `sha256sum`-style text ("<hex>  name") against the downloaded bytes.
pub fn verify_sha256(data: &[u8], sha_text: &str) -> bool {
    let want = sha_text
        .split_whitespace()
        .next()
        .unwrap_or("")
        .to_ascii_lowercase();
    let got: String = Sha256::digest(data)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    want.len() == 64 && want == got
}

/// Download, verify and install. On success the caller restarts the launcher.
pub fn download_and_apply(r: &Release) -> Result<(), String> {
    let zip = fetch(&r.zip_url, asset_url_ok, MAX_ZIP, Duration::from_secs(120))?;
    let sha = fetch(&r.sha_url, asset_url_ok, 4096, Duration::from_secs(20))?;
    if !verify_sha256(&zip, &String::from_utf8_lossy(&sha)) {
        return Err("checksum mismatch: update refused".into());
    }
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let dir = exe.parent().ok_or("no install folder")?;
    apply_zip(
        &zip,
        dir,
        exe.file_name().and_then(|n| n.to_str()).unwrap_or(NEW_EXE),
    )
}

/// Started as the old `launcher.exe` with `PersonaRM.exe` beside it: the exe to start instead.
pub fn shim_target(me: &Path) -> Option<PathBuf> {
    if !me.file_name()?.to_str()?.eq_ignore_ascii_case(OLD_EXE) {
        return None;
    }
    let new = me.with_file_name(NEW_EXE);
    new.is_file().then_some(new)
}

/// What to start to reopen the launcher: the new exe if it is installed, else this one.
pub fn relaunch_exe() -> Option<PathBuf> {
    let me = std::env::current_exe().ok()?;
    Some(me.with_file_name(NEW_EXE))
        .filter(|p| p.is_file())
        .or(Some(me))
}

fn walk(dir: &Path, base: &Path, out: &mut Vec<PathBuf>) -> std::io::Result<()> {
    for e in std::fs::read_dir(dir)? {
        let p = e?.path();
        if p.is_dir() {
            walk(&p, base, out)?;
        } else {
            out.push(p.strip_prefix(base).unwrap().to_path_buf());
        }
    }
    Ok(())
}

/// Extract `zip` next to the install and swap the files in. `launcher_exe` must be in the zip.
/// Only files present in the zip are touched (never launcher.json, settings, saves).
pub fn apply_zip(zip: &[u8], install: &Path, launcher_exe: &str) -> Result<(), String> {
    let stage = install.join(STAGE);
    let _ = std::fs::remove_dir_all(&stage);
    let r = extract_and_swap(zip, install, &stage, launcher_exe);
    let _ = std::fs::remove_dir_all(&stage);
    r
}

fn extract_and_swap(
    zip: &[u8],
    install: &Path,
    stage: &Path,
    launcher_exe: &str,
) -> Result<(), String> {
    let e = |x: &dyn std::fmt::Display| x.to_string();
    let mut ar = zip::ZipArchive::new(Cursor::new(zip)).map_err(|x| e(&x))?;
    for i in 0..ar.len() {
        let mut f = ar.by_index(i).map_err(|x| e(&x))?;
        let rel = f.enclosed_name().ok_or("unsafe path in update zip")?;
        let dest = stage.join(&rel);
        if f.is_dir() {
            std::fs::create_dir_all(&dest).map_err(|x| e(&x))?;
        } else {
            std::fs::create_dir_all(dest.parent().unwrap()).map_err(|x| e(&x))?;
            let mut out = std::fs::File::create(&dest).map_err(|x| e(&x))?;
            std::io::copy(&mut f.by_ref().take(MAX_ZIP), &mut out).map_err(|x| e(&x))?;
        }
    }
    let mut files = Vec::new();
    walk(stage, stage, &mut files).map_err(|x| e(&x))?;
    files.sort();
    if !files.iter().any(|f| f == Path::new(launcher_exe)) {
        return Err(format!("{launcher_exe} missing from the update"));
    }
    // Windows cannot overwrite a running exe but can rename it: old files become `<name>.old`,
    // listed in a manifest so the next start deletes exactly those and nothing else.
    let olds: Vec<PathBuf> = files
        .iter()
        .filter(|f| install.join(f).exists())
        .map(|f| with_old(f))
        .collect();
    let list: Vec<String> = olds
        .iter()
        .map(|p| p.to_string_lossy().into_owned())
        .collect();
    std::fs::write(install.join(MANIFEST), list.join("\n")).map_err(|x| e(&x))?;
    let mut done: Vec<&PathBuf> = Vec::new();
    let mut fail = None;
    for rel in &files {
        let dest = install.join(rel);
        let step = (|| -> std::io::Result<()> {
            std::fs::create_dir_all(dest.parent().unwrap())?;
            if dest.exists() {
                let old = install.join(with_old(rel));
                let _ = std::fs::remove_file(&old);
                std::fs::rename(&dest, &old)?;
            }
            std::fs::rename(stage.join(rel), &dest)
        })();
        match step {
            Ok(()) => done.push(rel),
            Err(x) => {
                // Put back the file whose rename succeeded before the second step failed.
                let old = install.join(with_old(rel));
                if !dest.exists() && old.exists() {
                    let _ = std::fs::rename(&old, &dest);
                }
                fail = Some(x.to_string());
                break;
            }
        }
    }
    if let Some(msg) = fail {
        for rel in done.into_iter().rev() {
            let dest = install.join(rel);
            let old = install.join(with_old(rel));
            let _ = std::fs::remove_file(&dest);
            if old.exists() {
                let _ = std::fs::rename(&old, &dest);
            }
        }
        let _ = std::fs::remove_file(install.join(MANIFEST));
        return Err(format!("could not install the update: {msg}"));
    }
    Ok(())
}

fn with_old(rel: &Path) -> PathBuf {
    let mut s = rel.as_os_str().to_os_string();
    s.push(".old");
    PathBuf::from(s)
}

/// On start: delete the `.old` files of the previous update (the old exe is only deletable now).
/// Returns true while some `.old` file is still locked (the old exe of a just-restarted launcher).
pub fn cleanup_old(install: &Path) -> bool {
    let _ = std::fs::remove_dir_all(install.join(STAGE));
    let m = install.join(MANIFEST);
    let Ok(text) = std::fs::read_to_string(&m) else {
        return false;
    };
    let mut left = false;
    for l in text.lines().filter(|l| !l.is_empty()) {
        let p = install.join(l);
        if p.exists() && std::fs::remove_file(&p).is_err() {
            left = true;
        }
    }
    if !left {
        let _ = std::fs::remove_file(m);
    }
    left
}

/// Used by tests and the debug fake server.
#[allow(dead_code)]
pub fn write_file(p: &Path, data: &[u8]) {
    std::fs::File::create(p).unwrap().write_all(data).unwrap();
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("prm-upd-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn make_zip(files: &[(&str, &[u8])]) -> Vec<u8> {
        let mut w = zip::ZipWriter::new(Cursor::new(Vec::new()));
        for (n, d) in files {
            w.start_file(*n, zip::write::SimpleFileOptions::default())
                .unwrap();
            w.write_all(d).unwrap();
        }
        w.finish().unwrap().into_inner()
    }

    #[test]
    fn versions() {
        assert_eq!(parse_version("v1.2.3"), Some((1, 2, 3)));
        assert_eq!(parse_version("0.10.0"), Some((0, 10, 0)));
        assert_eq!(parse_version("v1.2"), None);
        assert_eq!(parse_version("v1.2.3-rc1"), None);
        assert!(is_newer("v0.10.0", "0.9.9"));
        assert!(is_newer("v1.0.0", "0.99.99"));
        assert!(!is_newer("v0.1.0", "0.1.0"));
        assert!(!is_newer("v0.0.9", "0.1.0"));
        assert!(!is_newer("garbage", "0.1.0"));
    }

    #[test]
    fn checksum() {
        let sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  x.zip\n";
        assert!(verify_sha256(b"abc", sha));
        assert!(verify_sha256(b"abc", &sha.to_uppercase()));
        assert!(!verify_sha256(b"abd", sha));
        assert!(!verify_sha256(b"abc", ""));
        assert!(!verify_sha256(b"abc", "ba78"));
    }

    #[test]
    fn urls() {
        assert!(asset_url_ok(
            "https://github.com/sargon494/PersonaRM/releases/download/v1/a.zip"
        ));
        assert!(asset_url_ok("https://objects.githubusercontent.com/x"));
        assert!(!asset_url_ok("http://github.com/x"));
        assert!(!asset_url_ok("https://github.com.evil.com/x"));
        assert!(!asset_url_ok("https://github.com@evil.com/x"));
        assert!(!asset_url_ok("https://evil.com/github.com/x"));
        assert!(!asset_url_ok("https://github.com:8443/x"));
        assert!(!asset_url_ok("http://127.0.0.1:1/x") || local_override().is_some());
    }

    #[test]
    fn release_json() {
        let sfx = if cfg!(windows) {
            "windows-x64.zip"
        } else {
            "linux-x64.zip"
        };
        let j = format!(
            r#"{{"tag_name":"v1.2.3","assets":[{{"name":"a-{sfx}.sha256","browser_download_url":"https://github.com/s"}},{{"name":"a-{sfx}","browser_download_url":"https://github.com/z"}}]}}"#
        );
        let r = parse_release(j.as_bytes()).unwrap();
        assert_eq!(
            (r.zip_url.as_str(), r.sha_url.as_str()),
            ("https://github.com/z", "https://github.com/s")
        );
        assert!(parse_release(br#"{"tag_name":"v1","assets":[]}"#).is_none());
    }

    #[test]
    fn swap_keeps_user_files_and_cleans_old() {
        let d = tmp("swap");
        std::fs::create_dir_all(d.join("docs")).unwrap();
        write_file(&d.join("launcher.exe"), b"old-launcher");
        write_file(&d.join("SDL3.dll"), b"old-sdl");
        write_file(&d.join("launcher.json"), b"user-config");
        write_file(&d.join("notes.old"), b"user-file");
        let z = make_zip(&[
            ("launcher.exe", b"new-launcher"),
            ("SDL3.dll", b"new-sdl"),
            ("docs/LAUNCHER.md", b"doc"),
        ]);
        apply_zip(&z, &d, "launcher.exe").unwrap();
        assert_eq!(
            std::fs::read(d.join("launcher.exe")).unwrap(),
            b"new-launcher"
        );
        assert_eq!(
            std::fs::read(d.join("launcher.exe.old")).unwrap(),
            b"old-launcher"
        );
        assert_eq!(std::fs::read(d.join("docs/LAUNCHER.md")).unwrap(), b"doc");
        assert_eq!(
            std::fs::read(d.join("launcher.json")).unwrap(),
            b"user-config"
        );
        assert!(!d.join(STAGE).exists());
        cleanup_old(&d);
        assert!(!d.join("launcher.exe.old").exists() && !d.join(MANIFEST).exists());
        assert_eq!(std::fs::read(d.join("notes.old")).unwrap(), b"user-file");
        assert_eq!(
            std::fs::read(d.join("launcher.exe")).unwrap(),
            b"new-launcher"
        );
    }

    #[test]
    #[cfg(windows)] // the launcher.exe -> PersonaRM.exe transition only exists on Windows installs
    fn old_install_updates_and_shims_to_new_name() {
        // An old install runs launcher.exe: its updater demands "launcher.exe" in the zip.
        let d = tmp("rename");
        write_file(&d.join("launcher.exe"), b"v0.2.2");
        write_file(&d.join("launcher.json"), b"cfg");
        let z = make_zip(&[
            ("PersonaRM.exe", b"v0.3.0"),
            ("launcher.exe", b"v0.3.0"),
            ("SDL3.dll", b"s"),
        ]);
        apply_zip(&z, &d, "launcher.exe").unwrap();
        assert_eq!(std::fs::read(d.join("launcher.exe")).unwrap(), b"v0.3.0");
        assert_eq!(std::fs::read(d.join("PersonaRM.exe")).unwrap(), b"v0.3.0");
        assert_eq!(std::fs::read(d.join("launcher.json")).unwrap(), b"cfg");
        // The new binary started as launcher.exe hands over to PersonaRM.exe; under its own name it stays.
        assert_eq!(shim_target(&d.join("launcher.exe")), Some(d.join(NEW_EXE)));
        assert_eq!(shim_target(&d.join("Launcher.EXE")), Some(d.join(NEW_EXE)));
        assert_eq!(shim_target(&d.join(NEW_EXE)), None);
        // No PersonaRM.exe yet: keep running as launcher.exe.
        std::fs::remove_file(d.join("PersonaRM.exe")).unwrap();
        assert_eq!(shim_target(&d.join("launcher.exe")), None);
        // An install already on the new name updates with a zip that no longer ships launcher.exe.
        write_file(&d.join("PersonaRM.exe"), b"v0.3.0");
        apply_zip(
            &make_zip(&[("PersonaRM.exe", b"v0.4.0")]),
            &d,
            "PersonaRM.exe",
        )
        .unwrap();
        assert_eq!(std::fs::read(d.join("PersonaRM.exe")).unwrap(), b"v0.4.0");
        assert!(apply_zip(&make_zip(&[("launcher.exe", b"x")]), &d, "PersonaRM.exe").is_err());
    }

    #[test]
    fn refuses_bad_zips_and_rolls_back() {
        let d = tmp("bad");
        write_file(&d.join("launcher.exe"), b"old");
        write_file(&d.join("a.bin"), b"old-a");
        assert!(
            apply_zip(
                &make_zip(&[("../evil.txt", b"x"), ("launcher.exe", b"n")]),
                &d,
                "launcher.exe"
            )
            .is_err()
        );
        assert!(!d.parent().unwrap().join("evil.txt").exists());
        assert!(apply_zip(&make_zip(&[("a.bin", b"n")]), &d, "launcher.exe").is_err());
        assert!(apply_zip(b"not a zip", &d, "launcher.exe").is_err());
        assert_eq!(std::fs::read(d.join("a.bin")).unwrap(), b"old-a");
        // A failure mid-swap (a directory sits where a file must go) restores what was already swapped.
        write_file(&d.join("z.bin"), b"old-z");
        std::fs::create_dir_all(d.join("z.bin.old/sub")).unwrap();
        write_file(&d.join("z.bin.old/sub/x"), b"x");
        let z = make_zip(&[
            ("a.bin", b"new-a"),
            ("launcher.exe", b"new"),
            ("z.bin", b"z"),
        ]);
        assert!(apply_zip(&z, &d, "launcher.exe").is_err());
        assert_eq!(std::fs::read(d.join("a.bin")).unwrap(), b"old-a");
        assert_eq!(std::fs::read(d.join("launcher.exe")).unwrap(), b"old");
        assert_eq!(std::fs::read(d.join("z.bin")).unwrap(), b"old-z");
        assert!(!d.join("a.bin.old").exists() && !d.join(MANIFEST).exists());
    }
}
