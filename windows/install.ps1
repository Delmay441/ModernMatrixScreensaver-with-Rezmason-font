<#
  install.ps1 — install Modern Matrix as the CURRENT USER's screen saver.
  No administrator rights required. Copies the self-contained .scr to a stable
  per-user location and points Windows at it, then applies the change immediately.

  Uninstall with uninstall.ps1. To also make it appear in the Windows "Screen Saver
  Settings" dropdown, run install-systemwide.ps1 from an elevated prompt.
#>
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'ModernMatrix.scr'
if (-not (Test-Path $src)) { throw "ModernMatrix.scr not found next to this script. Build it first (build.ps1)." }

$destDir = Join-Path $env:LOCALAPPDATA 'ModernMatrix'
New-Item -ItemType Directory -Force -Path $destDir | Out-Null
$dest = Join-Path $destDir 'ModernMatrix.scr'
Copy-Item $src $dest -Force

$key = 'HKCU:\Control Panel\Desktop'
# Preserve the user's previous screensaver so uninstall can restore it.
$prev = (Get-ItemProperty -Path $key -Name 'SCRNSAVE.EXE' -ErrorAction SilentlyContinue).'SCRNSAVE.EXE'
if ($prev -and $prev -ne $dest) {
    Set-ItemProperty -Path $key -Name 'ModernMatrixPrevSaver' -Value $prev
}
Set-ItemProperty -Path $key -Name 'SCRNSAVE.EXE' -Value $dest
Set-ItemProperty -Path $key -Name 'ScreenSaveActive' -Value '1'
$timeout = (Get-ItemProperty -Path $key -Name 'ScreenSaveTimeOut' -ErrorAction SilentlyContinue).'ScreenSaveTimeOut'
if (-not $timeout) { Set-ItemProperty -Path $key -Name 'ScreenSaveTimeOut' -Value '600' }

# Apply immediately (no logoff needed).
rundll32.exe user32.dll, UpdatePerUserSystemParameters 1, True

Write-Host "Installed Modern Matrix as your screen saver." -ForegroundColor Green
Write-Host "  File     : $dest"
Write-Host "  Activates after $([math]::Round([int]((Get-ItemProperty -Path $key -Name 'ScreenSaveTimeOut').'ScreenSaveTimeOut')/60)) min idle."
Write-Host "  Preview  : & `"$dest`" /s"
Write-Host "  Configure: & `"$dest`" /c"
Write-Host "  Uninstall: .\uninstall.ps1"
