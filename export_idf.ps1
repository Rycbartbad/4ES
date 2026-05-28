param(
    [string]$IdfPath = $env:IDF_PATH,
    [string]$IdfToolsPath = $env:IDF_TOOLS_PATH,
    [string]$IdfPythonEnvPath = $env:IDF_PYTHON_ENV_PATH
)

$ErrorActionPreference = "Stop"

$explicitIdfPath = $PSBoundParameters.ContainsKey("IdfPath")
$explicitToolsPath = $PSBoundParameters.ContainsKey("IdfToolsPath")
$explicitPythonEnvPath = $PSBoundParameters.ContainsKey("IdfPythonEnvPath")

$candidates = @()
if ($explicitIdfPath -and $IdfPath) {
    $candidates += $IdfPath
}
$candidates += @(
    "$env:USERPROFILE\esp\v5.2.6\esp-idf",
    "D:\IDF_v5.2.6\v5.2.6\esp-idf",
    "C:\Espressif\frameworks\esp-idf-v5.2.6",
    "C:\Espressif\frameworks\esp-idf-v5.2",
    $env:IDF_PATH
)

$resolvedIdfPath = $null
foreach ($candidate in $candidates) {
    if ($candidate -and (Test-Path (Join-Path $candidate "export.ps1"))) {
        $resolvedIdfPath = (Resolve-Path $candidate).Path
        break
    }
}

if (-not $resolvedIdfPath) {
    throw "ESP-IDF export.ps1 was not found. Pass -IdfPath or set IDF_PATH."
}

$toolsCandidates = @()
if ($explicitToolsPath -and $IdfToolsPath) {
    $toolsCandidates += $IdfToolsPath
}
$toolsCandidates += @(
    "D:\ESPIDF\IDF_5_1_2\TOOLS",
    "$env:USERPROFILE\.espressif",
    "C:\Espressif\tools",
    $env:IDF_TOOLS_PATH
)

$resolvedToolsPath = $null
foreach ($candidate in $toolsCandidates) {
    if ($candidate -and (Test-Path (Join-Path $candidate "tools"))) {
        $resolvedToolsPath = (Resolve-Path $candidate).Path
        break
    }
}

$resolvedPythonEnvPath = $null
if ($explicitPythonEnvPath -and $IdfPythonEnvPath -and (Test-Path $IdfPythonEnvPath)) {
    $resolvedPythonEnvPath = (Resolve-Path $IdfPythonEnvPath).Path
} elseif ($resolvedToolsPath) {
    $pythonEnvRoot = Join-Path $resolvedToolsPath "python_env"
    if (Test-Path $pythonEnvRoot) {
        $envDir = Get-ChildItem -Path $pythonEnvRoot -Directory -Filter "idf5.2*" |
            Sort-Object Name -Descending |
            Select-Object -First 1
        if ($envDir) {
            $resolvedPythonEnvPath = $envDir.FullName
        }
    }
}

$env:IDF_PATH = $resolvedIdfPath
$env:IDF_TARGET = "esp32s3"

if ($resolvedToolsPath) {
    $env:IDF_TOOLS_PATH = $resolvedToolsPath
}

if ($resolvedPythonEnvPath) {
    $env:IDF_PYTHON_ENV_PATH = $resolvedPythonEnvPath
}

Remove-Item Env:ESP_IDF_VERSION -ErrorAction SilentlyContinue

& (Join-Path $resolvedIdfPath "export.ps1")
