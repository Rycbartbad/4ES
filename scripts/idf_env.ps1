function Initialize-IdfEnvironment {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("esp32s3", "esp32c3")]
        [string]$Chip
    )

    $script:project_root = Split-Path -Parent $PSScriptRoot
    $settings_path = Join-Path $script:project_root ".vscode\settings.json"

    $idf_path = $null
    $tools_root = $null

    if (Test-Path $settings_path) {
        $settings = Get-Content $settings_path -Raw | ConvertFrom-Json
        if ($settings.'idf.currentSetup') {
            $idf_path = [string]$settings.'idf.currentSetup'
        }
        if ($settings.'idf.toolsPathWin') {
            $tools_root = [string]$settings.'idf.toolsPathWin'
        }
    }

    if (-not $idf_path -or -not (Test-Path $idf_path)) {
        throw "ESP-IDF path not found. Set idf.currentSetup in .vscode/settings.json."
    }

    if (-not $tools_root -or -not (Test-Path $tools_root)) {
        throw "ESP-IDF tools path not found. Set idf.toolsPathWin in .vscode/settings.json."
    }

    $py_env_root = Join-Path $tools_root "python_env"
    $py_env = Get-ChildItem $py_env_root -Directory -Filter "idf5.2_py*_env" |
        Sort-Object Name -Descending |
        Select-Object -First 1

    if (-not $py_env) {
        throw "ESP-IDF Python virtualenv not found under $py_env_root"
    }

    $script:idf_path = $idf_path
    $script:tools_base = Join-Path $tools_root "tools"
    $script:py_venv = Join-Path $py_env.FullName "Scripts"
    $script:python = Join-Path $script:py_venv "python.exe"

    if (-not (Test-Path $script:python)) {
        throw "Python not found: $script:python"
    }

    $env:IDF_PATH = $script:idf_path
    $env:IDF_TOOLS_PATH = $tools_root
    $env:ESP_ROM_ELF_DIR = Join-Path $script:idf_path "components\esp_rom\$Chip"

    $envars_raw = & $script:python "$script:idf_path\tools\idf_tools.py" export --format key-value
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to export ESP-IDF environment via idf_tools.py"
    }

    $pathSeparator = [IO.Path]::PathSeparator
    foreach ($line in $envars_raw) {
        $pair = $line.Split("=", 2)
        if ($pair.Count -lt 2) { continue }

        $var_name = $pair[0].Trim()
        $var_val = $pair[1].Trim()

        if ($var_name -eq "PATH") {
            $var_val = $var_val.Trim($pathSeparator + "%PATH%")
            $env:PATH = $var_val + $pathSeparator + $env:PATH
        }
        else {
            Set-Item -Path "env:$var_name" -Value $var_val -Force
        }
    }
}
