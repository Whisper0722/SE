# =============================================================================
# install-electron.ps1 - 手动安装 Electron 二进制
#
# 背景：npm 安装 electron 包时，postinstall 会从 GitHub 下载 ~150MB 的
# electron.exe 压缩包；网络不好时容易超时。这个脚本改为：
#   1. 从 package.json 读取 electron 版本
#   2. 从 GitHub Release 下载对应 win32-x64 zip
#   3. 解压到 node_modules/electron/dist
#   4. 写 path.txt，让 electron 包能定位到 electron.exe
#
# 前提：先安装 npm 包装（跳过二进制下载）：
#   $env:ELECTRON_SKIP_BINARY_DOWNLOAD='1'; npm install
# =============================================================================

$ErrorActionPreference = 'Stop'

# 仓库根 = scripts 的上一级；app 目录在根下。
$root = Split-Path -Parent $PSScriptRoot
$app = Join-Path $root 'app'
$pkgPath = Join-Path $app 'package.json'
# 读取 package.json 中声明的 electron 版本（去掉 ^/~ 前缀）。
$pkg = Get-Content $pkgPath -Raw | ConvertFrom-Json
$version = $pkg.devDependencies.electron
if (-not $version) {
    throw 'package.json has no devDependencies.electron'
}
$version = $version.TrimStart('^', '~')

# electron npm 包装必须已经存在（负责提供 index.js/cli.js/path.txt 机制）。
$wrapper = Join-Path $app 'node_modules\electron'
if (-not (Test-Path $wrapper)) {
    throw "Install the npm wrapper first: cd app; `$env:ELECTRON_SKIP_BINARY_DOWNLOAD='1'; npm install"
}

# 下载目录（已加入 .gitignore）。
$downloadDir = Join-Path $app '.electron-download'
New-Item -ItemType Directory -Force $downloadDir | Out-Null
$zip = Join-Path $downloadDir "electron-v$version-win32-x64.zip"

# 已下载则复用，避免重复拉 150MB。
if (-not (Test-Path $zip)) {
    $url = "https://github.com/electron/electron/releases/download/v$version/electron-v$version-win32-x64.zip"
    Write-Host "Downloading $url"
    & curl.exe -L --fail --retry 5 --retry-delay 2 --max-time 900 -o $zip $url
    if ($LASTEXITCODE -ne 0) {
        throw "download failed: $url"
    }
}

# 解压到临时目录（Windows 自带 tar 支持 zip）。
$extract = Join-Path $downloadDir 'extracted'
if (Test-Path $extract) {
    Remove-Item -Recurse -Force $extract
}
New-Item -ItemType Directory -Force $extract | Out-Null
& tar.exe -xf $zip -C $extract
if ($LASTEXITCODE -ne 0) {
    throw "extract failed: $zip"
}

# 复制到 electron 包期望的 dist/ 目录。
$dist = Join-Path $wrapper 'dist'
if (Test-Path $dist) {
    Remove-Item -Recurse -Force $dist
}
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item -Path (Join-Path $extract '*') -Destination $dist -Recurse -Force
# path.txt 告诉 electron 包的 index.js 实际可执行文件名。
Set-Content -Path (Join-Path $wrapper 'path.txt') -Value 'electron.exe' -Encoding ascii -NoNewline

Write-Host "Electron $version installed at $dist"
