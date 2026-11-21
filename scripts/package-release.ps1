# Builds the release zip + .sha256 from already-built binaries. Used by .github/workflows/release.yml
# and runnable locally. Only program files and docs go in: no game data, BIOS or recompiled code.
# The native code is built on the player's PC from their own disc (personarm_native + libtcc.dll);
# the zip is checked against an exact list of files, so a generated module can never slip in.
param(
    [Parameter(Mandatory)][string]$Tag,        # e.g. v0.2.0
    [Parameter(Mandatory)][string]$BinDir,     # folder with personarm.exe, personarm_sdl.exe, SDL3.dll,
                                               # personarm_native.exe, libtcc.dll, TinyCC-*.txt
    [Parameter(Mandatory)][string]$Launcher,   # PersonaRM.exe (also shipped as launcher.exe, see docs/LAUNCHER.md)
    [string]$Out = "dist"
)
$ErrorActionPreference = 'Stop'
$stage = Join-Path $Out "stage"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage/docs", "$stage/licenses" | Out-Null
# launcher.exe: same binary under the old name, so installs from before the rename can still update.
Copy-Item $Launcher "$stage/PersonaRM.exe"
Copy-Item $Launcher "$stage/launcher.exe"
foreach ($f in 'personarm.exe', 'personarm_sdl.exe', 'SDL3.dll', 'personarm_native.exe', 'libtcc.dll') { Copy-Item (Join-Path $BinDir $f) $stage }
foreach ($f in 'TinyCC-LICENSE.txt', 'TinyCC-SOURCE.txt') { Copy-Item (Join-Path $BinDir $f) "$stage/licenses" }
Copy-Item README.md, LICENSE $stage
Copy-Item docs/LAUNCHER.md, docs/SETTINGS.md, docs/NATIVE_CODE.md "$stage/docs"
& "$PSScriptRoot/check-release-files.ps1" -Dir $stage
$zip = Join-Path $Out "PersonaRM-$Tag-windows-x64.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
# bsdtar writes a standard zip (forward slashes), which Compress-Archive in PowerShell 5.1 does not.
tar -a -c -f $zip -C $stage (Get-ChildItem $stage -Name)
if ($LASTEXITCODE) { throw "tar failed" }
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256" -Encoding ascii -NoNewline
Remove-Item $stage -Recurse -Force
Write-Host "$zip $hash"
