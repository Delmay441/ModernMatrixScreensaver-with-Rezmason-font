<#
  build.ps1 — build driver for the Windows Modern Matrix screensaver.

  Compiles windows\*.cpp + core\mmcore.c with MSVC and links a single
  ModernMatrix.scr (a Win32 PE renamed .scr). Shaders are compiled at runtime,
  so there is nothing else to ship.

  Usage:
    windows\build.ps1            # build windows\ModernMatrix.scr
    windows\build.ps1 -Test      # build + run the mmcore link test (no .scr)
    windows\build.ps1 -Run       # build, then launch the saver full-screen (/s)
    windows\build.ps1 -Configure # build, then open the config dialog (/c)
    windows\build.ps1 -Clean     # remove build artifacts

  The MSVC environment is located via vswhere and imported once per session.
#>
[CmdletBinding()]
param(
    [switch]$Test,
    [switch]$Run,
    [switch]$Configure,
    [switch]$Shot,          # build, then render a headless PNG to windows\build\shot.png
    [int]$Frames = 300,     # warmup frames for -Shot
    [switch]$Clean
)

# Note: native tools (cl, vcvars) emit harmless lines to stderr; under 'Stop' PS 5.1
# would treat those as terminating errors. We use 'Continue' + explicit exit-code
# checks (throw on real failures) instead.
$ErrorActionPreference = 'Continue'
$win   = $PSScriptRoot
$root  = Split-Path -Parent $win
$core  = Join-Path $root 'core'
$build = Join-Path $win 'build'
$scr   = Join-Path $win 'ModernMatrix.scr'

if ($Clean) {
    if (Test-Path $build) { Remove-Item -Recurse -Force $build }
    if (Test-Path $scr)   { Remove-Item -Force $scr }
    Write-Host "Cleaned." -ForegroundColor Green
    return
}

New-Item -ItemType Directory -Force -Path $build | Out-Null

# --- Locate + import the MSVC x64 build environment (once per session) ---------
if (-not $env:VSCMD_VER) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere not found. Is Visual Studio / Build Tools installed?" }
    $vsPath = & $vswhere -latest -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath
    if (-not $vsPath) { throw "No VS install with the C++ toolset found." }
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
    Write-Host "Importing MSVC environment from $vsPath ..." -ForegroundColor DarkGray
    cmd /c "`"$vcvars`" 2>nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Force "env:$($matches[1])" $matches[2] }
    }
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw "cl.exe not on PATH after vcvars import." }

$common = @('/nologo','/std:c++20','/O2','/EHsc','/W3','/MT','/DUNICODE','/D_UNICODE',
            '/D_CRT_SECURE_NO_WARNINGS', "/I$core", "/I$build", "/Fo:$build\")

# --- Link-test mode: prove the toolchain + mmcore linkage ---------------------
if ($Test) {
    $exe = Join-Path $build '_linktest.exe'
    Write-Host "Building link test ..." -ForegroundColor Cyan
    & cl @common (Join-Path $win '_linktest.cpp') (Join-Path $core 'mmcore.c') "/Fe:$exe"
    if ($LASTEXITCODE -ne 0) { throw "Link test compile failed ($LASTEXITCODE)." }
    Write-Host "`n--- running link test ---" -ForegroundColor Cyan
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Link test run failed ($LASTEXITCODE)." }
    Write-Host "`nToolchain + mmcore verified." -ForegroundColor Green
    return
}

# --- Full .scr build ----------------------------------------------------------

# Embed shaders.hlsl as a C string so the .scr is self-contained (no runtime file).
$hlsl = Get-Content (Join-Path $win 'shaders.hlsl') -Raw
$embed = 'static const char kShaderHLSL[] = R"MMHLSL(' + "`r`n" + $hlsl + "`r`n" + ')MMHLSL";' + "`r`n"
Set-Content -Path (Join-Path $build 'shaders_embed.h') -Value $embed -Encoding ascii

$cppFiles = Get-ChildItem (Join-Path $win '*.cpp') |
            Where-Object { $_.Name -ne '_linktest.cpp' } |
            Select-Object -ExpandProperty FullName
if (-not $cppFiles) { throw "No Windows source files yet (windows\*.cpp). Nothing to build." }

# Compile the resource script if present.
$resObj = $null
$rc = Join-Path $win 'mm.rc'
if (Test-Path $rc) {
    $resObj = Join-Path $build 'mm.res'
    Write-Host "Compiling resources ..." -ForegroundColor DarkGray
    & rc /nologo /I $win /fo $resObj $rc
    if ($LASTEXITCODE -ne 0) { throw "rc failed ($LASTEXITCODE)." }
}

$libs = @('d3d11.lib','dxgi.lib','d3dcompiler.lib','dwrite.lib','d2d1.lib','windowscodecs.lib',
          'user32.lib','gdi32.lib','comdlg32.lib','comctl32.lib','advapi32.lib','ole32.lib','shell32.lib')

Write-Host "Compiling + linking ModernMatrix.scr ..." -ForegroundColor Cyan
$sources = @($cppFiles) + @(Join-Path $core 'mmcore.c')
$linkArgs = @('/link','/SUBSYSTEM:WINDOWS','/MANIFEST:EMBED', "/OUT:$scr") + $libs
if ($resObj) { $sources += $resObj }
& cl @common @sources @linkArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
Write-Host "`nBuilt $scr" -ForegroundColor Green

# Dev launches run a copy named .exe (PowerShell won't direct-exec a .scr, and a
# live .scr can hold a file lock). The copy sits next to shaders.hlsl.
function Invoke-Dev([string[]]$cliArgs, [bool]$wait) {
    $exe = Join-Path $win 'mm_dev.exe'
    Get-Process mm_dev -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    Copy-Item $scr $exe -Force
    Remove-Item "$env:TEMP\ModernMatrix.log" -ErrorAction SilentlyContinue
    Start-Process -FilePath $exe -ArgumentList $cliArgs -Wait:$wait -PassThru -NoNewWindow
}

if ($Shot) {
    $png = Join-Path $build 'shot.png'
    Remove-Item $png -ErrorAction SilentlyContinue
    Write-Host "Rendering $Frames-frame headless shot ..." -ForegroundColor Cyan
    $p = Invoke-Dev @('/shot', $png, "$Frames") $true
    Write-Host ("shot exit={0}  ->  {1}  (exists={2})" -f $p.ExitCode, $png, (Test-Path $png))
    if (Test-Path "$env:TEMP\ModernMatrix.log") { Write-Host "--- log ---"; Get-Content "$env:TEMP\ModernMatrix.log" }
}
elseif ($Run)       { Write-Host "Launching /s ..." -ForegroundColor DarkGray; Invoke-Dev @('/s') $false | Out-Null }
elseif ($Configure) { Write-Host "Launching /c ..." -ForegroundColor DarkGray; Invoke-Dev @('/c') $false | Out-Null }
