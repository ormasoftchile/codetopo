<#
.SYNOPSIS
    install.ps1 — Bootstrap codetopo on Windows.

.DESCRIPTION
    What it does:
      1. Checks prerequisites (cmake >= 3.20, MSVC C++20 compiler, vcpkg)
      2. Builds codetopo in Release mode
      3. Installs the binary to %LOCALAPPDATA%\Programs\codetopo\codetopo.exe
      4. Adds the install dir to the user PATH (if missing)
      5. Validates the installed binary
      6. Prints a next-step hint

    cmake is located in this order:
      1. cmake already on PATH
      2. CMake bundled with Visual Studio (located via vswhere)
      3. A standalone install at "C:\Program Files\CMake\bin\cmake.exe"

    NOTE — cmake --install support:
      CMakeLists.txt does not currently define an install() target, so this
      script copies the binary directly instead of using `cmake --install`.

    Safe to re-run (idempotent).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install.ps1
#>

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$InstallDir  = Join-Path $env:LOCALAPPDATA 'Programs\codetopo'
$BinaryName  = 'codetopo.exe'
$BuildBinary = Join-Path $ScriptDir 'build\Release\codetopo.exe'

# ─── Output helpers ─────────────────────────────────────────────────────────────
function Write-Info    { param([string]$m) Write-Host "  -> $m" -ForegroundColor Cyan }
function Write-Ok      { param([string]$m) Write-Host "  [OK] $m" -ForegroundColor Green }
function Write-WarnMsg  { param([string]$m) Write-Host "  [!] $m" -ForegroundColor Yellow }
function Write-Section { param([string]$m) Write-Host "`n$m" -ForegroundColor White }
function Die           { param([string]$m) Write-Host "  [X] ERROR: $m" -ForegroundColor Red; exit 1 }

# ─── Locate cmake ───────────────────────────────────────────────────────────────
function Resolve-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    # Try Visual Studio's bundled CMake via vswhere.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.CMake.Project `
            -property installationPath 2>$null
        if ($vsRoot) {
            $candidate = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }

    # Standalone install.
    $standalone = 'C:\Program Files\CMake\bin\cmake.exe'
    if (Test-Path $standalone) { return $standalone }

    return $null
}

# ─── 1. Prerequisites ───────────────────────────────────────────────────────────
Write-Section 'Checking prerequisites...'

$CMake = Resolve-CMake
if (-not $CMake) {
    Die "cmake not found. Install CMake >= 3.20 (https://cmake.org/download/) or the Visual Studio 'C++ CMake tools for Windows' component."
}

# Ensure the resolved cmake's directory is on PATH for this session.
$cmakeDir = Split-Path -Parent $CMake
if (($env:PATH -split ';') -notcontains $cmakeDir) {
    $env:PATH = "$cmakeDir;$env:PATH"
}

$cmakeVersionStr = (& $CMake --version | Select-Object -First 1) -replace '^cmake version\s+', ''
$cmakeVersionStr = ([regex]::Match($cmakeVersionStr, '\d+\.\d+\.\d+')).Value
$cmakeVersion    = [version]$cmakeVersionStr
if ($cmakeVersion -lt [version]'3.20.0') {
    Die "cmake $cmakeVersionStr found, but >= 3.20 is required."
}
Write-Ok "cmake $cmakeVersionStr ($CMake)"

# C++20 compiler — MSVC. Locate a VS install with the VC toolset.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $vsInstall = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property displayName 2>$null
    if ($vsInstall) {
        Write-Ok "Compiler: $vsInstall (MSVC)"
    } else {
        Write-WarnMsg "No MSVC C++ toolset detected via vswhere. Install the 'Desktop development with C++' workload."
        Write-WarnMsg "CMake configuration will fail without a C++20 compiler."
    }
} else {
    Write-WarnMsg "vswhere not found; cannot verify MSVC. Ensure Visual Studio with the C++ workload is installed."
}

# vcpkg — CMakeLists.txt will auto-clone if missing, but warn so the user knows.
if (-not $env:VCPKG_ROOT) {
    $repoVcpkg = Join-Path $ScriptDir 'vcpkg\scripts\buildsystems\vcpkg.cmake'
    if (Test-Path $repoVcpkg) {
        $env:VCPKG_ROOT = (Join-Path $ScriptDir 'vcpkg')
        Write-Ok "vcpkg at $env:VCPKG_ROOT (in-repo)"
    } else {
        Write-WarnMsg "VCPKG_ROOT is not set. CMake will auto-clone vcpkg into $ScriptDir\vcpkg (requires internet)."
        Write-WarnMsg "To skip the clone, set VCPKG_ROOT to an existing vcpkg installation:"
        Write-WarnMsg '  $env:VCPKG_ROOT = "C:\path\to\vcpkg"'
    }
} else {
    if (-not (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
        Write-WarnMsg "VCPKG_ROOT=$env:VCPKG_ROOT is set but vcpkg.cmake not found there. CMake will auto-clone."
    } else {
        Write-Ok "vcpkg at $env:VCPKG_ROOT"
    }
}

# ─── 2. Build ───────────────────────────────────────────────────────────────────
Write-Section 'Building codetopo (Release)...'

Push-Location $ScriptDir
try {
    # A running codetopo.exe (e.g. an MCP server attached to an editor) locks the
    # output binary and causes LNK1104. Stop any running instance first.
    $running = Get-Process codetopo -ErrorAction SilentlyContinue
    if ($running) {
        Write-WarnMsg "Stopping running codetopo.exe (PID $($running.Id -join ', ')) to free the build output."
        $running | Stop-Process -Force
        Start-Sleep -Milliseconds 300
    }

    Write-Info 'Configuring...'
    & $CMake --preset release
    if ($LASTEXITCODE -ne 0) { Die "CMake configuration failed (exit $LASTEXITCODE)." }

    Write-Info 'Compiling... (this may take a few minutes on first run)'
    & $CMake --build build --config Release --target codetopo
    if ($LASTEXITCODE -ne 0) { Die "Build failed (exit $LASTEXITCODE)." }
}
finally {
    Pop-Location
}

if (-not (Test-Path $BuildBinary)) {
    Die "Build reported success but $BuildBinary was not found."
}
Write-Ok "Build complete: $BuildBinary"

# ─── 3. Install ─────────────────────────────────────────────────────────────────
Write-Section "Installing to $InstallDir..."

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

$installedPath = Join-Path $InstallDir $BinaryName

# Stop a running instance from the install location too, so the copy succeeds.
Get-Process codetopo -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $installedPath } |
    Stop-Process -Force

Copy-Item -Force $BuildBinary $installedPath
Write-Ok "Installed: $installedPath"

# The vcpkg x64-windows triplet links dependencies as DLLs, so the binary needs
# them next to the exe at runtime. Copy every DLL from the build output dir.
$buildDir = Split-Path -Parent $BuildBinary
$dlls = Get-ChildItem -Path (Join-Path $buildDir '*.dll') -ErrorAction SilentlyContinue
foreach ($dll in $dlls) {
    Copy-Item -Force $dll.FullName (Join-Path $InstallDir $dll.Name)
}
if ($dlls) {
    Write-Ok "Copied $($dlls.Count) runtime DLL(s): $(( $dlls | ForEach-Object Name ) -join ', ')"
}

# ─── 4. PATH ────────────────────────────────────────────────────────────────────
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$userPathEntries = @()
if ($userPath) { $userPathEntries = $userPath -split ';' | Where-Object { $_ } }

if ($userPathEntries -notcontains $InstallDir) {
    $newUserPath = (@($userPathEntries) + $InstallDir) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $newUserPath, 'User')
    Write-Ok "Added $InstallDir to your user PATH."
    Write-WarnMsg 'Open a new terminal for the PATH change to take effect.'
} else {
    Write-Ok "$InstallDir is already on your user PATH."
}

# Make it available for validation in this session.
if (($env:PATH -split ';') -notcontains $InstallDir) {
    $env:PATH = "$InstallDir;$env:PATH"
}

# ─── 5. Validate ────────────────────────────────────────────────────────────────
Write-Section 'Validating...'

$validated = $false
try {
    $versionOut = & $installedPath --version 2>&1 | Select-Object -First 1
    if ($LASTEXITCODE -eq 0) {
        Write-Ok "codetopo is working: $versionOut"
        $validated = $true
    }
} catch { }

if (-not $validated) {
    try {
        & $installedPath --help *> $null
        if ($LASTEXITCODE -eq 0) {
            Write-Ok 'codetopo is working (--help succeeded)'
            $validated = $true
        }
    } catch { }
}

if (-not $validated) {
    Die 'Installed binary did not respond to --version or --help. Check the build output above.'
}

# ─── 6. Next steps ──────────────────────────────────────────────────────────────
Write-Host ''
Write-Host '[OK] codetopo installed successfully!' -ForegroundColor Green
Write-Host ''
Write-Host '  Next step: Index your project and configure your editor:' -ForegroundColor White
Write-Host '  codetopo init --root C:\path\to\your\project' -ForegroundColor Cyan
Write-Host ''
Write-Host '  Options:'
Write-Host '    --editors vscode,cursor,copilot   # choose editor targets'
Write-Host '    --watch                           # enable file-watching mode'
Write-Host ''
