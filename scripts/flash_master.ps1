#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Flash ESP-LEGO master firmware (ESP32-S3) with COM auto-detect.
.DESCRIPTION
    Usage:
      .\scripts\flash_master.ps1              # flash, auto-detect COM
      .\scripts\flash_master.ps1 -Port COM4   # specify COM port manually
      .\scripts\flash_master.ps1 -Monitor     # flash + serial monitor
.PARAMETER Port
    COM port (e.g. COM4). Auto-detected if omitted.
.PARAMETER Monitor
    Open serial monitor after flashing.
#>
param(
    [string]$Port,
    [switch]$Monitor
)

$ErrorActionPreference = "Stop"

. "$PSScriptRoot\idf_env.ps1"
Initialize-IdfEnvironment -Chip esp32s3
Set-Location -LiteralPath $project_root

function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok  ($msg) { Write-Host "  ✓ $msg" -ForegroundColor Green }

# detect COM (if not specified)
if (-not $Port) {
    Write-Step "Auto-detecting COM port for ESP32-S3..."
    $comPorts = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($comPorts.Count -eq 0) { Write-Host "No COM ports found!" -ForegroundColor Red; exit 1 }

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
        Write-Host "Specify: .\scripts\flash_master.ps1 -Port COM4"
        exit 1
    }
}

# flash
Write-Step "Flashing MASTER to $Port ..."
& $python "$idf_path\tools\idf.py" -p $Port flash
if ($LASTEXITCODE -ne 0) { Write-Host "FLASH FAILED" -ForegroundColor Red; exit 1 }
Write-Ok "Flash complete"

if ($Monitor) {
    Write-Step "Opening serial monitor on $Port ..."
    & $python "$idf_path\tools\idf.py" -p $Port monitor
}

Write-Host "`nDone." -ForegroundColor Green
