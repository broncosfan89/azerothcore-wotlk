param(
  [string]$InputDbc = "build/bin/Release/Data/dbc/Item.dbc",
  [string]$OutputMpq = "tools/client_patch/patch-Z.MPQ",
  [string]$SourceList = "tools/client_patch/mythic_wotlk_source_items.txt",
  [switch]$PatchInputInPlace
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

$PatchTool = Join-Path $RepoRoot "tools/patch_item_dbc.py"
$PatchedDbc = Join-Path $RepoRoot "tools/client_patch/DBFilesClient/Item.dbc"
$PayloadDir = Join-Path $RepoRoot "tools/client_patch_payload"
$PayloadDbcDir = Join-Path $PayloadDir "DBFilesClient"
$PayloadDbc = Join-Path $PayloadDbcDir "Item.dbc"
$MpqCliExe = Join-Path $RepoRoot "tools/vendor/mpqcli/build/bin/Release/mpqcli.exe"
$MpqCliRepo = Join-Path $RepoRoot "tools/vendor/mpqcli"

if (!(Test-Path $PatchTool)) {
  throw "Missing patch tool: $PatchTool"
}

if (!(Test-Path $InputDbc)) {
  throw "Input Item.dbc not found: $InputDbc"
}

if (!(Test-Path $SourceList)) {
  throw "Source item list not found: $SourceList"
}

if (!(Get-Command python -ErrorAction SilentlyContinue)) {
  throw "Python not found in PATH."
}

New-Item -ItemType Directory -Path (Split-Path $PatchedDbc -Parent) -Force | Out-Null

$patchArgs = @($PatchTool, "--input", $InputDbc, "--output", $PatchedDbc, "--source-list", $SourceList)
if ($PatchInputInPlace) {
  $patchArgs += "--in-place"
}

Write-Host "[1/4] Patching Item.dbc..."
& python @patchArgs

if ($LASTEXITCODE -ne 0) {
  throw "patch_item_dbc.py failed."
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
Copy-Item -Path $PatchedDbc -Destination $PayloadDbc -Force

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
Write-Host "Patched DBC: $PatchedDbc"
Write-Host "Patch MPQ  : $OutputMpq"
