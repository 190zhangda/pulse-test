# Pulse 自定义补丁 — 全量构建脚本
# 用法: 在 Pulse 源码根目录执行
#   powershell -ExecutionPolicy Bypass -File dist\pulse-custom-patch\build_all.ps1
# 依次: 校验载荷 -> 构建全部目标 -> 验证 -> 打包便携版
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot | Split-Path | Split-Path   # dist\pulse-custom-patch -> dist -> pulse root
Set-Location $root

Write-Host '== 1/4 payload checks ==' -ForegroundColor Cyan
powershell -File scripts\check_release_payload.ps1 -BuildDir .\build
powershell -File scripts\stage_installer_runtime.ps1 -BuildDir .\build -RequireStaticRuntime

Write-Host '== 2/4 build ==' -ForegroundColor Cyan
cmd /c "call scripts\vcvars.bat >nul 2>&1 && cmake --build build --target pulse pulse_preview_test pulse_app_controllers_test pulse_dialog_bundle_test"
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host '== 3/4 verify ==' -ForegroundColor Cyan
.\build\pulse_dialog_bundle_test.exe --selftest

Write-Host '== 4/4 pack portable ==' -ForegroundColor Cyan
$ver = (Get-Content version.txt -Raw).Trim()
$outDir = "dist\Pulse-$ver-portable-win-x64"
$zipPath = "dist\Pulse-$ver-portable-win-x64.zip"
if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
python tools\pack_installer\pack_portable.py --root . --build build --version $ver --out $outDir --zip $zipPath
if ($LASTEXITCODE -ne 0) { throw "pack failed" }
Get-FileHash $zipPath -Algorithm SHA256 | Format-List
Write-Host "Done: $outDir / $zipPath"
