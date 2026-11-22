# Fails unless a release folder (or zip) holds exactly the expected program files: never recompiled
# game code (modules.dll/.so, generated .c, manifest.json from a native cache), game data or BIOS.
# Used by package-release.ps1 on the staged folder and by release.yml on the final zip.
param([string]$Dir, [string]$Zip)
$ErrorActionPreference = 'Stop'
$expected = @(
    'PersonaRM.exe', 'launcher.exe', 'personarm_sdl.exe', 'SDL3.dll', 'personarm_native.exe', 'libtcc.dll',
    'README.md', 'LICENSE', 'docs/LAUNCHER.md', 'docs/SETTINGS.md', 'docs/NATIVE_CODE.md',
    'licenses/TinyCC-LICENSE.txt', 'licenses/TinyCC-SOURCE.txt') | Sort-Object
if ($Zip) {
    $files = @(tar -t -f $Zip | Where-Object { $_ -and -not $_.EndsWith('/') } | ForEach-Object { $_ -replace '^\./', '' })
    if ($LASTEXITCODE) { throw "cannot list $Zip" }
} else {
    $root = (Resolve-Path $Dir).Path
    $files = @(Get-ChildItem $root -Recurse -File | ForEach-Object { $_.FullName.Substring($root.Length + 1) -replace '\\', '/' })
}
$files = $files | Sort-Object
# Case-sensitive: Windows file names are not, so a personarm.exe once silently replaced PersonaRM.exe.
$extra = @($files | Where-Object { $expected -cnotcontains $_ })
$missing = @($expected | Where-Object { $files -cnotcontains $_ })
if ($extra -or $missing) {
    throw "release files differ from the allowed list (no generated game code may ship). Unexpected: $($extra -join ', '). Missing: $($missing -join ', ')"
}
# PersonaRM.exe and launcher.exe must be the same launcher binary (launcher.exe is the pre-rename name).
if ($Dir) {
    $a = (Get-FileHash (Join-Path $root 'PersonaRM.exe')).Hash; $b = (Get-FileHash (Join-Path $root 'launcher.exe')).Hash
    if ($a -ne $b) { throw "PersonaRM.exe and launcher.exe differ: PersonaRM.exe is not the launcher" }
}
Write-Host "release files OK ($($files.Count) files, no generated game code)"
