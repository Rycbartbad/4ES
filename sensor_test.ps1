param(
    [ValidateSet("build", "flash", "monitor", "flash-monitor", "ports")]
    [string]$Action = "build",

    [string]$Port = $env:ESP_LEGO_PORT,
    [string]$IdfPath = $env:IDF_PATH,
    [string]$IdfToolsPath = $env:IDF_TOOLS_PATH,
    [string]$IdfPythonEnvPath = $env:IDF_PYTHON_ENV_PATH,
    [string]$BuildDir = "build\sensor"
)

$ErrorActionPreference = "Stop"

function Find-EspIdfPath {
    param([string]$Preferred)

    $candidates = @()
    if ($Preferred) {
        $candidates += $Preferred
    }

    $candidates += @(
        "$env:USERPROFILE\esp\v5.2.6\esp-idf",
        "D:\IDF_v5.2.6\v5.2.6\esp-idf",
        "C:\Espressif\frameworks\esp-idf-v5.2.6",
        "C:\Espressif\frameworks\esp-idf-v5.2",
        $env:IDF_PATH
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate "export.ps1"))) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "ESP-IDF export.ps1 was not found. Pass -IdfPath or set IDF_PATH."
}

function Find-IdfToolsPath {
    param([string]$Preferred)

    $candidates = @()
    if ($Preferred) {
        $candidates += $Preferred
    }

    $candidates += @(
        "D:\ESPIDF\IDF_5_1_2\TOOLS",
        "$env:USERPROFILE\.espressif",
        "C:\Espressif\tools",
        $env:IDF_TOOLS_PATH
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate "tools"))) {
            return (Resolve-Path $candidate).Path
        }
    }

    return $null
}

function Find-IdfPythonEnvPath {
    param(
        [string]$Preferred,
        [string]$ToolsPath
    )

    if ($Preferred -and (Test-Path $Preferred)) {
        return (Resolve-Path $Preferred).Path
    }

    if (-not $ToolsPath) {
        return $null
    }

    $pythonEnvRoot = Join-Path $ToolsPath "python_env"
    if (-not (Test-Path $pythonEnvRoot)) {
        return $null
    }

    $envDir = Get-ChildItem -Path $pythonEnvRoot -Directory -Filter "idf5.2*" |
        Sort-Object Name -Descending |
        Select-Object -First 1

    if ($envDir) {
        return $envDir.FullName
    }

    return $null
}

function Invoke-IdfPy {
    param([string[]]$IdfArgs)

    $idfCommand = Get-Command idf.py -ErrorAction SilentlyContinue
    if ($idfCommand) {
        & idf.py @IdfArgs
        if ($LASTEXITCODE -ne 0) {
            throw "idf.py failed with exit code $LASTEXITCODE."
        }
        return
    }

    $idfPy = Join-Path $script:ResolvedIdfPath "tools\idf.py"
    if (-not (Test-Path $idfPy)) {
        throw "idf.py was not found after ESP-IDF export."
    }

    & python $idfPy @IdfArgs
    if ($LASTEXITCODE -ne 0) {
        throw "idf.py failed with exit code $LASTEXITCODE."
    }
}

function Require-Port {
    if (-not $Port) {
        Write-Host "No serial port was provided."
        Write-Host "Available ports:"
        [System.IO.Ports.SerialPort]::GetPortNames() | ForEach-Object { Write-Host "  $_" }
        throw "Pass -Port COMx or set ESP_LEGO_PORT."
    }
}

if ($Action -eq "ports") {
    [System.IO.Ports.SerialPort]::GetPortNames()
    exit 0
}

$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ProjectDir

$preferredIdfPath = if ($PSBoundParameters.ContainsKey("IdfPath")) { $IdfPath } else { $null }
$preferredToolsPath = if ($PSBoundParameters.ContainsKey("IdfToolsPath")) { $IdfToolsPath } else { $null }
$preferredPythonEnvPath = if ($PSBoundParameters.ContainsKey("IdfPythonEnvPath")) { $IdfPythonEnvPath } else { $null }

$script:ResolvedIdfPath = Find-EspIdfPath -Preferred $preferredIdfPath
$resolvedToolsPath = Find-IdfToolsPath -Preferred $preferredToolsPath
$resolvedPythonEnvPath = Find-IdfPythonEnvPath -Preferred $preferredPythonEnvPath -ToolsPath $resolvedToolsPath

$env:IDF_PATH = $script:ResolvedIdfPath
$env:IDF_TARGET = "esp32s3"

if ($resolvedToolsPath) {
    $env:IDF_TOOLS_PATH = $resolvedToolsPath
}

if ($resolvedPythonEnvPath) {
    $env:IDF_PYTHON_ENV_PATH = $resolvedPythonEnvPath
}

Remove-Item Env:ESP_IDF_VERSION -ErrorAction SilentlyContinue

Write-Host "Project: $ProjectDir"
Write-Host "ESP-IDF: $script:ResolvedIdfPath"
if ($resolvedToolsPath) {
    Write-Host "Tools:   $resolvedToolsPath"
}
if ($Port) {
    Write-Host "Port:    $Port"
}

& (Join-Path $script:ResolvedIdfPath "export.ps1")

$commonArgs = @(
    "-B", $BuildDir,
    "-DSDKCONFIG=$BuildDir\sdkconfig",
    "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.sensor"
)

switch ($Action) {
    "build" {
        Invoke-IdfPy -IdfArgs ($commonArgs + @("build"))
    }
    "flash" {
        Require-Port
        Invoke-IdfPy -IdfArgs ($commonArgs + @("-p", $Port, "flash"))
    }
    "monitor" {
        Require-Port
        Invoke-IdfPy -IdfArgs ($commonArgs + @("-p", $Port, "monitor"))
    }
    "flash-monitor" {
        Require-Port
        Invoke-IdfPy -IdfArgs ($commonArgs + @("-p", $Port, "flash", "monitor"))
    }
}
