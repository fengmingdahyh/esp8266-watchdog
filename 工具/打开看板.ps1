# 找到 WatchDog 的当前 IP 并用浏览器打开看板
# 用法： powershell -ExecutionPolicy Bypass -File .\打开看板.ps1
#        .\打开看板.ps1 -NoOpen        # 只报 IP，不开浏览器
param([switch]$NoOpen)

$ErrorActionPreference = "Continue"

# 候选地址按“最可能”排序：mDNS → ARP 里按 MAC 找 → 手动补充
$cands = New-Object System.Collections.ArrayList

try {
  Resolve-DnsName watchdog.local -ErrorAction Stop |
    Where-Object { $_.IPAddress } | ForEach-Object { [void]$cands.Add($_.IPAddress) }
} catch { }

# 如果知道设备的 MAC（印在设备标签上），填到这里可按 ARP 表定位：
# $MAC = "XX-XX-XX-XX-XX-XX"
# Get-NetNeighbor -AddressFamily IPv4 -ErrorAction SilentlyContinue |
#   Where-Object { $_.LinkLayerAddress -eq $MAC } |
#   ForEach-Object { [void]$cands.Add($_.IPAddress) }
# (arp -a) | Select-String $MAC | ForEach-Object {
#   [void]$cands.Add((($_ -split "\s+") | Where-Object { $_ -match '^\d+\.\d+\.\d+\.\d+$' } | Select-Object -First 1)) }

# 也可以把设备固定 IP 直接加进候选列表，例如：
# foreach ($ip in @("192.168.1.100")) { [void]$cands.Add($ip) }

foreach ($ip in ($cands | Where-Object { $_ } | Select-Object -Unique)) {
  try {
    $r = Invoke-WebRequest "http://$ip/api/status" -UseBasicParsing -TimeoutSec 6
    if ($r.StatusCode -eq 200) {
      $j = $r.Content | ConvertFrom-Json
      Write-Host "设备在线： http://$ip/" -ForegroundColor Green
      Write-Host ("  固件 {0} · {1} · 信号 {2} dBm · 运行 {3}" -f $j.ver, $j.ssid, $j.rssi, $j.uptime) -ForegroundColor DarkGray
      if (-not $NoOpen) { Start-Process "http://$ip/" }
      exit 0
    }
  } catch { }
}

Write-Host "没找到设备。按顺序排查：" -ForegroundColor Yellow
Write-Host "  1. 设备可能退回配网模式了：手机连热点 WatchDog-Setup，打开 http://192.168.4.1"
Write-Host "  2. 抓串口日志：复位设备后用串口工具（115200）抓 boot 日志"
Write-Host "  3. 断电重插后再跑一次（弱信号下关联要 20~40 秒才连上）"
exit 1
