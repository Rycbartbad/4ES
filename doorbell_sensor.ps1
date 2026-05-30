param(
    [ValidateSet("ports", "probe", "set-target", "build", "flash", "monitor", "flash-monitor", "menuconfig", "clean", "fullclean")]
    [string]$Action = "build",

    [string]$Port = "COM4",
    [string]$BuildDir = "build\sensor",
    [string]$IDFPath = "",

    [ValidateSet("esp32s3", "esp32c3")]
    [string]$Target = "esp32s3",

    [ValidateSet("default_reset", "usb_reset", "no_reset", "no_reset_no_sync")]
    [string]$Before = "default_reset"
)

$ErrorActionPreference = "Stop"

$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ProjectDir

function Resolve-IDFPath {
    param([string]$ExplicitPath)

    $candidates = @()
    if ($ExplicitPath) { $candidates += $ExplicitPath }
    if ($env:IDF_PATH) { $candidates += $env:IDF_PATH }

    $candidates += @(
        "C:\Users\Lenovo\esp\esp-idf-v5.2.6",
        "D:\IDF_v5.2.6\v5.2.6\esp-idf",
        "D:\Espressif\frameworks\esp-idf-v5.2.6",
        "C:\Espressif\frameworks\esp-idf-v5.2.6"
    )

    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        $exportScript = Join-Path $candidate "export.ps1"
        $idfPy = Join-Path $candidate "tools\idf.py"
        if ((Test-Path $exportScript) -and (Test-Path $idfPy)) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "ESP-IDF v5.2.6 was not found. Pass -IDFPath <path-to-esp-idf>."
}

function Show-Ports {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
    if ($ports.Count -eq 0) {
        Write-Host "No serial ports found."
        return
    }

    Write-Host "Available serial ports:"
    foreach ($name in $ports) {
        Write-Host "  $name"
    }
}

function Get-IDFPython {
    $python = "python"

    if ($env:IDF_PYTHON_ENV_PATH) {
        $envPython = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
        if (Test-Path $envPython) {
            $python = $envPython
        }
    }

    return $python
}

function Invoke-Idf {
    param([string[]]$IdfArgs)

    $idfPy = Join-Path $script:ResolvedIDFPath "tools\idf.py"
    $python = Get-IDFPython

    Write-Host "idf.py $($IdfArgs -join ' ')"
    & $python $idfPy @IdfArgs
    if ($LASTEXITCODE -ne 0) {
        throw "idf.py failed with exit code $LASTEXITCODE"
    }
}

function Invoke-Esptool {
    param([string[]]$EsptoolArgs)

    $python = Get-IDFPython
    Write-Host "python -m esptool $($EsptoolArgs -join ' ')"
    & $python -m esptool @EsptoolArgs
    if ($LASTEXITCODE -ne 0) {
        throw "esptool failed with exit code $LASTEXITCODE"
    }
}

function Ensure-Target {
    $sdkconfigPath = Join-Path $ProjectDir $BuildDir
    $sdkconfigPath = Join-Path $sdkconfigPath "sdkconfig"

    $targetPattern = '^CONFIG_IDF_TARGET="' + [Regex]::Escape($Target) + '"$'
    if ((Test-Path $sdkconfigPath) -and
        (Select-String -Path $sdkconfigPath -Pattern $targetPattern -Quiet)) {
        return
    }

    Invoke-Idf ($commonArgs + @("set-target", $Target))
}

if ($Action -eq "ports") {
    Show-Ports
    return
}

$script:ResolvedIDFPath = Resolve-IDFPath -ExplicitPath $IDFPath
Write-Host "Using ESP-IDF: $script:ResolvedIDFPath"

if (($env:IDF_PATH -eq $script:ResolvedIDFPath) -and
    $env:IDF_PYTHON_ENV_PATH -and
    (Test-Path (Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"))) {
    Write-Host "ESP-IDF environment already active; skipping export.ps1"
} else {
    & (Join-Path $script:ResolvedIDFPath "export.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "ESP-IDF export.ps1 failed with exit code $LASTEXITCODE"
    }
}

$defaults = "sdkconfig.defaults;sdkconfig.defaults.sensor"
if ($Target -eq "esp32c3") {
    $defaults = "sdkconfig.defaults.sensor;sdkconfig.defaults.sensor.c3"
}

$commonArgs = @(
    "-B", $BuildDir,
    "-DSDKCONFIG=$BuildDir\sdkconfig",
    "-DSDKCONFIG_DEFAULTS=$defaults"
)

switch ($Action) {
    "probe" {
        Invoke-Esptool (@("--chip", $Target, "-p", $Port, "-b", "115200", "--before", $Before, "--after", "hard_reset", "chip_id"))
    }
    "set-target" {
        Invoke-Idf ($commonArgs + @("set-target", $Target))
    }
    "build" {
        Ensure-Target
        Invoke-Idf ($commonArgs + @("build"))
    }
    "flash" {
        Invoke-Idf ($commonArgs + @("-p", $Port, "flash"))
    }
    "monitor" {
        Invoke-Idf ($commonArgs + @("-p", $Port, "monitor"))
    }
    "flash-monitor" {
        Invoke-Idf ($commonArgs + @("-p", $Port, "flash", "monitor"))
    }
    "menuconfig" {
        Invoke-Idf ($commonArgs + @("menuconfig"))
    }
    "clean" {
        Invoke-Idf ($commonArgs + @("clean"))
    }
    "fullclean" {
        Invoke-Idf ($commonArgs + @("fullclean"))
    }
}
