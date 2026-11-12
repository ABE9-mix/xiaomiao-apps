<#
    刷入小喵掌机固件 + 抓启动日志

    用法:
        pwsh -File tools/flash.ps1                                  # 默认刷摩斯固件到 COM6
        pwsh -File tools/flash.ps1 -Bin dist/xiaomiao-midi-merged.bin
        pwsh -File tools/flash.ps1 -Port COM7 -Monitor               # 刷完抓 10 秒串口日志

    要点：
      * merge-bin 产出的**合并固件从 0x0 整片刷**（不要用 0x1000，那是单个 bootloader 的偏移）
      * 单个 bin 的偏移才是 0x1000(bootloader) / 0x9000(分区表) / 0x20000(app)
      * USB 是 GD32F350 USB-CDC 桥，偶发断连：遇到 PermissionError / 端口消失就重插再跑
#>
param(
    [string]$Bin = 'dist/xiaomiao-morse-merged.bin',
    [string]$Port = 'COM6',
    [int]$Baud = 460800,
    [switch]$Monitor,
    [int]$MonitorSeconds = 10
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Bin)) { $Bin = Join-Path $Root $Bin }
if (-not (Test-Path $Bin)) { throw "找不到固件：$Bin" }

$f = Get-Item $Bin
Write-Host ("固件 {0}  {1:N0} B" -f $f.Name, $f.Length)
Write-Host ("sha256 {0}" -f (Get-FileHash $Bin -Algorithm SHA256).Hash)

$ports = [System.IO.Ports.SerialPort]::GetPortNames()
Write-Host ("可用串口: {0}" -f ($ports -join ', '))
if ($ports -notcontains $Port) {
    throw "$Port 不在（设备没插好或 USB 桥掉线，重插后再跑）"
}

# esptool：优先 IDF 自带环境，其次 uv tool run
$venvPython = Get-ChildItem (Join-Path $Root 'idf-tools\python_env') -Directory -ErrorAction SilentlyContinue |
              ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' } |
              Where-Object { Test-Path $_ } | Select-Object -First 1

$esptool = $null
if (Get-Command uv -ErrorAction SilentlyContinue) { $esptool = @('uv', 'tool', 'run', 'esptool') }
elseif ($venvPython) { $esptool = @($venvPython, '-m', 'esptool') }
else { throw "找不到 esptool：既没有 uv，也没有 IDF python 环境" }

$args = @() + $esptool + @(
    '--port', $Port, '--chip', 'esp32', '--baud', "$Baud",
    '--before', 'default_reset', '--after', 'hard_reset',
    'write_flash', '-z', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '4MB',
    '0x0', $Bin
)
Write-Host "--- 写入 ---"
& $args[0] $args[1..($args.Count - 1)]

if ($Monitor) {
    Start-Sleep -Seconds 2
    if ([System.IO.Ports.SerialPort]::GetPortNames() -notcontains $Port) {
        Write-Warning "$Port 刷完掉了，抓不到日志"
        return
    }
    Write-Host "--- 启动日志（$MonitorSeconds 秒）---"
    $sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
    $sp.ReadTimeout = 300
    $sp.Open()
    $sp.DtrEnable = $true; Start-Sleep -Milliseconds 120; $sp.DtrEnable = $false
    Start-Sleep -Milliseconds 100
    $sp.RtsEnable = $true; Start-Sleep -Milliseconds 150; $sp.RtsEnable = $false
    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddSeconds($MonitorSeconds)
    while ((Get-Date) -lt $deadline) {
        try { $t = $sp.ReadExisting(); if ($t) { [void]$sb.Append($t) } } catch { }
        Start-Sleep -Milliseconds 40
    }
    $sp.Close()
    $o = $sb.ToString()
    $i = $o.IndexOf('Calling app_main'); if ($i -lt 0) { $i = 0 }
    Write-Host $o.Substring($i)
    if ($o -match 'Guru Meditation') { Write-Warning '启动日志里有 Guru Meditation（崩溃）' }
}

