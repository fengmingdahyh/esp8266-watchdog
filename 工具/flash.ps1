<#
  WatchDog · 烧录脚本
  ------------------------------------------------------------
  用法：
    .\flash.ps1                          # 自动找 COM 口，刷固件 + 文件系统
    .\flash.ps1 -Port COMx               # 指定串口（CH340 芯片常见 COM3~COM8）
    .\flash.ps1 -FsOnly                  # 只重写网页文件（改页面后用）
    .\flash.ps1 -Bin .\固件\WatchDog.bin  # 指定固件

  说明：
    固件写 0x0，文件系统（LittleFS）写 0x300000，大小 0xFA000
    需要：python + esptool（esptool 不在 PATH 时，自行 pip install esptool pyserial）
    文件系统打包需要 mklittlefs.exe（来自 esp8266 core 的 tools/mklittlefs）
#>
[CmdletBinding()]
param(
  [string]$Port = "",
  [string]$Bin = "",
  [string]$Data = "",
  [int]$Baud = 460800,
  [int]$FsOffset = 0x300000,
  [int]$FsSize = 0xFA000,
  [switch]$FsOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Bin)  { $Bin  = Join-Path $root "固件\WatchDog.bin" }
if (-not $Data) { $Data = Join-Path $root "WatchDog\data" }
$mklf = Join-Path $PSScriptRoot "mklittlefs.exe"
if (-not (Test-Path $mklf)) { $mklf = "mklittlefs.exe" }

if (-not $FsOnly -and -not (Test-Path -LiteralPath $Bin)) {
  throw "找不到固件文件：$Bin`n请先编译固件并放入 固件\WatchDog.bin，或用 -Bin 指定路径。"
}
if (Get-Command esptool.py -ErrorAction SilentlyContinue) {
  $esptoolExe = "esptool.py"
  $esptoolPre = @()
} elseif (Get-Command esptool.exe -ErrorAction SilentlyContinue) {
  $esptoolExe = "esptool.exe"
  $esptoolPre = @()
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
  $esptoolExe = (Get-Command python).Source
  $esptoolPre = @("-m", "esptool")
} else {
  throw "找不到 esptool：pip install esptool pyserial"
}
$build = Join-Path $PSScriptRoot ".build"
New-Item -ItemType Directory -Force -Path $build | Out-Null
$img = Join-Path $build "fs.bin"

function Find-Port {
  $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -match '\((COM\d+)\)' -and $_.Name -match 'CH340|CP210|USB-SERIAL|Silicon' } |
         Select-Object -First 1
  if ($dev -and $dev.Name -match '\((COM\d+)\)') { return $Matches[1] }
  return ([System.IO.Ports.SerialPort]::GetPortNames() | Select-Object -Last 1)
}

if (-not $Port) {
  $Port = Find-Port
  if (-not $Port) { throw "没有找到串口，用 -Port COMx 指定" }
  Write-Host "串口: $Port" -ForegroundColor Cyan
}

Write-Host "`n[1/2] 打包文件系统 ..." -ForegroundColor Cyan
if (Test-Path $img) { Remove-Item $img -Force }
& $mklf -c $Data -b 8192 -p 256 -s ("0x{0:X}" -f $FsSize) $img | Out-Null
if (-not (Test-Path $img)) { throw "文件系统打包失败" }
& $mklf -l -b 8192 -p 256 -s ("0x{0:X}" -f $FsSize) $img | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }

if (-not $FsOnly) {
  Write-Host "`n[2/2] 写入固件 + 文件系统 ..." -ForegroundColor Cyan
  & $esptoolExe @esptoolPre --port $Port --baud $Baud write_flash --flash_mode dout --flash_size 4MB 0x0 $Bin ("0x{0:X}" -f $FsOffset) $img
} else {
  Write-Host "`n[2/2] 只写入文件系统 ..." -ForegroundColor Cyan
  & $esptoolExe @esptoolPre --port $Port --baud $Baud write_flash --flash_mode dout --flash_size 4MB ("0x{0:X}" -f $FsOffset) $img
}
if ($LASTEXITCODE -ne 0) { throw "烧录失败" }

Write-Host "`n完成。设备重启后：" -ForegroundColor Green
Write-Host "  · 首次没有配置 → 连接热点 WatchDog-Setup，浏览器打开 192.168.4.1 配网" -ForegroundColor Green
Write-Host "  · 配好之后 → 浏览器访问设备 IP 或 http://watchdog.local" -ForegroundColor Green
