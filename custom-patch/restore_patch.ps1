# Pulse 自定义补丁还原脚本（撤销 apply_patch.ps1）
# 用法: powershell -ExecutionPolicy Bypass -File restore_patch.ps1 -PulseRoot <Pulse 源码根目录> [-Stamp <yyyyMMdd-HHmmss>]
# 不指定 -Stamp 时使用 backup 下最新一次备份。
param(
    [Parameter(Mandatory=$true)][string]$PulseRoot,
    [string]$Stamp
)
$ErrorActionPreference = 'Stop'
$patchDir = $PSScriptRoot
$backupRoot = Join-Path $patchDir 'backup'
if (-not (Test-Path $backupRoot)) { Write-Error "no backup folder: $backupRoot" }
if ($Stamp) { $use = Join-Path $backupRoot $Stamp } else { $use = Get-ChildItem $backupRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName }
if (-not (Test-Path $use)) { Write-Error "backup not found: $use" }
Write-Host "Restoring from: $use"
Get-ChildItem $use -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($use.Length + 1)
    $target = Join-Path $PulseRoot $rel
    Copy-Item $_.FullName $target -Force
    Write-Host "restore $rel"
}
Write-Host "Done."
