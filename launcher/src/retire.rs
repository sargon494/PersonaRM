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

/// The real long path when the file exists (resolves 8.3 short names such as RUNNER~1), else as given.
fn canon(p: &str) -> String {
    std::fs::canonicalize(p).map_or_else(|_| p.to_string(), |c| c.to_string_lossy().trim_start_matches(r"\\?\").to_string())
}

/// From "<lnk>	<target>[	<icon>]" lines: the shortcuts whose target is `old`, each with whether its
/// icon also comes from `old` (compared by real path, so 8.3 short names and case don't matter).
pub fn matching_lnks(listing: &str, old: &str) -> Vec<(String, bool)> {
    let old = canon(old);
    let is_old = |p: &str| same_path(&canon(p), &old);
    listing
        .lines()
        .filter_map(|l| {
            let mut f = l.trim_end_matches('\r').split('\t');
            let (lnk, target, icon) = (f.next()?, f.next()?, f.next().unwrap_or(""));
            let icon_file = icon.rsplit_once(',').map_or(icon, |(p, _)| p);
            is_old(target).then(|| (lnk.to_string(), !icon_file.is_empty() && is_old(icon_file)))
        })
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
try { $l=$sh.CreateShortcut($_.FullName); $_.FullName + [char]9 + $l.TargetPath + [char]9 + $l.IconLocation } catch {} } } }";

const SET: &str = "$ErrorActionPreference='Stop'; $sh=New-Object -ComObject WScript.Shell; $n=$env:PRM_NEW; \
foreach($e in $env:PRM_LNKS.Split('|')){ $p,$f=$e.Split('*'); $l=$sh.CreateShortcut($p); $l.TargetPath=$n; \
if($f -eq '1'){ $i=$l.IconLocation; $k=$i.LastIndexOf(','); $l.IconLocation=$n+$(if($k -ge 0){$i.Substring($k)}else{',0'}) }; $l.Save() }";

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
    // '*' cannot appear in a Windows path, so it separates each shortcut from its move-the-icon flag.
    let list = hits.iter().map(|(p, icon)| format!("{p}*{}", u8::from(*icon))).collect::<Vec<_>>().join("|");
    ps(SET, &[("PRM_NEW", &canon(&new.to_string_lossy())), ("PRM_LNKS", &list)])?;
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
        let l = "C:\\d\\A.lnk\tC:\\Games\\PRM\\launcher.exe\tC:\\Games\\PRM\\launcher.exe,0\r\nC:\\d\\B.lnk\tC:\\Other\\launcher.exe\nC:\\d\\C.lnk\t\nC:\\d\\D.lnk\tc:\\games\\prm\\Launcher.exe\tC:\\icons\\x.ico,2\r\n";
        let want = vec![("C:\\d\\A.lnk".to_string(), true), ("C:\\d\\D.lnk".to_string(), false)];
        assert_eq!(matching_lnks(l, r"C:\Games\PRM\launcher.exe"), want);
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
        let n = canon(&new.to_string_lossy()).to_lowercase();
        for l in [&a, &b] {
            let r = read_lnk(l).to_lowercase();
            let f: Vec<&str> = r.split('|').collect();
            assert_eq!(f[0], n);
            assert_eq!(f[1], "--x");
            assert!(f[2].starts_with(&n), "icon: {}", f[2]);
            assert!(same_path(&canon(f[3]), &canon(&install.to_string_lossy())), "dir: {}", f[3]);
        }
        assert_eq!(read_lnk(&c), c_before);
        let _ = std::fs::remove_dir_all(&root);
    }
}
