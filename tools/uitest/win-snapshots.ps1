# Smoke test of the Windows interface: opens a throwaway repository (branches, a merge, tags, a remote, a stash
# and uncommitted changes) in bin\Mangrove.exe, which saves pictures of its windows (MANGROVE_SNAPSHOT: the
# main window and the settings window, in three themes) and quits. Fails when the app crashes, hangs or doesn't
# save them. The windows come to the front and stay on top while it runs: it is meant for CI machines.
#
#   .\tools\uitest\win-snapshots.ps1 [output folder]     (default: build\snapshots)

param([string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Out) { $Out = Join-Path $root 'build\snapshots' }
$app = Join-Path $root 'bin\Mangrove.exe'
if (-not (Test-Path $app)) { throw 'Build the app first: .\build.ps1' }
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$work = Join-Path $root 'build\uitest'  # not under %TEMP%: its short 8.3 names would show in the app
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory $work | Out-Null

# ---- the repository
$repo = Join-Path $work 'sample-app'
function g {
    $ErrorActionPreference = 'Continue'  # git writes some of its messages to stderr
    & git -C $repo -c user.name='Ada Lovelace' -c user.email=ada@example.com -c core.autocrlf=false @args 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git $args failed" }
}
function put([string]$path, [string]$text) {
    $p = Join-Path $repo $path
    New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
    [IO.File]::WriteAllText($p, $text.Replace("`r`n", "`n"))
}
& git init -q -b main $repo
put 'README.md' "# Sample app`n`nA small project used to test Mangrove.`n"
put 'src/main.c' "#include <stdio.h>`n`nint main(void) {`n    printf(`"hello\n`");`n    return 0;`n}`n"
g add -A
g commit -qm 'Initial commit'
put 'src/util.c' "int add(int a, int b) { return a + b; }`n"
g add -A
g commit -qm 'Add the util module' -m 'Small helpers shared by the commands.'
g switch -q -c feature/login
put 'src/login.c' "int login(const char* user) {`n    return user != 0;`n}`n"
g add -A
g commit -qm 'Add a login screen'
put 'src/login.c' "int login(const char* user) {`n    if (!user) return 0;`n    return 1;`n}`n"
g commit -qam 'Check the user name before logging in'
g switch -q main
put 'README.md' "# Sample app`n`nA small project used to test Mangrove.`n`n## Building`n`nRun make.`n"
g commit -qam 'Explain how to build'
g merge -q --no-ff feature/login -m "Merge branch 'feature/login'"
g tag v1.0
g switch -q -c fix/typo HEAD~1
put 'README.md' "# Sample app`n`nA small project used to try Mangrove.`n`n## Building`n`nRun make.`n"
g commit -qam 'Fix a typo in the readme'
g switch -q main
& git init -q --bare (Join-Path $work 'origin.git')
g remote add origin (Join-Path $work 'origin.git')
g push -q -u origin main feature/login
put 'src/util.c' "int add(int a, int b) { return a + b; }`nint sub(int a, int b) { return a - b; }`n"
g commit -qam 'Add sub()'
put 'src/main.c' "#include <stdio.h>`n`nint main(void) {`n    printf(`"hello, stash\n`");`n    return 0;`n}`n"
g stash push -q -m 'try another greeting'
# uncommitted changes: a modified file, part of it staged, and a new file
put 'src/main.c' "#include <stdio.h>`n#include `"util.h`"`n`nint main(void) {`n    printf(`"%d\n`", add(2, 3));`n    return 0;`n}`n"
put 'src/util.c' "int add(int a, int b) { return a + b; }`nint sub(int a, int b) { return a - b; }`nint mul(int a, int b) { return a * b; }`n"
g add src/util.c
put 'src/util.h' "int add(int a, int b);`nint sub(int a, int b);`nint mul(int a, int b);`n"

# ---- the app: an account in the list (no token: the list comes from the settings)
$settings = Join-Path $work 'settings.ini'
[IO.File]::WriteAllText($settings, "tab=$repo`r`nactive=0`r`nrecent=$repo`r`naccount=github`thttps://github.com`toctocat`tThe Octocat`r`n")
$env:MANGROVE_SETTINGS = $settings
$env:MANGROVE_SNAPSHOT = $Out
$status = 0
$p = Start-Process -FilePath $app -PassThru
$null = $p.Handle  # keeps the handle: ExitCode stays readable after WaitForExit
if (-not $p.WaitForExit(180000)) {
    Write-Host 'The app did not finish in 3 minutes.'
    $p.Kill()
    $status = 1
} elseif ($p.ExitCode -ne 0) {
    Write-Host "The app exited with code $($p.ExitCode)."
    $status = 1
}
if (Test-Path "$Out\snapshots.log") { Get-Content "$Out\snapshots.log" }
Get-ChildItem $Out | Format-Table Name, Length -AutoSize | Out-String | Write-Host
foreach ($f in '1-main', '2-settings-general', '3-settings-appearance', '4-settings-git', '5-settings-integrations',
                '6-settings-token', '7-settings-sign-in', '8-main-dark', '9-settings-dark', '10-main-mangrove',
                '11-settings-mangrove') {
    $png = Join-Path $Out "$f.png"
    if (-not (Test-Path $png) -or (Get-Item $png).Length -eq 0) {
        Write-Host "Missing $f.png"
        $status = 1
    }
}
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
exit $status
