#!/usr/bin/env pwsh
# bench.ps1 — Cold-cache benchmark runner for codetopo
# Usage: .\bench.ps1 [-Scale 100000] [-Threads 16] [-Root C:\One\DsMainDev] [-SkipFlush]

param(
    [int]$Scale = 0,           # --max-files (0 = all files)
    [int]$Threads = 16,
    [string]$Root = "C:\One\DsMainDev",
    [int]$ArenaSize = 128,
    [int]$LargeArenaSize = 1024,
    [int]$LargeFileThreshold = 150,
    [int]$MaxFileSize = 512,
    [switch]$SkipFlush,
    [string]$Label = ""
)

$ErrorActionPreference = "Stop"
$exe = ".\build\Release\codetopo.exe"

if (!(Test-Path $exe)) {
    Write-Error "Build not found at $exe. Run cmake --build build --config Release first."
    exit 1
}

# --- Step 1: Flush OS file cache ---
if (!$SkipFlush) {
    Write-Host "`n=== Flushing OS file cache ===" -ForegroundColor Cyan
    $flushCode = @"
using System;
using System.Collections.Generic;
public class BenchCacheFlush {
    public static void Flush(int totalGB) {
        var arrays = new List<byte[]>();
        int chunkMB = 512;
        int chunks = totalGB * 1024 / chunkMB;
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < chunks; i++) {
                try {
                    byte[] arr = new byte[chunkMB * 1024L * 1024L];
                    for (long j = 0; j < arr.Length; j += 4096) arr[j] = 1;
                    arrays.Add(arr);
                } catch (OutOfMemoryException) { break; }
            }
            arrays.Clear();
            GC.Collect(2, GCCollectionMode.Forced, true);
            GC.WaitForPendingFinalizers();
        }
    }
}
"@
    try { [BenchCacheFlush]::Flush(1) } catch {
        Add-Type -TypeDefinition $flushCode
    }
    [BenchCacheFlush]::Flush(24)

    $p = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
    $standbyMB = [math]::Round($p.StandbyCacheNormalPriorityBytes / 1MB +
                               $p.StandbyCacheCoreBytes / 1MB +
                               $p.StandbyCacheReserveBytes / 1MB)
    Write-Host "Standby cache: ${standbyMB} MB" -ForegroundColor Yellow
    if ($standbyMB -gt 4096) {
        Write-Host "WARNING: Standby still high. Results may not be fully cold." -ForegroundColor Red
    }
} else {
    Write-Host "`n=== Skipping cache flush (warm run) ===" -ForegroundColor Yellow
}

# --- Step 2: Delete existing index ---
Write-Host "`n=== Deleting index ===" -ForegroundColor Cyan
$dbPath = Join-Path $Root ".codetopo"
Remove-Item (Join-Path $dbPath "index.sqlite*") -ErrorAction SilentlyContinue

# --- Step 3: Build args ---
$args = @("index", "--root", $Root, "--threads", $Threads,
          "--arena-size", $ArenaSize, "--large-arena-size", $LargeArenaSize,
          "--large-file-threshold", $LargeFileThreshold,
          "--max-file-size", $MaxFileSize, "--profile")
if ($Scale -gt 0) {
    $args += @("--max-files", $Scale)
}

$scaleLabel = if ($Scale -gt 0) { "${Scale}" } else { "FULL" }
$tag = if ($Label) { $Label } else { $scaleLabel }

Write-Host "`n=== Running benchmark: $tag files, $Threads threads ===" -ForegroundColor Cyan
Write-Host "Command: $exe $($args -join ' ')" -ForegroundColor DarkGray

# --- Step 4: Run and capture ---
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$output = & $exe @args 2>&1 | Out-String
$sw.Stop()

$wallTime = [math]::Round($sw.Elapsed.TotalSeconds, 1)

# --- Step 5: Parse results ---
$doneMatch = [regex]::Match($output, 'Done: (\d+) files in (\d+)s \((\d+) files/s\)')
$crashCount = ([regex]::Matches($output, 'crash #\d+')).Count
$killCount = ([regex]::Matches($output, 'WATCHDOG: killing')).Count
$quarantineMatch = [regex]::Match($output, 'Quarantine: (\d+) file')
$quarantined = if ($quarantineMatch.Success) { $quarantineMatch.Groups[1].Value } else { "0" }

# Extract profile data
$profileMatch = [regex]::Match($output, 'Total: ([\d.]+) ms \| (\d+) files \| (\d+) files/s')

Write-Host "`n=== Results: $tag ===" -ForegroundColor Green
Write-Host $output

# --- Step 6: Summary ---
Write-Host "`n=== Summary ===" -ForegroundColor Green
Write-Host "Scale:      $tag"
Write-Host "Wall time:  ${wallTime}s"
Write-Host "Crashes:    $crashCount"
Write-Host "WD kills:   $killCount"
Write-Host "Quarantine: $quarantined"
if ($doneMatch.Success) {
    Write-Host "Indexing:   $($doneMatch.Groups[2].Value)s ($($doneMatch.Groups[3].Value) f/s)"
}
if ($profileMatch.Success) {
    Write-Host "Profile:    $($profileMatch.Groups[1].Value)ms total, $($profileMatch.Groups[3].Value) f/s"
}
Write-Host "Cache:      $(if ($SkipFlush) { 'WARM' } else { 'COLD' })"
