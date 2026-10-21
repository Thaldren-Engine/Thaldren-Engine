# Packages a release of the client into dist\thaldren-engine-<version>-windows-x64\ and a zip next to it.
#
#   powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -Version v0.1.0 [-Exe <ta_play.exe>]
#
# The folder holds the client, the Visual C++ runtime DLLs it needs (app-local, as Microsoft
# allows), run.bat, the license, the third-party notices and a readme. No game data is included:
# players use their own installed copy of Total Annihilation 3.1.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Exe = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root "build\client\ta_play.exe" }
if (-not (Test-Path $Exe)) { throw "build the client first: $Exe not found" }

$commit = (git -C $root rev-parse --short HEAD).Trim()
if (git -C $root status --porcelain --untracked-files=no) { throw "the working tree has uncommitted changes; commit before packaging a release" }
$label = "thaldren-engine-$Version-windows-x64"
$dist = Join-Path $root "dist"
$out = Join-Path $dist $label
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

Copy-Item $Exe $out
foreach ($dll in @("MSVCP140.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll")) {
    $src = Join-Path $env:WINDIR "System32\$dll"
    if (Test-Path $src) { Copy-Item $src $out } else { throw "$dll not found in System32" }
}
foreach ($doc in @("LICENSE.md", "THIRD_PARTY_NOTICES.md")) { Copy-Item (Join-Path $root $doc) $out }

$run = @"
@echo off
rem Set this to your Total Annihilation folder (the one that contains TotalA.exe):
set TA_DIR=C:\Program Files (x86)\Steam\steamapps\common\Total Annihilation
if not exist "%TA_DIR%\TotalA.exe" (
  echo Total Annihilation was not found in "%TA_DIR%".
  echo Edit run.bat and set TA_DIR to your Total Annihilation folder.
  pause
  exit /b 1
)
"%~dp0ta_play.exe" "%TA_DIR%" --settings "%~dp0settings.txt" %*
if errorlevel 1 pause
"@
Set-Content -Path (Join-Path $out "run.bat") -Value $run -Encoding ascii

$readme = @"
Thaldren Engine $Version (commit $commit), Windows x64

A reimplementation of the Total Annihilation 3.1 engine. Not affiliated with or endorsed by the
owners of Total Annihilation. No game data is included.

Requirements
- Windows 10 or 11, 64-bit.
- Your own installed Total Annihilation 3.1 (the Steam release works).

Running
- Copy ta_play.exe and the three DLLs into your Total Annihilation folder (the one containing
  TotalA.exe) and run ta_play.exe there; or
- edit run.bat so TA_DIR is your Total Annihilation folder, then run run.bat.

Options (after ta_play.exe or run.bat on a command line)
  --cpu                         use the software renderer
  --no-sound                    no sound or music
  --direct --map "Moon Quartet" skip the menus and start a skirmish
The original's own switches (-d, -s, -w, -t, a language word, ...) work as in TotalA.exe.

License
PolyForm Noncommercial License 1.0.0 (LICENSE.md): noncommercial use, copying, changes and
sharing are allowed; every copy must keep the license and this line:
Required Notice: Copyright (c) 2026 Thaldren
Third-party components: THIRD_PARTY_NOTICES.md.
"@
Set-Content -Path (Join-Path $out "README.txt") -Value $readme -Encoding ascii

$zip = Join-Path $dist "$label.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $out "*") -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
Set-Content -Path "$zip.sha256" -Value "$hash  $label.zip" -Encoding ascii
Write-Output "folder: $out"
Write-Output "zip:    $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
Write-Output "sha256: $hash"
