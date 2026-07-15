#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Build, flash, and monitor ESP-LEGO master firmware (ESP32-S3).
.DESCRIPTION
    Auto-switches configuration to master role. Usage:
      .\scripts\build_master.ps1              # build only
      .\scripts\build_master.ps1 -Flash       # build + auto-detect COM + flash
      .\scripts\build_master.ps1 -Monitor     # build + flash + serial monitor
      .\scripts\build_master.ps1 -Port COM4   # specify COM port manually
.PARAMETER Flash
    Build and flash to device.
.PARAMETER Monitor
    Build, flash, then open serial monitor.
.PARAMETER Port
    COM port (e.g. COM4). Auto-detected if omitted.
#>
param(
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Port
)

$ErrorActionPreference = "Stop"

. "$PSScriptRoot\idf_env.ps1"
Initialize-IdfEnvironment -Chip esp32s3
Set-Location -LiteralPath $project_root

# ── helpers ──────────────────────────────────────────────────────────
function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok  ($msg) { Write-Host "  ✓ $msg" -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "  ⚠ $msg" -ForegroundColor Yellow }

function Is-Master-Config {
    $t = Select-String -Path sdkconfig -Pattern 'CONFIG_IDF_TARGET="esp32s3"' -Quiet
    $r = Select-String -Path sdkconfig -Pattern 'CONFIG_DEVICE_ROLE_MASTER=y' -Quiet
    if (-not ($t -and $r)) { return $false }

    # Also check CMakeCache target — may be stale from previous build
    if (Test-Path "build\CMakeCache.txt") {
        $cmake_target = Select-String -Path "build\CMakeCache.txt" -Pattern 'IDF_TARGET:STRING=(\S+)' | ForEach-Object { $_.Matches.Groups[1].Value }
        if ($cmake_target -ne "esp32s3") { return $false }
    }
    return $true
}

# ── step 1: auto-switch config ──────────────────────────────────────
Write-Step "Checking configuration..."

$saved_config = "sdkconfig.master"

if (-not (Test-Path sdkconfig)) {
    Write-Warn "No sdkconfig found — generating master config..."
    $env:SDKCONFIG_DEFAULTS = "sdkconfig.defaults;sdkconfig.defaults.master"
    & $python "$idf_path\tools\idf.py" set-target esp32s3 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "set-target FAILED" -ForegroundColor Red; exit 1 }
    Copy-Item sdkconfig $saved_config -Force
    Write-Ok "Config generated and saved as $saved_config"
}
else {
    $current_ok = Is-Master-Config
    if ($current_ok) {
        Write-Ok "Already master config (esp32s3 + DEVICE_ROLE_MASTER)"
    }
    else {
        Write-Warn "Current config is NOT master — switching..."
        Copy-Item sdkconfig "sdkconfig.backup" -Force

        if (Test-Path $saved_config) {
            Copy-Item $saved_config sdkconfig -Force
            Write-Ok "Restored $saved_config (backup saved as sdkconfig.backup)"
        }

        # CMakeCache.txt may still reference old target — force fullclean
        & $python "$idf_path\tools\idf.py" fullclean 2>&1 | Out-Null
        if (-not (Test-Path $saved_config)) {
            # No saved master config — generate fresh with set-target
            $env:SDKCONFIG_DEFAULTS = "sdkconfig.defaults;sdkconfig.defaults.master"
            & $python "$idf_path\tools\idf.py" set-target esp32s3 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { Write-Host "set-target FAILED" -ForegroundColor Red; exit 1 }
            Copy-Item sdkconfig $saved_config -Force
            Write-Ok "Generated and saved as $saved_config"
        }
    }
}

# ── step 2: build ───────────────────────────────────────────────────
Write-Step "Building MASTER firmware (ESP32-S3)..."
& $python "$idf_path\tools\idf.py" build
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit 1 }

Copy-Item sdkconfig $saved_config -Force
Write-Ok "Build complete (config saved)"

# ── step 3: detect COM + flash ──────────────────────────────────────
if ($Flash -or $Monitor) {
    if (-not $Port) {
        Write-Step "Auto-detecting COM port for ESP32-S3..."
        $comPorts = [System.IO.Ports.SerialPort]::GetPortNames()
        if ($comPorts.Count -eq 0) { Write-Host "No COM ports found!" -ForegroundColor Red; exit 1 }

        $Port = $null
        foreach ($com in $comPorts) {
            Write-Host "  Probing $com ... " -NoNewline
            $result = & $python -m esptool --port $com chip_id 2>&1
            if ($LASTEXITCODE -eq 0 -and $result -match "ESP32-S3") {
                $Port = $com
                Write-Host "FOUND!" -ForegroundColor Green
                break
            }
            Write-Host "no match"
        }

        if (-not $Port) {
            Write-Host "ERROR: No ESP32-S3 found." -ForegroundColor Red
            Write-Host "Available ports: $($comPorts -join ', ')"
            Write-Host "Specify: .\scripts\build_master.ps1 -Flash -Port COM4"
            exit 1
        }
    }

    Write-Step "Flashing to $Port ..."
    & $python "$idf_path\tools\idf.py" -p $Port flash
    if ($LASTEXITCODE -ne 0) { Write-Host "FLASH FAILED" -ForegroundColor Red; exit 1 }
    Write-Ok "Flash complete"
}

# ── step 4: monitor ─────────────────────────────────────────────────
if ($Monitor) {
    Write-Step "Opening serial monitor on $Port ..."
    & $python "$idf_path\tools\idf.py" -p $Port monitor
}

Write-Host "`nDone." -ForegroundColor Green
