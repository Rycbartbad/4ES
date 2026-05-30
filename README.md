# ESP32-S3 主机编译、烧录与监视命令

以下命令在 Windows PowerShell 中执行，位置为本项目根目录：

```powershell
cd D:\robomaster\good_code\2027\4ES
```

当前主机板使用 ESP32-S3 原生 USB Serial/JTAG，烧录不需要额外 UART 转接板。

## 主机固件编译

```powershell
$IDF = "C:\Users\Lenovo\esp\esp-idf-v5.2.6"
& (Join-Path $IDF "export.ps1")
$python = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"

$BuildDir = "build\master_usb"
$Defaults = "sdkconfig.defaults;sdkconfig.defaults.master"

& $python (Join-Path $IDF "tools\idf.py") `
    -B $BuildDir `
    -DSDKCONFIG="$BuildDir\sdkconfig" `
    "-DSDKCONFIG_DEFAULTS=$Defaults" `
    set-target esp32s3

& $python (Join-Path $IDF "tools\idf.py") `
    -B $BuildDir `
    -DSDKCONFIG="$BuildDir\sdkconfig" `
    "-DSDKCONFIG_DEFAULTS=$Defaults" `
    build
```

## 自动检测 ESP32-S3 COM 口

```powershell
$ESPPORT = $null
foreach ($port in ([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)) {
    Write-Host "Probe $port"
    & $python -m esptool --chip esp32s3 -p $port -b 115200 `
        --before default_reset --after no_reset chip_id
    if ($LASTEXITCODE -eq 0) {
        $ESPPORT = $port
        break
    }
}

if (-not $ESPPORT) {
    throw "No ESP32-S3 USB Serial/JTAG port found."
}

Write-Host "Using $ESPPORT"
```

端口号会随复位、bootloader/app 状态变化，以上面的自动检测结果为准。本机上一次检测到的是 `COM8`。

## 主机固件烧录

```powershell
& $python (Join-Path $IDF "tools\idf.py") `
    -B $BuildDir `
    -p $ESPPORT `
    flash
```

如果当前端口仍是 `COM8`：

```powershell
& $python (Join-Path $IDF "tools\idf.py") -B build\master_usb -p COM8 flash
```

## 主机串口监视

```powershell
& $python (Join-Path $IDF "tools\idf.py") -B build\master_usb -p $ESPPORT monitor
```

`Ctrl+]` 退出 monitor。

## 从机分支

从机代码和脚本提交在 `doorbell-sensor` 分支。需要烧录从机时先切分支：

```powershell
git switch doorbell-sensor
.\doorbell_sensor.ps1 build -BuildDir build\sensor_usb
.\doorbell_sensor.ps1 flash -BuildDir build\sensor_usb -Port COM8
```
