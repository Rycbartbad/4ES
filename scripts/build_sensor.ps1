#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Build, flash, and monitor ESP-LEGO sensor firmware.
.DESCRIPTION
    Auto-switches configuration to sensor role. Buzzer and pump profiles target
    ESP32-C3. The light profile targets a separate ESP32-S3 with a BH1750.
    Usage:
      .\scripts\build_sensor.ps1              # build only
      .\scripts\build_sensor.ps1 -Profile pump
      .\scripts\build_sensor.ps1 -Profile light
      .\scripts\build_sensor.ps1 -Flash       # build + auto-detect COM + flash
      .\scripts\build_sensor.ps1 -Monitor     # build + flash + serial monitor
      .\scripts\build_sensor.ps1 -Port COM5   # specify COM port manually
.PARAMETER Flash
    Build and flash to device.
.PARAMETER Monitor
    Build, flash, then open serial monitor.
.PARAMETER Port
    COM port (e.g. COM5). Auto-detected if omitted.
.PARAMETER Profile
    Firmware profile: buzzer (default), pump, or light.
#>
param(
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Port,
    [ValidateSet("buzzer", "pump", "light")]
    [string]$Profile = "buzzer"
)

$ErrorActionPreference = "Stop"
$Profile = $Profile.ToLowerInvariant()
$target_chip = if ($Profile -eq "light") { "esp32s3" } else { "esp32c3" }

. "$PSScriptRoot\idf_env.ps1"
Initialize-IdfEnvironment -Chip $target_chip
Set-Location -LiteralPath $project_root

# ── helpers ──────────────────────────────────────────────────────────
function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok  ($msg) { Write-Host "  ✓ $msg" -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "  ⚠ $msg" -ForegroundColor Yellow }

function Is-Sensor-Config {
    $t = Select-String -Path sdkconfig -Pattern "CONFIG_IDF_TARGET=`"$target_chip`"" -Quiet
    $r = Select-String -Path sdkconfig -Pattern 'CONFIG_DEVICE_ROLE_SENSOR=y' -Quiet
    $profile_pattern = switch ($Profile) {
        "pump" { 'CONFIG_SENSOR_ACTUATOR_PUMP=y' }
        "light" { 'CONFIG_SENSOR_ACQUISITION_BH1750=y' }
        default { 'CONFIG_SENSOR_ACTUATOR_BUZZER=y' }
    }
    $p = Select-String -Path sdkconfig -Pattern $profile_pattern -Quiet
    if (-not ($t -and $r -and $p)) { return $false }

    # Also check CMakeCache target — may be stale from previous build
    if (Test-Path "build\CMakeCache.txt") {
        $cmake_target = Select-String -Path "build\CMakeCache.txt" -Pattern 'IDF_TARGET:STRING=(\S+)' | ForEach-Object { $_.Matches.Groups[1].Value }
        if ($cmake_target -ne $target_chip) { return $false }
    }
    return $true
}

# ── step 1: auto-switch config ──────────────────────────────────────
Write-Step "Checking configuration..."

$saved_config = switch ($Profile) {
    "pump" { "sdkconfig.sensor.pump" }
    "light" { "sdkconfig.sensor.light" }
    default { "sdkconfig.sensor" }
}
$config_defaults = "sdkconfig.defaults;sdkconfig.defaults.sensor"
if ($Profile -ne "buzzer") {
    $config_defaults += ";sdkconfig.defaults.$Profile"
}

if (-not (Test-Path sdkconfig)) {
    # No config at all — generate fresh
    Write-Warn "No sdkconfig found — generating $Profile sensor config..."
    $env:SDKCONFIG_DEFAULTS = $config_defaults
    & $python "$idf_path\tools\idf.py" set-target $target_chip 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "set-target FAILED" -ForegroundColor Red; exit 1 }
    # Save for next time
    Copy-Item sdkconfig $saved_config -Force
    Write-Ok "Config generated and saved as $saved_config"
}
else {
    $current_ok = Is-Sensor-Config
    if ($current_ok) {
        Write-Ok "Already $Profile sensor config ($target_chip + DEVICE_ROLE_SENSOR)"
    }
    else {
        Write-Warn "Current config is NOT sensor — switching..."
        Copy-Item sdkconfig "sdkconfig.local.backup" -Force

        if (Test-Path $saved_config) {
            Copy-Item $saved_config sdkconfig -Force
            Write-Ok "Restored $saved_config (backup saved as sdkconfig.local.backup)"
        }

        # CMakeCache.txt may still reference old target — force fullclean
        & $python "$idf_path\tools\idf.py" fullclean 2>&1 | Out-Null
        if (-not (Test-Path $saved_config)) {
            # No saved sensor config — generate fresh with set-target
            $env:SDKCONFIG_DEFAULTS = $config_defaults
            & $python "$idf_path\tools\idf.py" set-target $target_chip 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { Write-Host "set-target FAILED" -ForegroundColor Red; exit 1 }
            Copy-Item sdkconfig $saved_config -Force
            Write-Ok "Generated and saved as $saved_config"
        }
    }
}

# ── step 2: build ───────────────────────────────────────────────────
Write-Step "Building SENSOR firmware ($target_chip, profile=$Profile)..."
& $python "$idf_path\tools\idf.py" build
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit 1 }

# Save config after successful build
Copy-Item sdkconfig $saved_config -Force
Write-Ok "Build complete (config saved)"

# ── step 3: detect COM + flash ──────────────────────────────────────
if ($Flash -or $Monitor) {
    if ($Profile -eq "light" -and -not $Port) {
        Write-Host "ERROR: The light sensor and Master are both ESP32-S3 boards." -ForegroundColor Red
        Write-Host "Specify the light sensor port explicitly with -Port COMx to avoid flashing the Master."
        exit 1
    }
    if (-not $Port) {
        Write-Step "Auto-detecting COM port for $target_chip..."
        $comPorts = [System.IO.Ports.SerialPort]::GetPortNames()
        if ($comPorts.Count -eq 0) { Write-Host "No COM ports found!" -ForegroundColor Red; exit 1 }

        $Port = $null
        foreach ($com in $comPorts) {
            Write-Host "  Probing $com ... " -NoNewline
            $result = & $python -m esptool --port $com chip_id 2>&1
            $chip_pattern = if ($target_chip -eq "esp32s3") { "ESP32-S3" } else { "ESP32-C3" }
            if ($LASTEXITCODE -eq 0 -and $result -match $chip_pattern) {
                $Port = $com
                Write-Host "FOUND!" -ForegroundColor Green
                break
            }
            Write-Host "no match"
        }

        if (-not $Port) {
            Write-Host "ERROR: No $target_chip found." -ForegroundColor Red
            Write-Host "Available ports: $($comPorts -join ', ')"
            Write-Host "Specify: .\scripts\build_sensor.ps1 -Flash -Port COM5"
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
