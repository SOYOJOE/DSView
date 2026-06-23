param(
    [string]$OutDir = "$PSScriptRoot\build.dir\deploy",
    [string]$BuildDir = "$PSScriptRoot\build.dir",
    [string]$MingwBin = "D:\Program Files\msys64\mingw64\bin",
    [string]$MingwLib = "D:\Program Files\msys64\mingw64\lib",
    [string]$MingwShare = "D:\Program Files\msys64\mingw64\share",
    [switch]$SkipPython = $false
)

$ErrorActionPreference = "Stop"

Write-Host "=== DSView Windows Deployment ===" -ForegroundColor Cyan
Write-Host "Output: $OutDir"
Write-Host ""

if (-not (Test-Path "$BuildDir\DSView.exe")) {
    Write-Error "DSView.exe not found in $BuildDir. Build first."
    exit 1
}

# Create output structure
$dirs = @(
    "$OutDir",
    "$OutDir\platforms",
    "$OutDir\styles",
    "$OutDir\imageformats",
    "$OutDir\iconengines",
    "$OutDir\res",
    "$OutDir\lang",
    "$OutDir\demo",
    "$OutDir\decoders"
)
foreach ($d in $dirs) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

# ---- DLLs ----
Write-Host "[1/6] Copying DLLs..." -ForegroundColor Yellow

$dlls = @(
    # Direct deps from DSView.exe
    "libfftw3-3.dll",
    "libgcc_s_seh-1.dll",
    "libglib-2.0-0.dll",
    "libwinpthread-1.dll",
    "libpython3.14.dll",
    "Qt5Core.dll",
    "Qt5Gui.dll",
    "Qt5Widgets.dll",
    "Qt5WinExtras.dll",
    "libstdc++-6.dll",
    "libusb-1.0.dll",
    "zlib1.dll",
    # Transitive deps
    "libintl-8.dll",
    "libpcre2-8-0.dll",
    "libpcre2-16-0.dll",
    "libdouble-conversion.dll",
    "libicuin78.dll",
    "libicuuc78.dll",
    "libicudt78.dll",
    "libzstd.dll",
    "libharfbuzz-0.dll",
    "libmd4c.dll",
    "libpng16-16.dll",
    "libiconv-2.dll",
    "libfreetype-6.dll",
    "libgraphite2.dll",
    "libbrotlidec.dll",
    "libbrotlicommon.dll",
    "libbz2-1.dll",
    # Python C extension deps
    "libffi-8.dll",
    # Qt SVG support (icons)
    "Qt5Svg.dll"
)

foreach ($dll in $dlls) {
    $src = "$MingwBin\$dll"
    if (Test-Path $src) {
        Copy-Item $src "$OutDir\" -Force
        Write-Host "  $dll"
    } else {
        Write-Warning "  MISSING: $dll"
    }
}

# ---- DSView.exe ----
Copy-Item "$BuildDir\DSView.exe" "$OutDir\" -Force
Write-Host "  DSView.exe"

# ---- Qt Plugins ----
Write-Host "[2/6] Copying Qt plugins..." -ForegroundColor Yellow

$qtPlugins = "$MingwShare\qt5\plugins"

# platforms (required)
if (Test-Path "$qtPlugins\platforms\qwindows.dll") {
    Copy-Item "$qtPlugins\platforms\qwindows.dll" "$OutDir\platforms\" -Force
    Write-Host "  platforms/qwindows.dll"
}

# styles
if (Test-Path "$qtPlugins\styles\qwindowsvistastyle.dll") {
    Copy-Item "$qtPlugins\styles\qwindowsvistastyle.dll" "$OutDir\styles\" -Force
    Write-Host "  styles/qwindowsvistastyle.dll"
}

# imageformats (optional but commonly needed)
$imgFormats = @("qgif.dll", "qicns.dll", "qico.dll", "qjpeg.dll", "qsvg.dll", "qtga.dll", "qtiff.dll", "qwbmp.dll", "qwebp.dll")
foreach ($f in $imgFormats) {
    if (Test-Path "$qtPlugins\imageformats\$f") {
        Copy-Item "$qtPlugins\imageformats\$f" "$OutDir\imageformats\" -Force
    }
}
Write-Host "  imageformats/*.dll"

# iconengines
if (Test-Path "$qtPlugins\iconengines\qsvgicon.dll") {
    Copy-Item "$qtPlugins\iconengines\qsvgicon.dll" "$OutDir\iconengines\" -Force
    Write-Host "  iconengines/qsvgicon.dll"
}

# ---- Resources ----
Write-Host "[3/6] Copying resources..." -ForegroundColor Yellow

$resDirs = @("res", "lang", "demo")
foreach ($d in $resDirs) {
    $src = "$BuildDir\$d"
    if (Test-Path $src) {
        Copy-Item "$src\*" "$OutDir\$d\" -Recurse -Force
        Write-Host "  $d/"
    } else {
        Write-Warning "  MISSING: $d/"
    }
}

# ---- Decoders ----
Write-Host "[4/6] Copying protocol decoders..." -ForegroundColor Yellow

$decSrc = "$BuildDir\decoders"
if (Test-Path $decSrc) {
    Copy-Item "$decSrc\*" "$OutDir\decoders\" -Recurse -Force
    Write-Host "  decoders/"
} else {
    Write-Warning "  MISSING: decoders/"
}

# ---- Python stdlib ----
if (-not $SkipPython) {
    Write-Host "[5/6] Copying Python stdlib..." -ForegroundColor Yellow

    $pyLib = "$MingwLib\python3.14"
    if (Test-Path $pyLib) {
        New-Item -ItemType Directory -Force -Path "$OutDir\lib" | Out-Null
        Copy-Item "$pyLib" "$OutDir\lib\" -Recurse -Force
        Write-Host "  lib/python3.14/"
    } else {
        Write-Warning "  MISSING: Python stdlib at $pyLib"
    }
} else {
    Write-Host "[5/6] Skipping Python stdlib (--SkipPython)" -ForegroundColor Yellow
}

# ---- qt.conf ----
Write-Host "[6/6] Writing qt.conf..." -ForegroundColor Yellow

@"
[Paths]
Plugins = .
"@ | Out-File -FilePath "$OutDir\qt.conf" -Encoding ASCII

# ---- Launch script ----
$launchBat = @"
@echo off
set PYTHONHOME=%~dp0
set PATH=%~dp0;%PATH%
start "" "%~dp0DSView.exe"
"@
$launchBat | Out-File -FilePath "$OutDir\run.bat" -Encoding ASCII

Write-Host ""
Write-Host "=== Deployment complete ===" -ForegroundColor Green
Write-Host "Output directory: $OutDir"
Write-Host ""
Write-Host "To run on another machine:"
Write-Host "  1. Copy the entire '$OutDir' folder"
Write-Host "  2. Double-click run.bat"
Write-Host ""
Write-Host "Or zip it:"
Write-Host "  Compress-Archive -Path '$OutDir\*' -DestinationPath 'DSView-portable.zip'"
Write-Host ""

# Size summary
$totalSize = (Get-ChildItem $OutDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host "Total size: $([math]::Round($totalSize / 1MB, 1)) MB"
