# Packages a test build of the client into dist\<name>-<date>-<commit>\ and a zip next to it.
#
#   powershell -ExecutionPolicy Bypass -File tools\package_test_build.ps1 [-Name test-build] [-GameDir <default game folder>]
#
# The folder holds the client executable, the Visual C++ runtime DLLs it needs (app-local, as
# Microsoft allows), run.bat and a readme. No game data is included: testers point run.bat at
# their own installed copy.
param(
    [string]$Name = "test-build",
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Total Annihilation"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\client\ta_play.exe"
if (-not (Test-Path $exe)) { throw "build the client first: $exe not found" }

$commit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = (git -C $root status --porcelain --untracked-files=no)
if ($dirty) { $commit = "$commit-modified" }
$stamp = Get-Date -Format "yyyyMMdd"
$label = "$Name-$stamp-$commit"
$dist = Join-Path $root "dist"
$out = Join-Path $dist $label
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

Copy-Item $exe $out
foreach ($dll in @("MSVCP140.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll")) {
    $src = Join-Path $env:WINDIR "System32\$dll"
    if (Test-Path $src) { Copy-Item $src $out } else { Write-Warning "$dll not found; testers need the VC++ 2015-2022 x64 redistributable" }
}

$run = @"
@echo off
rem Set this to your Total Annihilation folder (the one that contains TotalA.exe):
set TA_DIR=$GameDir
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
Test build $label

Requirements
- Windows 10 or 11, 64-bit.
- Your own installed Total Annihilation 3.1 (the Steam version works). No game files are included.
- Music needs Windows Media Foundation (part of Windows; "N" editions need the Media Feature Pack).

Running
1. Open run.bat in a text editor and set TA_DIR to your Total Annihilation folder.
2. Double-click run.bat. The game starts at the main menu.

Options (add them after run.bat on a command line, e.g. run.bat --cpu)
  --cpu                 use the software renderer if the GPU path fails
  --direct              skip the menus and start a skirmish at once
  --map "Moon Quartet"  skirmish map (with --direct)
  --no-sound            no sound or music

In game: F2 (or Tab) opens the game menu.

Reporting problems
Settings are saved in settings.txt next to the executable. If something goes wrong, send the
text from the console window and describe what you were doing.

This is a development test build; some screens and features are still incomplete.
"@
Set-Content -Path (Join-Path $out "README.txt") -Value $readme -Encoding ascii

$zip = Join-Path $dist "$label.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $out "*") -DestinationPath $zip
Write-Output "folder: $out"
Write-Output "zip:    $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
