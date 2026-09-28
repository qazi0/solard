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
# Uninstall:  $env:SOLARD_UNINSTALL = "1"; irm https://raw.githubusercontent.com/qazi0/solard/main/install/install.ps1 | iex

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$base = if ($env:SOLARD_BASE_URL) { $env:SOLARD_BASE_URL } else { 'https://github.com/qazi0/solard/releases/latest/download' }
$dir  = Join-Path $env:LOCALAPPDATA 'solard'
$exe  = Join-Path $dir 'solard.exe'
$conf = Join-Path $dir 'solard.conf'
$vbs  = Join-Path $dir 'start-hidden.vbs'
$port = if ($env:SOLARD_HTTP) { $env:SOLARD_HTTP } else { '8768' }
$task = 'solard'

function Stop-Solard {
    Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
    Get-Process solard -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($env:SOLARD_UNINSTALL -eq '1') {
    Stop-Solard
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item $exe, $vbs -Force -ErrorAction SilentlyContinue
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

# let phones on the home network reach it (asks for permission once)
if (-not (Get-NetFirewallRule -DisplayName 'solard (QuickSolar)' -ErrorAction SilentlyContinue)) {
    Write-Host 'Allowing phones on your network to reach solard (Windows will ask for permission)...'
    $cmd = "New-NetFirewallRule -DisplayName 'solard (QuickSolar)' -Direction Inbound -Program '$exe' -Action Allow -Profile Private,Domain | Out-Null"
    try { Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', $cmd }
    catch { Write-Host '  skipped -- phones may not reach solard until you allow it in Windows Defender Firewall.' }
}

Start-Sleep -Seconds 3
$ip = (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
       Where-Object { $_.IPAddress -notmatch '^(127|169\.254)\.' -and $_.PrefixOrigin -ne 'WellKnown' } |
       Select-Object -First 1).IPAddress
try {
    Invoke-WebRequest -UseBasicParsing -TimeoutSec 3 -Uri "http://127.0.0.1:$port/ping" | Out-Null
    Write-Host ''
    Write-Host 'solard is running, and starts every time you sign in.'
    Write-Host "  Dashboard:      http://$($ip):$port/"
    Write-Host '  Phone / TV app: open QuickSolar -> it finds this server automatically.'
    Write-Host "  Settings:       $conf"
    Write-Host '  Keep this PC from sleeping (Settings -> Power) so it records all day.'
} catch {
    Write-Host "solard was installed but isn't answering yet -- try restarting the PC."
}
