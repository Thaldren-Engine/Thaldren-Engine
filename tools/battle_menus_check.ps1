# Regression check: every in-battle menu path (tab bar, game menu, options pages with their
# buttons, exit menu and its question, save / load, chat line; opened and closed by Tab, F2, Esc
# and the buttons) leaves the battle running, in a skirmish and in a network game (two headless
# clients on 127.0.0.1). A battle is left only by the original's own ways (Exit to Menu -> Yes,
# the end of the game, a lost connection), none of which the scripts take.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/battle_menus_check.ps1 -Exe <ta_play.exe> -Game <game dir> [-Work <dir>]
# Needs an installed copy of the original game. Takes about two minutes (the network game runs
# in real time).
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Work = ""
)
$ErrorActionPreference = "Stop"
$scripts = Join-Path (Split-Path -Parent $PSScriptRoot) "ta_compat\tests\fixtures\client"
if ($Work -eq "") { $Work = Join-Path ([System.IO.Path]::GetTempPath()) ("battle_menus_" + [System.Diagnostics.Process]::GetCurrentProcess().Id) }
New-Item -ItemType Directory -Force $Work | Out-Null

function Start-Client([string]$name, [string]$script, [int]$frames, [string[]]$settings, [bool]$realtime) {
    $set = Join-Path $Work "$name.settings.txt"
    Set-Content -Path $set -Value (@("# settings") + $settings) -Encoding ascii
    $a = @("`"$Game`"", "--settings", "`"$set`"", "--headless", "--no-sound", "--no-pointer", "--frames", $frames, "--script", "`"$(Join-Path $scripts $script)`"")
    if ($realtime) { $a += "--realtime" }
    return Start-Process -FilePath $Exe -ArgumentList $a -RedirectStandardOutput (Join-Path $Work "$name.log") -RedirectStandardError (Join-Path $Work "$name.err") -WorkingDirectory $Work -PassThru -NoNewWindow
}

function Test-Client([string]$name) {
    $log = Get-Content (Join-Path $Work "$name.log")
    $left = $log | Select-String -Pattern "battle left"
    $started = $log | Select-String -Pattern "unit types: .* loaded"
    $end = $log | Select-String -Pattern "^battle running, 0 screens over it$"
    if (-not $started) { Write-Host "FAIL ${name}: the battle did not start"; return $false }
    if ($left) { Write-Host "FAIL ${name}: $($left[0].Line)"; return $false }
    if (-not $end) { Write-Host "FAIL ${name}: $($log | Select-Object -Last 1)"; return $false }
    Write-Host "ok   ${name}: battle running, no menu over it"
    return $true
}

$ok = $true
$p = Start-Client "skirmish" "battle_menus_skirmish.txt" 950 @() $false
$p.WaitForExit()
if (-not (Test-Client "skirmish")) { $ok = $false }

$h = Start-Client "net_host" "battle_menus_net_host.txt" 2050 @("NetLoopback = 1", "Nickname = Alice", "TCPADDR = ") $true
Start-Sleep -Milliseconds 500
$j = Start-Client "net_join" "battle_menus_net_join.txt" 2050 @("NetLoopback = 1", "Nickname = Bob", "TCPADDR = 127.0.0.1") $true
$h.WaitForExit()
$j.WaitForExit()
if (-not (Test-Client "net_host")) { $ok = $false }
if (-not (Test-Client "net_join")) { $ok = $false }

Write-Output "logs: $Work"
if ($ok) { exit 0 } else { exit 1 }
