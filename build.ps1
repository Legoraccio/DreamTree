# Mangrove build script for Windows (the macOS app is built by build-mac.sh).
# Compiles the shared core (src\core) and the Windows interface (src\windows) with the MSVC toolset from
# Visual Studio and the Windows SDK from the official Microsoft NuGet packages (downloaded into .\deps on
# first run, no admin needed).
#
#   .\build.ps1            release build -> bin\Mangrove.exe
#   .\build.ps1 -Dbg       debug build   -> bin\Mangrove-debug.exe
#   .\build.ps1 -Run       build and launch

param([switch]$Dbg, [switch]$Run, [string]$Src = "src", [string]$Out = "")

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$sdkVersion = '10.0.28000.2705'

# ---------------------------------------------------------------- toolchain
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC (Desktop development with C++) not found" }
$msvcVer = (Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1).Name
$msvc = "$vs\VC\Tools\MSVC\$msvcVer"

# ---------------------------------------------------------------- Windows SDK (NuGet)
$deps = Join-Path $root 'deps'
New-Item -ItemType Directory -Force $deps | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($p in 'microsoft.windows.sdk.cpp', 'microsoft.windows.sdk.cpp.x64', 'microsoft.windows.sdk.buildtools') {
    $dst = Join-Path $deps $p
    if (Test-Path $dst) { continue }
    $pkg = Join-Path $deps "$p.$sdkVersion.nupkg"
    if (-not (Test-Path $pkg)) {
        Write-Host "Downloading $p $sdkVersion ..."
        Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$p/$sdkVersion/$p.$sdkVersion.nupkg" -OutFile $pkg
    }
    [System.IO.Compression.ZipFile]::ExtractToDirectory($pkg, $dst)
}
$sdkInc = (Get-ChildItem "$deps\microsoft.windows.sdk.cpp\c\Include" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$sdkLib = "$deps\microsoft.windows.sdk.cpp.x64\c"
$sdkBin = (Get-ChildItem "$deps\microsoft.windows.sdk.buildtools\bin" -Directory | Sort-Object Name | Select-Object -Last 1).FullName + '\x64'

$env:PATH = "$msvc\bin\Hostx64\x64;$sdkBin;$env:PATH"
$env:INCLUDE = "$msvc\include;$sdkInc\ucrt;$sdkInc\um;$sdkInc\shared;$sdkInc\winrt"
$env:LIB = "$msvc\lib\x64;$sdkLib\ucrt\x64;$sdkLib\um\x64"

# ---------------------------------------------------------------- build
$cfg = if ($Dbg) { 'debug' } else { 'release' }
$srcDir = if ([IO.Path]::IsPathRooted($Src)) { $Src } else { Join-Path $root $Src }
$objDir = Join-Path $root "build\$cfg"
$binDir = Join-Path $root 'bin'
New-Item -ItemType Directory -Force $objDir, $binDir | Out-Null
if (-not $Out) { $Out = if ($Dbg) { 'Mangrove-debug.exe' } else { 'Mangrove.exe' } }
$exe = Join-Path $binDir $Out

$common = @('/nologo', '/c', '/std:c++20', '/EHsc', '/W3', '/utf-8', '/permissive-', '/Zc:__cplusplus',
            '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/MP', "/Fo$objDir\",
            "/I$srcDir\core", "/I$srcDir\windows")
# Standard exploit mitigations (stack cookies, Control Flow Guard, CET) stay on: they cost next to
# nothing and unsigned binaries without them are more likely to be rejected by Smart App Control.
if ($Dbg) { $cflags = $common + @('/Od', '/Zi', '/MTd', '/D_DEBUG', "/Fd$objDir\") }
else      { $cflags = $common + @('/O2', '/Oi', '/GL', '/Gy', '/GS', '/guard:cf', '/MT', '/DNDEBUG') }

# The core's POSIX layer is for macOS; Windows uses platform_win.cpp.
$sources = @(Get-ChildItem "$srcDir\core\*.cpp", "$srcDir\windows\*.cpp" | Where-Object { $_.Name -ne 'platform_posix.cpp' } |
             ForEach-Object { $_.FullName })
Remove-Item "$objDir\*.obj" -ErrorAction SilentlyContinue
$sw = [Diagnostics.Stopwatch]::StartNew()
# Native tools write diagnostics to stderr; rely on exit codes instead of PowerShell error records.
$ErrorActionPreference = 'Continue'
& cl.exe @cflags @sources 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "Compilation failed" }

$res = @()
if (Test-Path "$srcDir\windows\app.rc") {
    & rc.exe /nologo /fo "$objDir\app.res" "$srcDir\windows\app.rc" 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "Resource compilation failed" }
    $res = @("$objDir\app.res")
}

$libs = 'user32.lib', 'gdi32.lib', 'kernel32.lib', 'advapi32.lib', 'shell32.lib', 'ole32.lib', 'oleaut32.lib',
        'uuid.lib', 'shlwapi.lib', 'dwmapi.lib', 'uxtheme.lib', 'd2d1.lib', 'd3d11.lib', 'dxgi.lib',
        'dcomp.lib', 'dwrite.lib', 'windowscodecs.lib', 'comdlg32.lib', 'imm32.lib', 'runtimeobject.lib'
$lflags = @('/nologo', '/SUBSYSTEM:WINDOWS', '/MANIFEST:NO', '/DYNAMICBASE', '/NXCOMPAT', '/HIGHENTROPYVA', "/OUT:$exe")
if ($Dbg) { $lflags += @('/DEBUG') } else { $lflags += @('/LTCG', '/OPT:REF', '/OPT:ICF', '/guard:cf', '/CETCOMPAT') }
$objs = @(Get-ChildItem "$objDir\*.obj" | ForEach-Object { $_.FullName })
& link.exe @lflags @objs @res @libs 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "Link failed" }
$sw.Stop()
"Built {0} ({1:N0} KB) in {2:N1}s" -f $exe, ((Get-Item $exe).Length / 1KB), $sw.Elapsed.TotalSeconds

if ($Run) { Start-Process $exe }
