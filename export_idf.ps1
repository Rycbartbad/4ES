$env:IDF_PATH = "C:\Users\Fancy\esp\v5.2.6\esp-idf"
$env:IDF_TOOLS_PATH = "D:\ESPIDF\IDF_5_1_2\TOOLS"
$env:IDF_PYTHON_ENV_PATH = "D:\ESPIDF\IDF_5_1_2\TOOLS\python_env\idf5.2_py3.11_env"

if (-not (Test-Path "$env:IDF_PATH\export.ps1")) {
    Write-Error "ESP-IDF export.ps1 not found: $env:IDF_PATH\export.ps1"
    exit 1
}

& "$env:IDF_PATH\export.ps1"
