# Builds the release zip + .sha256 from already-built binaries. Used by .github/workflows/release.yml
# and runnable locally. Only program files and docs go in: no game data, BIOS or recompiled code.
param(
    [Parameter(Mandatory)][string]$Tag,        # e.g. v0.2.0
    [Parameter(Mandatory)][string]$BinDir,     # folder with personarm.exe, personarm_sdl.exe, SDL3.dll
    [Parameter(Mandatory)][string]$Launcher,   # launcher.exe
    [string]$Out = "dist"
)
$ErrorActionPreference = 'Stop'
$stage = Join-Path $Out "stage"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage/docs" | Out-Null
Copy-Item $Launcher "$stage/launcher.exe"
foreach ($f in 'personarm.exe', 'personarm_sdl.exe', 'SDL3.dll') { Copy-Item (Join-Path $BinDir $f) $stage }
Copy-Item README.md, LICENSE $stage
Copy-Item docs/LAUNCHER.md, docs/SETTINGS.md "$stage/docs"
$zip = Join-Path $Out "PersonaRM-$Tag-windows-x64.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
# bsdtar writes a standard zip (forward slashes), which Compress-Archive in PowerShell 5.1 does not.
tar -a -c -f $zip -C $stage (Get-ChildItem $stage -Name)
if ($LASTEXITCODE) { throw "tar failed" }
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256" -Encoding ascii -NoNewline
Remove-Item $stage -Recurse -Force
Write-Host "$zip $hash"
