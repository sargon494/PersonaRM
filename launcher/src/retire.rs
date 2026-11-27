//! Windows only: drop the leftover `launcher.exe` (the pre-rename name, still shipped in the zip so
//! installs from v0.2.2 and earlier can update) after pointing shortcuts that used it at PersonaRM.exe.
//! Shortcuts are read and written through WScript.Shell run by PowerShell (no COM crate needed).

use std::path::{Path, PathBuf};
use std::process::Command;

const OLD_EXE: &str = "launcher.exe";

/// Paths equal ignoring case, slash direction, a trailing slash and the `\\?\` prefix.
pub fn same_path(a: &str, b: &str) -> bool {
    let n = |s: &str| s.trim().trim_start_matches(r"\\?\").replace('/', "\\").trim_end_matches('\\').to_ascii_lowercase();
    let (a, b) = (n(a), n(b));
    !a.is_empty() && a == b
}

/// From "<lnk>\t<target>" lines: the shortcut files whose target is `old`.
pub fn matching_lnks(listing: &str, old: &str) -> Vec<String> {
    listing
        .lines()
        .filter_map(|l| l.split_once('\t'))
        .filter(|(_, t)| same_path(t, old))
        .map(|(p, _)| p.to_string())
        .collect()
}

fn ps(script: &str, env: &[(&str, &str)]) -> Option<String> {
    use std::os::windows::process::CommandExt;
    let mut c = Command::new("powershell");
    c.args(["-NoProfile", "-NonInteractive", "-Command", script]).creation_flags(0x0800_0000); // no console window
    for (k, v) in env {
        c.env(k, v);
    }
    let o = c.output().ok()?;
    o.status.success().then(|| String::from_utf8_lossy(&o.stdout).into_owned())
}

const LIST: &str = "[Console]::OutputEncoding=[Text.Encoding]::UTF8; $sh=New-Object -ComObject WScript.Shell; \
foreach($d in $env:PRM_DIRS.Split('|')){ if(Test-Path -LiteralPath $d){ \
Get-ChildItem -LiteralPath $d -Recurse -Filter *.lnk -File -ErrorAction SilentlyContinue | ForEach-Object { \
try { $l=$sh.CreateShortcut($_.FullName); $_.FullName + [char]9 + $l.TargetPath } catch {} } } }";

const SET: &str = "$ErrorActionPreference='Stop'; $sh=New-Object -ComObject WScript.Shell; $o=$env:PRM_OLD; $n=$env:PRM_NEW; \
foreach($p in $env:PRM_LNKS.Split('|')){ $l=$sh.CreateShortcut($p); $l.TargetPath=$n; $i=$l.IconLocation; \
if($i.StartsWith($o,[StringComparison]::OrdinalIgnoreCase)){ $l.IconLocation=$n+$i.Substring($o.Length) }; $l.Save() }";

/// Point every shortcut under `dirs` that targets `old` at `new` (arguments and working dir are kept;
/// the icon follows when it came from `old`). None if PowerShell failed or a shortcut could not be
/// saved; else how many were rewritten.
pub fn fix_shortcuts(old: &Path, new: &Path, dirs: &[PathBuf]) -> Option<usize> {
    let joined = dirs.iter().map(|d| d.to_string_lossy().into_owned()).collect::<Vec<_>>().join("|");
    let listing = ps(LIST, &[("PRM_DIRS", &joined)])?;
    let hits = matching_lnks(&listing, &old.to_string_lossy());
    if hits.is_empty() {
        return Some(0);
    }
    ps(SET, &[("PRM_OLD", &old.to_string_lossy()), ("PRM_NEW", &new.to_string_lossy()), ("PRM_LNKS", &hits.join("|"))])?;
    Some(hits.len())
}

/// Folders searched for shortcuts: user Desktop (plus OneDrive's), Public Desktop when writable, user Start Menu.
pub fn default_dirs() -> Vec<PathBuf> {
    let env = |k: &str| std::env::var_os(k).map(PathBuf::from);
    let mut v = Vec::new();
    for (var, sub) in [("USERPROFILE", "Desktop"), ("OneDrive", "Desktop")] {
        v.extend(env(var).map(|p| p.join(sub)));
    }
    if let Some(p) = env("PUBLIC").map(|p| p.join("Desktop")) {
        let probe = p.join(format!(".prm-probe-{}", std::process::id()));
        if std::fs::write(&probe, b"").is_ok() {
            let _ = std::fs::remove_file(probe);
            v.push(p);
        }
    }
    v.extend(env("APPDATA").map(|p| p.join(r"Microsoft\Windows\Start Menu\Programs")));
    v
}

/// If `install` holds both exes and `me` is PersonaRM.exe: fix shortcuts, then delete launcher.exe.
/// Any failure (PowerShell, locked file) leaves things as they are; the next start retries.
pub fn retire_launcher(install: &Path, me: &Path, dirs: &[PathBuf]) {
    let (old, new) = (install.join(OLD_EXE), install.join(crate::update::NEW_EXE));
    let running_new = me.file_name().is_some_and(|n| n.eq_ignore_ascii_case(crate::update::NEW_EXE));
    if !running_new || !old.is_file() || !new.is_file() {
        return;
    }
    if fix_shortcuts(&old, &new, dirs).is_some() {
        let _ = std::fs::remove_file(&old);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn path_matching() {
        assert!(same_path(r"C:\Games\PRM\launcher.exe", r"c:/games/prm/LAUNCHER.EXE"));
        assert!(same_path(r"\\?\C:\a\launcher.exe", r"C:\a\launcher.exe"));
        assert!(!same_path(r"C:\Other\launcher.exe", r"C:\Games\PRM\launcher.exe"));
        assert!(!same_path(r"C:\Games\PRM\launcher.exe.old", r"C:\Games\PRM\launcher.exe"));
        assert!(!same_path(r"C:\Games\PRM\PersonaRM.exe", r"C:\Games\PRM\launcher.exe"));
        assert!(!same_path("", ""));
    }

    #[test]
    fn listing_filter() {
        let l = "C:\\d\\A.lnk\tC:\\Games\\PRM\\launcher.exe\r\nC:\\d\\B.lnk\tC:\\Other\\launcher.exe\nC:\\d\\C.lnk\t\nC:\\d\\D.lnk\tc:\\games\\prm\\Launcher.exe\r\n";
        assert_eq!(matching_lnks(l, r"C:\Games\PRM\launcher.exe"), vec!["C:\\d\\A.lnk", "C:\\d\\D.lnk"]);
    }

    fn mk_lnk(lnk: &Path, target: &Path, icon: &Path) {
        let s = "$l=(New-Object -ComObject WScript.Shell).CreateShortcut($env:L); $l.TargetPath=$env:T; $l.Arguments='--x'; \
$l.WorkingDirectory=Split-Path $env:T; $l.IconLocation=$env:I+',0'; $l.Save()";
        ps(s, &[("L", &lnk.to_string_lossy()), ("T", &target.to_string_lossy()), ("I", &icon.to_string_lossy())]).unwrap();
    }

    fn read_lnk(lnk: &Path) -> String {
        let s = "$l=(New-Object -ComObject WScript.Shell).CreateShortcut($env:L); $l.TargetPath + '|' + $l.Arguments + '|' + $l.IconLocation + '|' + $l.WorkingDirectory";
        ps(s, &[("L", &lnk.to_string_lossy())]).unwrap().trim().to_string()
    }

    #[test]
    fn rewrites_shortcuts_and_deletes_old_exe() {
        let root = std::env::temp_dir().join(format!("prm-retire-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let (install, desk, start) = (root.join("install"), root.join("desk"), root.join("start/sub"));
        for d in [&install, &desk, &start] {
            std::fs::create_dir_all(d).unwrap();
        }
        let exe = std::env::current_exe().unwrap(); // any real exe works as the copy
        let (old, new) = (install.join("launcher.exe"), install.join("PersonaRM.exe"));
        std::fs::copy(&exe, &old).unwrap();
        std::fs::copy(&exe, &new).unwrap();
        let other = root.join("other.exe");
        std::fs::copy(&exe, &other).unwrap();
        let (a, b, c) = (desk.join("a.lnk"), start.join("b.lnk"), desk.join("c.lnk"));
        mk_lnk(&a, &old, &old);
        mk_lnk(&b, &old, &old);
        mk_lnk(&c, &other, &other);
        let c_before = read_lnk(&c);
        let dirs = [desk.clone(), root.join("start"), root.join("missing")];

        // Running as launcher.exe: must do nothing.
        retire_launcher(&install, &old, &dirs);
        assert!(old.exists());

        retire_launcher(&install, &new, &dirs);
        assert!(!old.exists() && new.exists());
        let n = new.to_string_lossy().to_lowercase();
        for l in [&a, &b] {
            let r = read_lnk(l).to_lowercase();
            let f: Vec<&str> = r.split('|').collect();
            assert_eq!(f[0], n);
            assert_eq!(f[1], "--x");
            assert!(f[2].starts_with(&n), "icon: {}", f[2]);
            assert_eq!(f[3], install.to_string_lossy().to_lowercase());
        }
        assert_eq!(read_lnk(&c), c_before);
        let _ = std::fs::remove_dir_all(&root);
    }
}
