param(
  [string]$InputSpellDbc = "build/bin/Release/Data/dbc/Spell.dbc",
  [string]$InputItemDbc = "tools/client_patch/DBFilesClient/Item.dbc",
  [string]$OutputMpq = "tools/client_patch/patch-Z.MPQ",
  [switch]$PatchInputInPlace
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

$PatchTool = Join-Path $RepoRoot "tools/patch_spell_dbc.py"
$PatchedSpellDbc = Join-Path $RepoRoot "tools/client_patch/DBFilesClient/Spell.dbc"
$PayloadDir = Join-Path $RepoRoot "tools/client_patch_payload"
$PayloadDbcDir = Join-Path $PayloadDir "DBFilesClient"
$PayloadSpellDbc = Join-Path $PayloadDbcDir "Spell.dbc"
$PayloadItemDbc = Join-Path $PayloadDbcDir "Item.dbc"
$MpqCliExe = Join-Path $RepoRoot "tools/vendor/mpqcli/build/bin/Release/mpqcli.exe"
$MpqCliRepo = Join-Path $RepoRoot "tools/vendor/mpqcli"

if (!(Test-Path $PatchTool)) {
  throw "Missing patch tool: $PatchTool"
}

if (!(Test-Path $InputSpellDbc)) {
  throw "Input Spell.dbc not found: $InputSpellDbc"
}

if (!(Get-Command python -ErrorAction SilentlyContinue)) {
  throw "Python not found in PATH."
}

New-Item -ItemType Directory -Path (Split-Path $PatchedSpellDbc -Parent) -Force | Out-Null

$patchArgs = @($PatchTool, "--input", $InputSpellDbc, "--output", $PatchedSpellDbc)
if ($PatchInputInPlace) {
  $patchArgs += "--in-place"
}

Write-Host "[1/4] Patching Spell.dbc..."
& python @patchArgs
if ($LASTEXITCODE -ne 0) {
  throw "patch_spell_dbc.py failed."
}

if (!(Test-Path $MpqCliExe)) {
  Write-Host "[2/4] mpqcli not found; installing/building under tools/vendor/mpqcli..."

  if (!(Test-Path $MpqCliRepo)) {
    & git clone --recursive https://github.com/TheGrayDot/mpqcli.git $MpqCliRepo
    if ($LASTEXITCODE -ne 0) { throw "git clone mpqcli failed." }
  } else {
    Write-Host "      Existing repo found: $MpqCliRepo"
  }

  Push-Location $MpqCliRepo
  & cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  if ($LASTEXITCODE -ne 0) { Pop-Location; throw "cmake configure failed for mpqcli." }
  & cmake --build build --config Release
  if ($LASTEXITCODE -ne 0) { Pop-Location; throw "cmake build failed for mpqcli." }
  Pop-Location
} else {
  Write-Host "[2/4] mpqcli found."
}

if (!(Test-Path $MpqCliExe)) {
  throw "mpqcli executable still missing: $MpqCliExe"
}

Write-Host "[3/4] Preparing payload..."
New-Item -ItemType Directory -Path $PayloadDbcDir -Force | Out-Null
Copy-Item -Path $PatchedSpellDbc -Destination $PayloadSpellDbc -Force

if (Test-Path $InputItemDbc) {
  Copy-Item -Path $InputItemDbc -Destination $PayloadItemDbc -Force
}

New-Item -ItemType Directory -Path (Split-Path $OutputMpq -Parent) -Force | Out-Null
if (Test-Path $OutputMpq) {
  Remove-Item -Path $OutputMpq -Force
}

Write-Host "[4/4] Building MPQ patch..."
& $MpqCliExe create $PayloadDir -o $OutputMpq -g wow-wotlk
if ($LASTEXITCODE -ne 0) {
  throw "mpqcli create failed."
}

Write-Host "Verifying MPQ contents..."
& $MpqCliExe list $OutputMpq
if ($LASTEXITCODE -ne 0) {
  throw "mpqcli list failed."
}

Write-Host ""
Write-Host "Done."
Write-Host "Patched Spell DBC: $PatchedSpellDbc"
Write-Host "Patch MPQ        : $OutputMpq"
