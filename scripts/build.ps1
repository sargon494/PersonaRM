# PersonaRM Windows Quick Build Script
Param(
    [string]$BuildType = "Release",
    [switch]$RunTests,
    [switch]$Launch
)

$ErrorActionPreference = "Stop"

Write-Host "=== PersonaRM Build ($BuildType) ===" -ForegroundColor Cyan

# 1. Configure CMake
Write-Host "`n[1/3] Configuring CMake..." -ForegroundColor Yellow
if ($RunTests) {
    cmake -B build -DCMAKE_BUILD_TYPE=$BuildType -DBUILD_TESTS=ON
} else {
    cmake -B build -DCMAKE_BUILD_TYPE=$BuildType
}

# 2. Build
Write-Host "`n[2/3] Compiling Project..." -ForegroundColor Yellow
cmake --build build --config $BuildType

# 3. Tests / Launch
if ($RunTests) {
    Write-Host "`n[3/3] Running Unit Tests..." -ForegroundColor Yellow
    ctest --test-dir build --output-on-failure -C $BuildType
} elseif ($Launch) {
    Write-Host "`n[3/3] Launching PersonaRM..." -ForegroundColor Green
    & ".\build\$BuildType\personarm.exe" --launch
} else {
    Write-Host "`n[3/3] Build finished successfully! Executable: .\build\$BuildType\personarm.exe" -ForegroundColor Green
}
