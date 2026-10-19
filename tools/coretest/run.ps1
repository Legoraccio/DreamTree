# Builds coretest.exe with the shared core and runs it (see coretest.cpp). Run .\build.ps1 once first: it
# downloads the Windows SDK.
#
#   .\tools\coretest\run.ps1

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC (Desktop development with C++) not found" }
$msvcVer = (Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1).Name
$msvc = "$vs\VC\Tools\MSVC\$msvcVer"
$deps = Join-Path $root 'deps'
if (-not (Test-Path "$deps\microsoft.windows.sdk.cpp")) { throw "Run .\build.ps1 once first (it downloads the Windows SDK)" }
$sdkInc = (Get-ChildItem "$deps\microsoft.windows.sdk.cpp\c\Include" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$sdkLib = "$deps\microsoft.windows.sdk.cpp.x64\c"
$env:PATH = "$msvc\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE = "$msvc\include;$sdkInc\ucrt;$sdkInc\um;$sdkInc\shared;$sdkInc\winrt"
$env:LIB = "$msvc\lib\x64;$sdkLib\ucrt\x64;$sdkLib\um\x64"

$out = Join-Path $root 'build\coretest'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'coretest.exe'
$core = Get-ChildItem "$root\src\core\*.cpp" | Where-Object { $_.Name -ne 'platform_posix.cpp' } | ForEach-Object { $_.FullName }
$ErrorActionPreference = 'Continue'
& cl.exe /nologo /std:c++20 /EHsc /W3 /utf-8 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /O2 /MT /GS /guard:cf `
    "/I$root\src\core" "/Fo$out\" "$PSScriptRoot\coretest.cpp" @core `
    /link "/OUT:$exe" /SUBSYSTEM:CONSOLE /DYNAMICBASE /NXCOMPAT /guard:cf /CETCOMPAT shell32.lib ole32.lib user32.lib advapi32.lib shlwapi.lib 2>&1 |
    Where-Object { $_ -match 'error|warning' } | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "coretest build failed" }
$ErrorActionPreference = 'Stop'

& $exe
if ($LASTEXITCODE -ne 0) { throw "coretest failed" }
