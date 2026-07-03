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
      1. CMake bundled with the matched Visual Studio install, found by filesystem
         glob (reuses the install pinned in build\CMakeCache.txt so the build stays
         incremental). Works even when vswhere.exe is not installed.
      2. cmake already on PATH
      3. CMake bundled with Visual Studio via vswhere (if installed)
      4. A standalone install at "C:\Program Files\CMake\bin\cmake.exe"

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

# ─── Locate Visual Studio / cmake / MSVC (vswhere-free) ─────────────────────────

# Newest-first list of VS install roots (…\Microsoft Visual Studio\<year>\<edition>)
# that carry the MSVC toolset. Used instead of vswhere.exe, which is not always
# installed — and when it's missing, vcpkg can't auto-detect Visual Studio either.
function Get-VsInstallRoots {
    $base = 'C:\Program Files\Microsoft Visual Studio'
    if (-not (Test-Path $base)) { return @() }
    # Rank installs by their newest MSVC toolset version (descending). Folder names mix
    # year (2019/2022) and product major (18 = VS 2026), so a plain name sort is wrong
    # ("2022" sorts above "18" even though 18 is newer). The toolset version under
    # VC\Tools\MSVC is an unambiguous newest-wins signal.
    Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
        Where-Object { Test-Path (Join-Path $_.FullName 'VC\Auxiliary\Build\vcvars64.bat') } |
        ForEach-Object {
            $toolset = [version]'0.0'
            $msvcDir = Join-Path $_.FullName 'VC\Tools\MSVC'
            if (Test-Path $msvcDir) {
                foreach ($d in (Get-ChildItem $msvcDir -Directory -ErrorAction SilentlyContinue)) {
                    $v = $null
                    if ([version]::TryParse($d.Name, [ref]$v) -and $v -gt $toolset) { $toolset = $v }
                }
            }
            [pscustomobject]@{ Path = $_.FullName; Toolset = $toolset }
        } |
        Sort-Object Toolset -Descending |
        Select-Object -ExpandProperty Path
}

# Choose which VS install to build with. If build\CMakeCache.txt already pins a
# generator instance, reuse that exact install so the build stays INCREMENTAL — a
# mismatch forces CMake to wipe and reconfigure (the slow "from scratch" rebuild).
# Otherwise use the newest install found.
function Resolve-VsRoot {
    $roots = @(Get-VsInstallRoots)
    if (-not $roots) { return $null }
    $cache = Join-Path $ScriptDir 'build\CMakeCache.txt'
    if (Test-Path $cache) {
        $m = Select-String -Path $cache -Pattern '^CMAKE_GENERATOR_INSTANCE:INTERNAL=(.+)$' -ErrorAction SilentlyContinue
        if ($m) {
            $pinned = ($m.Matches[0].Groups[1].Value).Trim() -replace '/', '\'
            $hit = $roots | Where-Object { $_ -ieq $pinned } | Select-Object -First 1
            if ($hit) { return $hit }
        }
    }
    return $roots[0]
}

# Import the MSVC dev environment (vcvars64.bat) for the chosen VS install into this
# session. vcpkg and CMake need the toolset on PATH/INCLUDE/LIB; importing vcvars is
# the reliable way to make MSVC discoverable without vswhere.
#
# Crucially, if a cl.exe from a DIFFERENT VS install is already on PATH (e.g. inherited
# from an earlier shell), re-import the chosen install's vcvars anyway. Otherwise vcpkg
# builds dependencies with one toolset while CMake links the project with another,
# producing unresolved-symbol link errors (LNK2019) from mismatched STL versions.
function Initialize-MsvcEnv {
    param([string]$VsRoot)
    $clCmd = Get-Command cl -ErrorAction SilentlyContinue
    $cl = if ($clCmd) { $clCmd.Source } else { $null }
    if ($cl -and (-not $VsRoot -or $cl.StartsWith($VsRoot, [System.StringComparison]::OrdinalIgnoreCase))) {
        return $true
    }
    if (-not $VsRoot) { return $false }
    $vcvars = Join-Path $VsRoot 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { return $false }
    # Clear the dev-shell re-entrancy guards so vcvars fully re-initializes even when a
    # DIFFERENT VS environment is already active in this session. Otherwise vcvars sees
    # VSCMD_VER set and just echoes back the stale, inherited toolset (mixed-toolchain).
    $cmdLine = "set `"VSCMD_VER=`" && set `"__VSCMD_PREINIT_PATH=`" && `"$vcvars`" >nul 2>&1 && set"
    & cmd.exe /c $cmdLine | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    $newClCmd = Get-Command cl -ErrorAction SilentlyContinue
    $newCl = if ($newClCmd) { $newClCmd.Source } else { $null }
    return [bool]($newCl -and (-not $VsRoot -or $newCl.StartsWith($VsRoot, [System.StringComparison]::OrdinalIgnoreCase)))
}

function Test-BuildCacheToolchainMismatch {
    param([string]$VsRoot, [string]$ClPath)

    $cache = Join-Path $ScriptDir 'build\CMakeCache.txt'
    if (-not (Test-Path $cache)) { return $false }

    $raw = Get-Content $cache -Raw
    $generatorMatch = [regex]::Match($raw, '(?m)^CMAKE_GENERATOR_INSTANCE:INTERNAL=(.+)$')
    if ($generatorMatch.Success) {
        $cachedVsRoot = ($generatorMatch.Groups[1].Value).Trim() -replace '/', '\'
        if ($cachedVsRoot -and $cachedVsRoot -ne $VsRoot) {
            return $true
        }
    }

    $compilerMatch = [regex]::Match($raw, '(?m)^CMAKE_CXX_COMPILER:FILEPATH=(.+)$')
    if ($compilerMatch.Success) {
        $cachedCompiler = ($compilerMatch.Groups[1].Value).Trim() -replace '/', '\'
        if ($cachedCompiler -and $cachedCompiler -ne $ClPath) {
            return $true
        }
    }

    $cCompilerMatch = [regex]::Match($raw, '(?m)^CMAKE_C_COMPILER:FILEPATH=(.+)$')
    if ($cCompilerMatch.Success) {
        $cachedCCompiler = ($cCompilerMatch.Groups[1].Value).Trim() -replace '/', '\'
        if ($cachedCCompiler -and $cachedCCompiler -ne $ClPath) {
            return $true
        }
    }

    return $false
}

# ─── Locate cmake ───────────────────────────────────────────────────────────────
function Resolve-CMake {
    param([string]$VsRoot)

    # Prefer the CMake bundled with the chosen VS install: its default generator
    # matches the existing build cache, keeping the build incremental.
    if ($VsRoot) {
        $bundled = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $bundled) { return $bundled }
    }

    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    # Try Visual Studio's bundled CMake via vswhere (if present).
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

$VsRoot = Resolve-VsRoot
if ($VsRoot) { Write-Ok "Visual Studio: $VsRoot" }

$CMake = Resolve-CMake -VsRoot $VsRoot
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

# C++20 compiler — MSVC. Import the dev environment so cl.exe / INCLUDE / LIB are set
# for vcpkg and CMake. This works without vswhere, which isn't always installed.
if (Initialize-MsvcEnv -VsRoot $VsRoot) {
    $clPath = (Get-Command cl -ErrorAction SilentlyContinue).Source
    Write-Ok "Compiler: MSVC ($clPath)"
} else {
    Write-WarnMsg "MSVC C++ toolset not detected. Install Visual Studio's 'Desktop development with C++' workload."
    Write-WarnMsg "CMake configuration will fail without a C++20 compiler."
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

    $clPath = (Get-Command cl -ErrorAction SilentlyContinue).Source
    if ($VsRoot -and $clPath -and (Test-BuildCacheToolchainMismatch -VsRoot $VsRoot -ClPath $clPath)) {
        Write-WarnMsg "Existing build cache points at a different toolchain; resetting build directory..."
        Remove-Item -Recurse -Force (Join-Path $ScriptDir 'build') -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Path (Join-Path $ScriptDir 'build') -Force | Out-Null
    }

    Write-Info 'Configuring...'
    $configureArgs = @('--preset', 'release')
    if ($VsRoot -and $clPath) {
        $configureArgs += @("-DCMAKE_C_COMPILER=$clPath", "-DCMAKE_CXX_COMPILER=$clPath")
    }
    & $CMake @configureArgs
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
