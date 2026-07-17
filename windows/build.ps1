<#
  build.ps1 — build driver for the Windows Matrix Reflow screensaver
  (built on the Modern Matrix / mmcore engine).

  Compiles windows\*.cpp + core\mmcore.c with MSVC and links a single
  MatrixReflow.scr (a Win32 PE renamed .scr). Shaders are pre-compiled 
  using fxc.exe into C-arrays, so there is nothing else to ship and 
  no runtime compilation overhead.

  Usage:
    windows\build.ps1            # build windows\MatrixReflow.scr
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

$ErrorActionPreference = 'Continue'
$win   = $PSScriptRoot
$root  = Split-Path -Parent $win
$core  = Join-Path $root 'core'
$build = Join-Path $win 'build'
$scr   = Join-Path $win 'MatrixReflow.scr'

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

# --- Offline Shader Compilation (fxc.exe) -------------------------------------
function Convert-BinaryToCHeader([string]$BinPath, [string]$HeaderPath, [string]$ArrayName) {
    if (-not (Test-Path $BinPath)) { throw "Binary not found: $BinPath" }
    $bytes = [System.IO.File]::ReadAllBytes($BinPath)
    $sb = [System.Text.StringBuilder]::new()
    [void]$sb.AppendLine("static const unsigned char ${ArrayName}[] = {")
    for ($i = 0; $i -lt $bytes.Count; $i++) {
        if ($i % 12 -eq 0) { [void]$sb.Append("    ") }
        [void]$sb.Append(("0x{0:X2}" -f $bytes[$i]))
        if ($i -lt $bytes.Count - 1) { [void]$sb.Append(", ") }
        if ($i % 12 -eq 11 -or $i -eq $bytes.Count - 1) { [void]$sb.AppendLine() }
    }
    [void]$sb.AppendLine("};")
    Add-Content -Path $HeaderPath -Value $sb.ToString()
}

Write-Host "Compiling shaders with fxc.exe ..." -ForegroundColor DarkGray
$hlsl = Join-Path $win 'shaders.hlsl'
$blobHeader = Join-Path $build 'shaders_blob.h'
if (Test-Path $blobHeader) { Remove-Item $blobHeader }

# Define all shader entry points and targets
$shaders = @(
    @{ Entry='glyph_vertex'; Target='vs_5_0'; Name='kBlob_glyph_vertex' },
    @{ Entry='glyph_fragment'; Target='ps_5_0'; Name='kBlob_glyph_fragment' },
    @{ Entry='fullscreen_vertex'; Target='vs_5_0'; Name='kBlob_fullscreen_vertex' },
    @{ Entry='bloom_threshold'; Target='ps_5_0'; Name='kBlob_bloom_threshold' },
    @{ Entry='bloom_downsample'; Target='ps_5_0'; Name='kBlob_bloom_downsample' },
    @{ Entry='bloom_upsample'; Target='ps_5_0'; Name='kBlob_bloom_upsample' },
    @{ Entry='bloom_composite'; Target='ps_5_0'; Name='kBlob_bloom_composite' },
    @{ Entry='crt_filter'; Target='ps_5_0'; Name='kBlob_crt_filter' }
)

foreach ($s in $shaders) {
    $cso = Join-Path $build ($s.Entry + '.cso')
    & fxc /nologo /O3 /T $s.Target /E $s.Entry /Fo $cso $hlsl
    if ($LASTEXITCODE -ne 0) { throw "fxc compilation failed for $($s.Entry)." }
    Convert-BinaryToCHeader $cso $blobHeader $s.Name
}

# --- Full .scr build ----------------------------------------------------------

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

# Removed d3dcompiler.lib as it's no longer needed for runtime
$libs = @('d3d11.lib','dxgi.lib','dwrite.lib','d2d1.lib','windowscodecs.lib',
          'user32.lib','gdi32.lib','comdlg32.lib','comctl32.lib','advapi32.lib','ole32.lib','shell32.lib')

Write-Host "Compiling + linking MatrixReflow.scr ..." -ForegroundColor Cyan
$sources = @($cppFiles) + @(Join-Path $core 'mmcore.c')
$linkArgs = @('/link','/SUBSYSTEM:WINDOWS','/MANIFEST:EMBED', "/OUT:$scr") + $libs
if ($resObj) { $sources += $resObj }
& cl @common @sources @linkArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
Write-Host "`nBuilt $scr" -ForegroundColor Green

function Invoke-Dev([string[]]$cliArgs, [bool]$wait) {
    $exe = Join-Path $win 'mm_dev.exe'
    Get-Process mm_dev -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    Copy-Item $scr $exe -Force
    Remove-Item (Join-Path $win 'MatrixReflow.log') -ErrorAction SilentlyContinue
    Remove-Item "$env:TEMP\MatrixReflow.log" -ErrorAction SilentlyContinue
    Start-Process -FilePath $exe -ArgumentList $cliArgs -Wait:$wait -PassThru -NoNewWindow
}

if ($Shot) {
    $png = Join-Path $build 'shot.png'
    Remove-Item $png -ErrorAction SilentlyContinue
    Write-Host "Rendering $Frames-frame headless shot ..." -ForegroundColor Cyan
    $p = Invoke-Dev @('/shot', $png, "$Frames") $true
    Write-Host ("shot exit={0}  ->  {1}  (exists={2})" -f $p.ExitCode, $png, (Test-Path $png))
    $devLog = Join-Path $env:LOCALAPPDATA 'MatrixReflow\MatrixReflow.log'
    if (Test-Path $devLog) {
        Write-Host "--- log (this run) ---"
        $lines = Get-Content $devLog
        $markerIdx = @(0..($lines.Count - 1)) | Where-Object { $lines[$_] -match '=== session start' } | Select-Object -Last 1
        if ($null -ne $markerIdx) { $lines[$markerIdx..($lines.Count - 1)] }
        else { $lines }
    } else {
        Write-Host "--- log: not found at $devLog ---"
    }
}
elseif ($Run)       { Write-Host "Launching /s ..." -ForegroundColor DarkGray; Invoke-Dev @('/s') $false | Out-Null }
elseif ($Configure) { Write-Host "Launching /c ..." -ForegroundColor DarkGray; Invoke-Dev @('/c') $false | Out-Null }