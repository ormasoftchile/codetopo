@echo off
setlocal enabledelayedexpansion

:: ============================================================================
:: profile_subset.bat — Quick profiling runs on controlled subsets
::
:: Usage:
::   profile_subset.bat <repo_path> <subset>
::
:: Subsets (using --root to subdirectories — avoids slow git ls-files scan):
::   tiny     ~500 files   (--max-files 500 on fsm)    ~5 seconds
::   small    ~2000 files  (--max-files 2000 on fsm)    ~10 seconds
::   fsm      4145 files   (mgmt/fsm, known baseline)   ~25 seconds
::   medium   ~10000 files (--max-files 10000 on mgmt)   ~60 seconds
::   large    ~50000 files (--max-files 50000 on mgmt)   ~5 minutes
::   full     all files    (no limit, full repo)         varies
::
:: For repos WITHOUT the DsMainDev/Sql structure, uses --max-files directly:
::   profile_subset.bat C:\MyRepo small
::
:: Examples:
::   profile_subset.bat C:\One\DsMainDev\Sql tiny
::   profile_subset.bat C:\One\DsMainDev\Sql fsm
::   profile_subset.bat C:\One\DsMainDev\Sql medium
:: ============================================================================

set "CODETOPO=%~dp0build\Release\codetopo.exe"
set "REPO=%~1"
set "SUBSET=%~2"

if "%REPO%"=="" (
    echo Usage: profile_subset.bat ^<repo_path^> ^<subset^>
    echo Subsets: tiny, small, fsm, medium, large, full
    exit /b 1
)
if "%SUBSET%"=="" set "SUBSET=small"

:: Common flags
set "COMMON=--large-arena-size 1024 --max-file-size 1024 --large-file-threshold 180 --parse-timeout 5 --turbo --profile --exclude **/GlobalSuppressions.cs"

:: Check for known subdirectories (DsMainDev/Sql repo structure)
set "FSM=%REPO%\xdb\manifest\svc\mgmt\fsm"
set "MGMT=%REPO%\xdb\manifest\svc\mgmt"
set "HAS_SUBDIRS=0"
if exist "%FSM%" set "HAS_SUBDIRS=1"

:: Determine effective root and max-files based on subset
set "EFF_ROOT=%REPO%"
set "MAX_FILES="

if /i "%SUBSET%"=="tiny" (
    if "%HAS_SUBDIRS%"=="1" (
        set "EFF_ROOT=%FSM%"
        set "MAX_FILES=--max-files 500"
        echo [tiny] ~500 files from mgmt/fsm via --max-files 500
    ) else (
        set "MAX_FILES=--max-files 500"
        echo [tiny] ~500 files via --max-files 500
    )
) else if /i "%SUBSET%"=="small" (
    if "%HAS_SUBDIRS%"=="1" (
        set "EFF_ROOT=%FSM%"
        set "MAX_FILES=--max-files 2000"
        echo [small] ~2000 files from mgmt/fsm via --max-files 2000
    ) else (
        set "MAX_FILES=--max-files 2000"
        echo [small] ~2000 files via --max-files 2000
    )
) else if /i "%SUBSET%"=="fsm" (
    if "%HAS_SUBDIRS%"=="1" (
        set "EFF_ROOT=%FSM%"
        echo [fsm] Known baseline: mgmt/fsm ~4145 C# files
    ) else (
        echo ERROR: fsm subset requires DsMainDev\Sql repo structure
        exit /b 1
    )
) else if /i "%SUBSET%"=="medium" (
    if "%HAS_SUBDIRS%"=="1" (
        set "EFF_ROOT=%MGMT%"
        set "MAX_FILES=--max-files 10000"
        echo [medium] ~10000 files from mgmt via --max-files 10000
    ) else (
        set "MAX_FILES=--max-files 10000"
        echo [medium] ~10000 files via --max-files 10000
    )
) else if /i "%SUBSET%"=="large" (
    if "%HAS_SUBDIRS%"=="1" (
        set "EFF_ROOT=%MGMT%"
        set "MAX_FILES=--max-files 50000"
        echo [large] ~50000 files from mgmt via --max-files 50000
    ) else (
        set "MAX_FILES=--max-files 50000"
        echo [large] ~50000 files via --max-files 50000
    )
) else if /i "%SUBSET%"=="full" (
    echo [full] All files, no limit
) else (
    echo Unknown subset: %SUBSET%
    echo Valid: tiny, small, fsm, medium, large, full
    exit /b 1
)

:: Delete stale DB for clean profile run
set "DB_DIR=!EFF_ROOT!\.codetopo"
if exist "!DB_DIR!\index.sqlite" (
    echo Removing stale DB for clean profile run...
    del /q "!DB_DIR!\index.sqlite" 2>nul
    del /q "!DB_DIR!\index.sqlite-wal" 2>nul
    del /q "!DB_DIR!\index.sqlite-shm" 2>nul
)

echo.
echo === codetopo profile: %SUBSET% ===
echo Root: !EFF_ROOT!
echo.

"%CODETOPO%" index --root "!EFF_ROOT!" %MAX_FILES% %COMMON%

echo.
echo === Profile complete ===
