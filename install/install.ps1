# solard installer for Windows -- the local server for the QuickSolar app.
#
#   In PowerShell / Windows Terminal:
#     irm https://raw.githubusercontent.com/qazi0/solard/main/install/install.ps1 | iex
#   In Command Prompt:
#     powershell -c "irm https://raw.githubusercontent.com/qazi0/solard/main/install/install.ps1 | iex"
#
# It downloads one small program into %LOCALAPPDATA%\solard, finds your GoodWe inverter
# on this network, and starts it (hidden) every time you sign in. It asks once for
# permission to let phones on your network reach it (a Windows Firewall rule).
#
# Options (set before running):  $env:SOLARD_CAPACITY = "15"   (usable battery kWh)
#                                 $env:SOLARD_HOST = "192.168.1.50"   (if not found automatically)
#                                 $env:SOLARD_AUTO_UPDATE = "0"   (don't update automatically)
# Updates: a daily hidden task checks GitHub for a newer release, verifies the download
# against the release's SHA256SUMS, swaps the program (keeping solard.prev.exe),
# restarts it and rolls back if the new one doesn't answer. Settings and history are kept.
# Turn it off with "auto_update = false" in solard.conf. Update now:
#   powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\solard\install.ps1" -Update
# Uninstall:  $env:SOLARD_UNINSTALL = "1"; irm https://raw.githubusercontent.com/qazi0/solard/main/install/install.ps1 | iex

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = if ($env:SOLARD_REPO) { $env:SOLARD_REPO } else { 'qazi0/solard' }
$base = if ($env:SOLARD_BASE_URL) { $env:SOLARD_BASE_URL } else { "https://github.com/$repo/releases/latest/download" }
$dir  = Join-Path $env:LOCALAPPDATA 'solard'
$exe  = Join-Path $dir 'solard.exe'
$conf = Join-Path $dir 'solard.conf'
$vbs  = Join-Path $dir 'start-hidden.vbs'
$port = if ($env:SOLARD_HTTP) { $env:SOLARD_HTTP } else { '8768' }
$task = 'solard'
$ps1  = Join-Path $dir 'install.ps1'
$uvbs = Join-Path $dir 'update-hidden.vbs'
$prev = Join-Path $dir 'solard.prev.exe'
$doUpdate = ($env:SOLARD_UPDATE -eq '1') -or ($args -contains '-Update')

function Stop-Solard {
    Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
    Get-Process solard -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Test-Up {                            # solard answering on its port within 20 s
    foreach ($i in 1..20) {
        try { Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 -Uri "http://127.0.0.1:$port/ping" | Out-Null; return $true } catch { Start-Sleep -Seconds 1 }
    }
    return $false
}
function Test-Newer($a, $b) {                 # is version a newer than b? ("dev" counts as oldest)
    if ($a -eq $b) { return $false }
    if (-not $b -or $b -like 'dev*') { return $true }
    if ($a -like 'dev*') { return $false }
    try { return [version]$a -gt [version]$b } catch { return $false }
}
function Get-Sum($file, $name) {              # the expected hash of $name in a SHA256SUMS file
    $line = Get-Content $file | Where-Object { ($_ -split '\s+')[1] -in @($name, "*$name") } | Select-Object -First 1
    if ($line) { return ($line -split '\s+')[0].ToLower() } else { return $null }
}
function Update-Solard {
    $log = Join-Path $dir 'update.log'
    function Log($m) { Add-Content -Path $log -Value "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m"; Write-Host $m }
    if (-not (Test-Path $exe)) { Log "solard is not installed in $dir"; return }
    if (Select-String -Path $conf -Pattern '^\s*auto_update\s*=\s*(false|0|no|off)' -Quiet) { Log "auto_update is off in $conf"; return }
    $m = Select-String -Path $conf -Pattern '^\s*http\s*=\s*(\d+)' | Select-Object -Last 1
    if ($m) { $script:port = $m.Matches[0].Groups[1].Value }
    $tag = if ($env:SOLARD_UPDATE_TAG) { $env:SOLARD_UPDATE_TAG } else { (Invoke-RestMethod -UseBasicParsing "https://api.github.com/repos/$repo/releases/latest").tag_name }
    if ($tag -notmatch '^v\d') { Log "could not read the latest release ($tag)"; return }
    $cur = (& $exe --version | Out-String).Trim(); if (-not $cur) { $cur = 'dev' }
    if (-not $env:SOLARD_UPDATE_TAG -and -not (Test-Newer $tag.Substring(1) $cur)) { Log "solard $cur is up to date"; return }
    $ub = if ($env:SOLARD_UPDATE_BASE) { $env:SOLARD_UPDATE_BASE } else { "https://github.com/$repo/releases/download/$tag" }
    $t = Join-Path $dir 'update.tmp'
    Remove-Item -Recurse -Force $t -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $t | Out-Null
    Invoke-WebRequest -UseBasicParsing -Uri "$ub/SHA256SUMS" -OutFile "$t\SHA256SUMS"
    Invoke-WebRequest -UseBasicParsing -Uri "$ub/solard-windows-x86_64.exe" -OutFile "$t\solard.exe"
    $want = Get-Sum "$t\SHA256SUMS" 'solard-windows-x86_64.exe'
    $got = (Get-FileHash -Algorithm SHA256 "$t\solard.exe").Hash.ToLower()
    if (-not $want -or $want -ne $got) { Remove-Item -Recurse -Force $t; Log 'checksum mismatch for solard-windows-x86_64.exe: not installing it'; throw 'checksum mismatch' }
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "$ub/install.ps1" -OutFile "$t\install.ps1"
        if ((Get-Sum "$t\SHA256SUMS" 'install.ps1') -eq (Get-FileHash -Algorithm SHA256 "$t\install.ps1").Hash.ToLower()) { Copy-Item -Force "$t\install.ps1" $ps1 }
    } catch { }
    Stop-Solard
    Copy-Item -Force $exe $prev
    Move-Item -Force "$t\solard.exe" $exe
    Remove-Item -Recurse -Force $t -ErrorAction SilentlyContinue
    Start-ScheduledTask -TaskName $task
    if (Test-Up) { Log "updated solard $cur -> $((& $exe --version | Out-String).Trim())" }
    else {
        Stop-Solard; Copy-Item -Force $prev $exe; Start-ScheduledTask -TaskName $task
        Log "the new solard did not start: rolled back to $cur"; throw 'rolled back'
    }
}

if ($doUpdate) { Update-Solard; return }

if ($env:SOLARD_UNINSTALL -eq '1') {
    Stop-Solard
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName 'solard-update' -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item $exe, $vbs, $uvbs, $ps1, $prev -Force -ErrorAction SilentlyContinue
    Write-Host "solard removed. Settings and history are still in $dir (delete it to remove them too)."
    Write-Host "The firewall rule 'solard (QuickSolar)' can be removed in Windows Defender Firewall."
    return
}

Write-Host "Installing solard into $dir"
New-Item -ItemType Directory -Force -Path (Join-Path $dir 'data') | Out-Null
Stop-Solard
Invoke-WebRequest -UseBasicParsing -Uri "$base/solard-windows-x86_64.exe" -OutFile "$exe.new"
Move-Item -Force "$exe.new" $exe

if (-not (Test-Path $conf)) {
    $lines = @('# solard settings (key = value). Restart solard after changes.', "http = $port")
    $lines += if ($env:SOLARD_CAPACITY) { "capacity = $($env:SOLARD_CAPACITY)" } else { '# capacity = 15      # usable battery kWh' }
    $lines += if ($env:SOLARD_HOST) { "host = $($env:SOLARD_HOST)" } else { '# host = found automatically' }
    if ($env:SOLARD_PORT) { $lines += "port = $($env:SOLARD_PORT)" }
    $lines += '# lat = 51.50       # for sunrise/sunset on the dashboard', '# lon = -0.12'
    $lines += if ($env:SOLARD_AUTO_UPDATE -eq '0') { 'auto_update = false' } else { '# auto_update = false  # stop the daily update check' }
    Set-Content -Path $conf -Value $lines -Encoding ASCII
}

if (-not (Select-String -Path $conf -Pattern '^host' -Quiet)) {
    Write-Host 'Looking for your GoodWe inverter on this network...'
    $out = & $exe --config $conf --discover 2>&1 | Out-String
    $m = [regex]::Match($out, 'found: ([0-9.]+)')
    if ($m.Success) {
        Write-Host "  found it at $($m.Groups[1].Value)"
        Add-Content -Path $conf -Value "host = $($m.Groups[1].Value)" -Encoding ASCII
    } else {
        Write-Host '  not found yet -- solard keeps looking in the background.'
        Write-Host '  (If it never shows up: set $env:SOLARD_HOST to the inverter address and run this again.)'
    }
}

# start hidden (no console window) at every sign-in, restart if it stops
Set-Content -Path $vbs -Encoding ASCII -Value @(
    'Set sh = CreateObject("WScript.Shell")',
    "sh.CurrentDirectory = ""$dir""",
    "sh.Run """"""$exe"""" --config """"$conf"""""", 0, False"
)
$action   = New-ScheduledTaskAction -Execute 'wscript.exe' -Argument "`"$vbs`"" -WorkingDirectory $dir
$trigger  = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) `
            -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $task -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $task

# daily update check (hidden): keep a copy of this installer and run it with -Update
try { Invoke-WebRequest -UseBasicParsing -Uri "$base/install.ps1" -OutFile $ps1 } catch { if ($PSCommandPath) { Copy-Item -Force $PSCommandPath $ps1 } }
if (Test-Path $ps1) {
    Set-Content -Path $uvbs -Encoding ASCII -Value @(
        'Set sh = CreateObject("WScript.Shell")',
        "sh.Run ""powershell -NoProfile -ExecutionPolicy Bypass -File $ps1 -Update"", 0, True"
    )
    $ua = New-ScheduledTaskAction -Execute 'wscript.exe' -Argument "`"$uvbs`"" -WorkingDirectory $dir
    $ut = New-ScheduledTaskTrigger -Daily -At '04:30' -RandomDelay (New-TimeSpan -Hours 2)
    $us = New-ScheduledTaskSettingsSet -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName 'solard-update' -Action $ua -Trigger $ut -Settings $us -Force | Out-Null
}

# let phones on the home network reach it (asks for permission once)
if (-not (Get-NetFirewallRule -DisplayName 'solard (QuickSolar)' -ErrorAction SilentlyContinue)) {
    Write-Host 'Allowing phones on your network to reach solard (Windows will ask for permission)...'
    $cmd = "New-NetFirewallRule -DisplayName 'solard (QuickSolar)' -Direction Inbound -Program '$exe' -Action Allow -Profile Private,Domain | Out-Null"
    try { Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', $cmd }
    catch { Write-Host '  skipped -- phones may not reach solard until you allow it in Windows Defender Firewall.' }
}

# the home-network address: the adapter with a default gateway (not a VPN adapter)
$ip = (Get-NetIPConfiguration -ErrorAction SilentlyContinue |
       Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq 'Up' -and $_.IPv4Address.IPAddress -notmatch '^(26|25|100)\.' } |
       Select-Object -First 1).IPv4Address.IPAddress
if (-not $ip) { $ip = 'this-pc' }
$up = $false
foreach ($i in 1..15) {                       # give the service a moment to start
    try { Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 -Uri "http://127.0.0.1:$port/ping" | Out-Null; $up = $true; break } catch { Start-Sleep -Seconds 1 }
}
try {
    if (-not $up) { throw 'not up' }
    Write-Host ''
    Write-Host 'solard is running, and starts every time you sign in.'
    Write-Host "  Dashboard:      http://$($ip):$port/"
    Write-Host '  Phone / TV app: open QuickSolar -> it finds this server automatically.'
    Write-Host "  Settings:       $conf"
    if ((Test-Path $ps1) -and -not (Select-String -Path $conf -Pattern '^\s*auto_update\s*=\s*(false|0|no|off)' -Quiet)) {
        Write-Host '  Updates:        checked daily (auto_update = false in the settings to turn off)'
    }
    Write-Host '  Keep this PC from sleeping (Settings -> Power) so it records all day.'
} catch {
    Write-Host "solard was installed but isn't answering yet -- try restarting the PC."
}
