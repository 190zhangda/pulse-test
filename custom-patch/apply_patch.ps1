# Pulse 自定义补丁应用脚本
# 用法: powershell -ExecutionPolicy Bypass -File apply_patch.ps1 -PulseRoot <Pulse 源码根目录>
# 功能: 1) 备份将被覆盖的文件到 backup\<date>\  2) 覆盖为补丁版本
param(
    [Parameter(Mandatory=$true)][string]$PulseRoot
)
$ErrorActionPreference = 'Stop'
$patchDir = $PSScriptRoot
$filesDir = Join-Path $patchDir 'files'
if (-not (Test-Path $filesDir)) { Write-Error "patch files missing: $filesDir" }
if (-not (Test-Path (Join-Path $PulseRoot 'CMakeLists.txt'))) { Write-Error "not a pulse source root: $PulseRoot (need CMakeLists.txt)" }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupRoot = Join-Path $patchDir "backup\$stamp"
$count = 0
Get-ChildItem $filesDir -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($filesDir.Length + 1)
    $target = Join-Path $PulseRoot $rel
    $targetDir = Split-Path $target
    if (-not (Test-Path $targetDir)) { New-Item -ItemType Directory -Force -Path $targetDir | Out-Null }
    if (Test-Path $target) {
        $bk = Join-Path $backupRoot $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $bk) | Out-Null
        Copy-Item $target $bk -Force
        Write-Host "backup  $rel"
    } else {
        Write-Host "new     $rel"
    }
    Copy-Item $_.FullName $target -Force
    $count++
}
Write-Host ""
Write-Host "Applied $count files. Backup: $backupRoot"
Write-Host "Rollback: copy files from backup folder back over $PulseRoot"
