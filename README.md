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

## DeepSeek local proxy for AI + ESP-NOW

Use this mode when the PC is connected to the ESP master SoftAP
(`http://192.168.4.1/`) and the ESP must still talk to ESP-NOW peers such as
`doorbell`.

Why: if the ESP master connects directly to a phone hotspot for the LLM call,
the Wi-Fi channel can move to the phone hotspot channel. ESP-NOW peers stay on
the configured SoftAP channel, so commands may stop reaching the peer. The
local proxy keeps the ESP master on the SoftAP/ESP-NOW channel and lets the PC
forward the DeepSeek request over the PC's internet connection.

Network shape:

- PC Wi-Fi connects to the ESP master AP and usually gets `192.168.4.2`.
- PC internet comes from USB tethering, Ethernet, or another route.
- ESP calls the PC proxy at `http://192.168.4.2:18082/v1`.
- The proxy forwards to `https://api.deepseek.com/chat/completions`.

Start the proxy on the PC:

```powershell
cd D:\robomaster\good_code\2027\4ES
python tools\deepseek_proxy.py --host 0.0.0.0 --port 18082
```

Optional: put the API key in the PC environment instead of the ESP web UI. The
proxy does not log the key.

```powershell
$env:DEEPSEEK_API_KEY = "your-deepseek-api-key"
python tools\deepseek_proxy.py --host 0.0.0.0 --port 18082
```

Check that the proxy is alive:

```powershell
Invoke-RestMethod http://127.0.0.1:18082/health
```

Configure the ESP web console at `http://192.168.4.1/`:

- LLM Base URL: `http://192.168.4.2:18082/v1`
- Model: `deepseek-chat`
- API Key: either save it in the web UI, or leave it empty if
  `DEEPSEEK_API_KEY` is set on the PC proxy process.

Do not rely on the ESP master connecting to the phone hotspot for this mode.
In local proxy mode the firmware detects the `http://192.168.4.x` LLM URL,
keeps STA disconnected, and sends the HTTP request through the SoftAP subnet.

Expected status after configuration:

```json
{
  "sta_connected": false,
  "llm_configured": true
}
```

Quick test:

1. Make sure the doorbell firmware is flashed as `CONFIG_SENSOR_MODULE_NAME="doorbell"`.
2. Make sure `/api/status` shows a peer named `doorbell`.
3. In the AI command box, enter `让蜂鸣器叫两声`.
4. The generated script should be `print(buzzer_beep(1,2));`.
5. The execution log should print `0`, meaning the command was sent
   successfully.

If the PC did not get `192.168.4.2`, check the ESP AP-side address:

```powershell
Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -like "192.168.4.*" } |
    Select-Object InterfaceAlias, IPAddress
```

Use that `192.168.4.x` address in the LLM Base URL.
