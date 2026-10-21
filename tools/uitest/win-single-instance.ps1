# Single instance on Windows: a second start of bin\Mangrove.exe with the same settings hands the folder on its
# command line to the first one, which opens it in a tab, and quits at once. The windows come to the front: it is
# meant for CI machines.
#
#   .\tools\uitest\win-single-instance.ps1

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$app = Join-Path $root 'bin\Mangrove.exe'
if (-not (Test-Path $app)) { throw 'Build the app first: .\build.ps1' }
$work = Join-Path $root 'build\uitest-instance'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory $work | Out-Null

function New-Repo([string]$name) {
    $dir = Join-Path $work $name
    $ErrorActionPreference = 'Continue'  # git writes some of its messages to stderr
    & git init -q -b main $dir 2>&1 | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir 'README.md'), "# $name`n")
    & git -C $dir add -A 2>&1 | Out-Null
    & git -C $dir -c user.name=Test -c user.email=test@example.com commit -qm 'First commit' 2>&1 | Out-Null
    return $dir
}
$first = New-Repo 'first'
$second = New-Repo 'second'
$settings = Join-Path $work 'settings.ini'
[IO.File]::WriteAllText($settings, "tab=$first`r`nactive=0`r`n")
$env:MANGROVE_SETTINGS = $settings  # its own instance: a Mangrove already open on this machine stays out of it

$status = 0
$p1 = Start-Process -FilePath $app -PassThru
$null = $p1.Handle  # keeps the handle: ExitCode stays readable
for ($i = 0; $i -lt 100 -and $p1.MainWindowHandle -eq 0; $i++) {
    Start-Sleep -Milliseconds 200
    $p1.Refresh()
}
if ($p1.MainWindowHandle -eq 0) {
    Write-Host 'The first instance shows no window.'
    $status = 1
}

# A second start with a folder: it hands the folder over and quits.
$p2 = Start-Process -FilePath $app -ArgumentList "`"$second`"" -PassThru
$null = $p2.Handle
if (-not $p2.WaitForExit(15000)) {
    Write-Host 'The second start did not quit.'
    $p2.Kill()
    $status = 1
} elseif ($p2.ExitCode -ne 0) {
    Write-Host "The second start exited with code $($p2.ExitCode)."
    $status = 1
}
if ($p1.HasExited) {
    Write-Host 'The first instance quit.'
    $status = 1
} else {
    Start-Sleep -Seconds 2
    $null = $p1.CloseMainWindow()  # it saves its tabs as it closes
    if (-not $p1.WaitForExit(15000)) {
        Write-Host 'The first instance did not close.'
        $p1.Kill()
        $status = 1
    }
}

$saved = Get-Content $settings -Raw
if ($saved -match [regex]::Escape("tab=$second")) {
    Write-Host 'The second start handed its folder to the first instance.'
} else {
    Write-Host "The folder of the second start didn't open in the first instance. Settings:"
    Write-Host $saved
    $status = 1
}
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
exit $status
