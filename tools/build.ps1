<#
    构建小喵掌机固件（ESP-IDF v5.5.4）

    用法:
        pwsh -File tools/build.ps1 -Project morse      # 摩斯码训练器
        pwsh -File tools/build.ps1 -Project midi       # MIDI 播放器
        pwsh -File tools/build.ps1 -Project xueersi    # 上游 dashboard 工程
        pwsh -File tools/build.ps1 -Project morse -Full  # 强制全量重编（清 build）

    路径规则：默认全部相对本仓库（idf/ 与 idf-tools/），可用 -IdfPath / -ToolsPath
    或环境变量 IDF_PATH / IDF_TOOLS_PATH 覆盖。

    产物：dist/xiaomiao-<project>-merged.bin，用 tools/flash.ps1 从 0x0 整片刷入。
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('morse', 'midi', 'xueersi')]
    [string]$Project,

    [string]$IdfPath,
    [string]$ToolsPath,
    [switch]$Full
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

$IdfPath   = if ($IdfPath)   { $IdfPath }   elseif ($env:IDF_PATH)       { $env:IDF_PATH }       else { Join-Path $Root 'idf\esp-idf-v5.5.4' }
$ToolsPath = if ($ToolsPath) { $ToolsPath } elseif ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { Join-Path $Root 'idf-tools' }

$dirName  = switch ($Project) { 'morse' { 'xiaomiao-morse' } 'midi' { 'xiaomiao-midi' } 'xueersi' { 'xueersi-idf' } }
$binName  = switch ($Project) { 'morse' { 'xiaomiao-morse' } 'midi' { 'xiaomiao-midi' } 'xueersi' { 'xiaomiao' } }
$Proj     = Join-Path $Root "project\$dirName"
$Dist     = Join-Path $Root 'dist'
$Log      = Join-Path $Root "build-$Project.log"
$Tmp      = Join-Path $Root 'tmp'

if (-not (Test-Path $IdfPath))   { throw "找不到 ESP-IDF：$IdfPath（用 -IdfPath 指定，或设置 IDF_PATH）" }
if (-not (Test-Path $Proj))      { throw "找不到工程目录：$Proj" }
New-Item -ItemType Directory -Force -Path $Dist, $Tmp | Out-Null

# ---- 环境：把临时目录/组件缓存/ccache 都收进仓库，避开沙箱与权限坑 ----
$env:IDF_PATH                  = $IdfPath
$env:IDF_TOOLS_PATH            = $ToolsPath
$env:IDF_COMPONENT_REGISTRY_URL = 'https://components.espressif.com'
$env:TMP = $env:TEMP = $env:TMPDIR = $Tmp
$env:IDF_COMPONENT_CACHE_PATH  = Join-Path $Tmp 'comp-cache'
$env:CCACHE_DIR                = Join-Path $Tmp 'ccache'
$env:IDF_CCACHE_ENABLE         = '0'      # ccache 与沙箱常冲突，关掉更稳
# github.com 在部分网络被 DNS 劫持，组件下载走 dl.espressif.com 的 GitHub 镜像
if (-not $env:IDF_GITHUB_ASSETS) { $env:IDF_GITHUB_ASSETS = 'dl.espressif.com/github_assets' }
New-Item -ItemType Directory -Force -Path $env:IDF_COMPONENT_CACHE_PATH, $env:CCACHE_DIR | Out-Null

$venv = Get-ChildItem (Join-Path $ToolsPath 'python_env') -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $venv) { throw "找不到 ESP-IDF python 环境：$ToolsPath\python_env\*\Scripts\python.exe" }

# ---- 用 idf_tools.py export 拿到工具链环境（比 export.ps1 可靠）----
$kv = & $venv (Join-Path $IdfPath 'tools\idf_tools.py') export --format key-value 2>$null
foreach ($line in $kv) {
    if ($line -match '^([A-Z_]+)=(.*)$') {
        $k = $Matches[1]; $v = $Matches[2]
        if ($k -eq 'PATH') { $v = $v -replace '%PATH%', $env:PATH }
        Set-Item -Path "env:$k" -Value $v
    }
}

"=== build $dirName ===" | Set-Content $Log
Push-Location $Proj
try {
    # idf.py set-target 会清空 build 目录（全量重编约 6 分钟）。
    # 只有 build 缺失或目标不是 esp32 时才需要，其余情况走增量（约 40 秒）。
    $cache = Join-Path $Proj 'build\CMakeCache.txt'
    $needTarget = $Full -or (-not (Test-Path $cache))
    if (-not $needTarget) {
        $hit = Select-String -Path $cache -Pattern '^IDF_TARGET:' -ErrorAction SilentlyContinue
        if (-not ($hit -and $hit.Line -match 'esp32\s*$')) { $needTarget = $true }
    }
    if ($needTarget) {
        "=== set-target esp32 (清空 build，全量) ===" | Tee-Object -FilePath $Log -Append
        & $venv (Join-Path $IdfPath 'tools\idf.py') set-target esp32 2>&1 | Tee-Object -FilePath $Log -Append
    } else {
        "=== incremental build (skip set-target) ===" | Tee-Object -FilePath $Log -Append
    }
    & $venv (Join-Path $IdfPath 'tools\idf.py') build 2>&1 | Tee-Object -FilePath $Log -Append
    if ($LASTEXITCODE -ne 0) { throw "构建失败，详见 $Log" }

    $out = Join-Path $Dist "$binName-merged.bin"
    & $venv (Join-Path $IdfPath 'tools\idf.py') merge-bin -o $out 2>&1 | Tee-Object -FilePath $Log -Append
    if ($LASTEXITCODE -ne 0) { throw "merge-bin 失败，详见 $Log" }

    $f = Get-Item $out
    Write-Host ""
    Write-Host ("OK  {0}  {1:N0} B" -f $out, $f.Length) -ForegroundColor Green
    Write-Host ("sha256 {0}" -f (Get-FileHash $out -Algorithm SHA256).Hash)
    Write-Host "刷入: pwsh -File tools/flash.ps1 -Bin $out"
}
finally {
    Pop-Location
}
