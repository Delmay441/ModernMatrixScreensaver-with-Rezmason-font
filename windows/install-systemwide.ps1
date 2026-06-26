<#
  install-systemwide.ps1 — copy ModernMatrix.scr into %WINDIR%\System32 so it shows
  up in the classic "Screen Saver Settings" dropdown for ALL users.
  Requires an ELEVATED (Run as administrator) PowerShell.

  After running, open: control desk.cpl,,@screensaver  and pick "Modern Matrix".
#>
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'ModernMatrix.scr'
if (-not (Test-Path $src)) { throw "ModernMatrix.scr not found next to this script. Build it first (build.ps1)." }
$dest = Join-Path $env:WINDIR 'System32\ModernMatrix.scr'
Copy-Item $src $dest -Force
Write-Host "Copied to $dest" -ForegroundColor Green
Write-Host "Open Screen Saver Settings and choose 'Modern Matrix':"
Write-Host "  control desk.cpl,,@screensaver"
