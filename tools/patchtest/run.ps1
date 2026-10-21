# Builds patchtest.exe and runs it on a throwaway repository in %TEMP%: every way of staging, unstaging and
# discarding hunks and lines that the diff view offers, checked against git (see patchtest.cpp).
#
#   .\tools\patchtest\run.ps1 [-Cases 150]

param([int]$Cases = 150)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')

# ---------------------------------------------------------------- toolchain (as in build.ps1)
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

$out = Join-Path $root 'build\patchtest'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'patchtest.exe'
$ErrorActionPreference = 'Continue'
& cl.exe /nologo /std:c++20 /EHsc /W3 /utf-8 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /O2 /MT /GS /guard:cf `
    "/I$root\src\core" "/Fo$out\" "$PSScriptRoot\patchtest.cpp" "$root\src\core\git.cpp" "$root\src\core\text.cpp" "$root\src\core\platform_win.cpp" `
    /link "/OUT:$exe" /SUBSYSTEM:CONSOLE /DYNAMICBASE /NXCOMPAT /guard:cf /CETCOMPAT shell32.lib ole32.lib user32.lib advapi32.lib shlwapi.lib 2>&1 |
    Where-Object { $_ -match 'error|warning' } | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "patchtest build failed" }
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- throwaway repository
$repo = Join-Path $env:TEMP 'mangrove-patchtest'
if (Test-Path $repo) { Remove-Item -Recurse -Force $repo }
New-Item -ItemType Directory $repo | Out-Null
function Invoke-RepoGit { & git.exe -C $repo @args 2>&1 | Out-Null; if ($LASTEXITCODE -ne 0) { throw "git $args failed" } }
function Put([string]$name, [string]$text) { [IO.File]::WriteAllText((Join-Path $repo $name), $text, (New-Object Text.UTF8Encoding $false)) }

Invoke-RepoGit init -q -b main
Invoke-RepoGit config user.name Test
Invoke-RepoGit config user.email test@example.com
Invoke-RepoGit config core.autocrlf false

$oldA = 1..60 | ForEach-Object { 'line {0:D2}' -f $_ }
Put a.txt ($oldA -join "`n")                                  # no final line break
Put b.txt "a`nb`nc"                                           # no final line break
Put c.txt "x1`r`nx2`r`nx3`r`n"                                # CRLF
Put e.txt ((1..8 | ForEach-Object { "e$_`n" }) -join '')
Put f.txt ((1..12 | ForEach-Object { 1..8 | ForEach-Object { "rep $_`n" } }) -join '')  # the same block 12 times
Invoke-RepoGit add -A
Invoke-RepoGit commit -qm base

$newA = foreach ($i in 1..60) {
    $line = $oldA[$i - 1]
    switch ($i) {
        5 { 'new 5a'; 'new 5b'; 'new 5c' }              # 2 lines replaced by 3 (with line 6)
        { $_ -in 6, 12, 13, 14, 51, 52, 53 } { }        # removed
        20 { $line; 'ins 20a'; 'ins 20b' }              # pure insertion
        22 { 'new 22' }                                  # 1 -> 1, in the same hunk as the insertion
        40 { 'new 40a'; 'new 40b' }                     # 1 -> 2
        50 { 'new 50' }                                  # 4 -> 1 (with lines 51-53)
        60 { 'new 60' }                                  # the last line gains its line break
        default { $line }
    }
}
Put a.txt (($newA -join "`n") + "`n")
Put b.txt "a`nB`nc`nd"                                        # no final line break on either side
Put c.txt "x1`r`nX2`r`nx3`r`nx4`r`n"
Remove-Item (Join-Path $repo 'e.txt')                         # deleted file
Put f.txt ((1..12 | ForEach-Object { 1..8 | ForEach-Object { if ($_ -eq 4) { "chg 4a`nchg 4b`n" } else { "rep $_`n" } } }) -join '')
Put d.txt ((1..6 | ForEach-Object { "d$_`n" }) -join '')      # new file, staged for the unstage tests

# ---------------------------------------------------------------- run
$failed = 0
foreach ($mode in 'stage', 'discard') {
    foreach ($f in 'a.txt', 'b.txt', 'c.txt', 'e.txt', 'f.txt') {
        & $exe $repo $f $mode $Cases 7
        if ($LASTEXITCODE -ne 0) { $failed++ }
    }
}
Invoke-RepoGit add -A
foreach ($f in 'a.txt', 'b.txt', 'c.txt', 'd.txt', 'e.txt', 'f.txt') {
    & $exe $repo $f unstage $Cases 13
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
Remove-Item -Recurse -Force $repo
if ($failed) { throw "$failed patchtest runs failed" }
"patchtest: all runs passed"
