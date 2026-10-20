# Smoke check of the original's command-line words that ta_play takes (docs/parity/FRONTEND.md
# rows 9.3 and 9.5): -d / -df (the display), the debug library's own switches, the bare word (the
# language, else the setting "language", else english). Each case is a short headless run with
# --movies (a full screen would play the start's 1.zrb; a windowed display plays nothing).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/command_line_check.ps1 -Exe <ta_play.exe> -Game <game dir> [-Work <dir>]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Work = ""
)
$ErrorActionPreference = "Stop"
if ($Work -eq "") { $Work = Join-Path ([System.IO.Path]::GetTempPath()) ("command_line_" + [System.Diagnostics.Process]::GetCurrentProcess().Id) }
New-Item -ItemType Directory -Force $Work | Out-Null

function Run-Case([string]$name, [string[]]$words, [string[]]$settings) {
    $set = Join-Path $Work "$name.settings.txt"
    Set-Content -Path $set -Value (@("# settings", "PlayMovie = 0") + $settings) -Encoding ascii
    $a = @("`"$Game`"") + $words + @("--movies", "--settings", "`"$set`"", "--headless", "--no-sound", "--no-pointer", "--frames", "3")
    $log = Join-Path $Work "$name.log"
    $p = Start-Process -FilePath $Exe -ArgumentList $a -RedirectStandardOutput $log -RedirectStandardError (Join-Path $Work "$name.err") -WorkingDirectory $Work -PassThru -NoNewWindow -Wait
    return Get-Content $log
}

$ok = $true
function Expect([string]$name, [string[]]$log, [string]$language, [string]$display, [bool]$movie) {
    $line = $log | Select-String -Pattern "^language " | Select-Object -First 1
    $want = "language $language, $display display"
    $played = [bool]($log | Select-String -Pattern "sound movie")
    if (-not $line -or $line.Line -ne $want -or $played -ne $movie) {
        Write-Host "FAIL ${name}: '$($line.Line)' movie=$played (wanted '$want' movie=$movie)"
        $script:ok = $false
    } else {
        Write-Host "ok   ${name}: $want, movie=$played"
    }
}

Expect "none" (Run-Case "none" @() @()) "english" "full-screen" $true
Expect "df" (Run-Case "df" @("-df") @()) "english" "full-screen" $true
Expect "d" (Run-Case "d" @("-d") @()) "english" "windowed" $false
Expect "slashD" (Run-Case "slashD" @("/D") @()) "english" "windowed" $false
Expect "d_then_df" (Run-Case "d_then_df" @("-d", "-DF") @()) "english" "full-screen" $true
Expect "dprinton" (Run-Case "dprinton" @("-dprinton") @()) "english" "full-screen" $true
Expect "german" (Run-Case "german" @("german") @()) "german" "full-screen" $true
Expect "two_words" (Run-Case "two_words" @("german", "-d", "french") @()) "french" "windowed" $false
Expect "setting" (Run-Case "setting" @() @("language = spanish")) "spanish" "full-screen" $true
Expect "word_over_setting" (Run-Case "word_over_setting" @("italian") @("language = spanish")) "italian" "full-screen" $true
Write-Host "logs: $Work"
if (-not $ok) { exit 1 }
