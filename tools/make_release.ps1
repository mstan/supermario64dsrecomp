<#
Package a completed Super Mario 64 DS Recomp Windows release.

The ZIP contains the portable recomp-ui launcher, exact-ROM title runner,
launcher assets, configuration, documentation, and MinGW/SDL dependencies.
ROMs, BIOS/firmware, saves, raw captures, and generated source are never
staged.

Build the runner and launcher first, then run:

  powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    tools\make_release.ps1 -Version 0.0.4
#>
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [string]$RunnerBuildDir = 'ndsrecomp\runner\build-sm64-native',
  [string]$BuildDir = 'build-release',
  [string]$RuntimeBinDir = 'C:\msys64\mingw64\bin',
  [ValidateSet('SDL3', 'SDL2')]
  [string]$SdlBackend = 'SDL3'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Resolve-BuildPath([string]$path) {
  if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $root $path }
  [IO.Path]::GetFullPath($path)
}

$runnerBuild = Resolve-BuildPath $RunnerBuildDir
$build = Resolve-BuildPath $BuildDir
$runner = Join-Path $runnerBuild 'nds_runner.exe'
$launcher = Join-Path $build 'sm64ds_launcher.exe'
$assets = Join-Path $build 'assets'

foreach ($required in @($runner, $launcher, $assets)) {
  if (-not (Test-Path -LiteralPath $required)) {
    throw "Release input missing: $required"
  }
}

$projectText = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($projectText -notmatch
    "project\(SuperMario64DSRecomp VERSION $([regex]::Escape($Version)) ") {
  throw "CMake project version does not match release $Version."
}

$gameToml = Get-Content (Join-Path $root 'game.toml') -Raw
$pinMatch = [regex]::Match(
  $gameToml, '(?m)^\s*pin\s*=\s*"([0-9a-f]{40})"\s*$')
if (-not $pinMatch.Success) {
  throw 'Could not read the reviewed ndsrecomp framework pin from game.toml.'
}
$expectedFramework = $pinMatch.Groups[1].Value
$indexedFramework = (git -C $root rev-parse ':ndsrecomp').Trim()
$checkedFramework = (git -C (Join-Path $root 'ndsrecomp') rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or
    $indexedFramework -ne $expectedFramework -or
    $checkedFramework -ne $expectedFramework) {
  throw 'ndsrecomp submodule is not at the reviewed release commit.'
}

$runnerText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($runner))
foreach ($bank in @(
    'sm64ds_arm9',
    'sm64ds_arm7',
    'sm64ds_arm7_ram',
    'sm64ds_arm9_ram',
    'sm64ds_arm9_ram_gameplay')) {
  if (-not $runnerText.Contains($bank)) {
    throw "Runner does not contain the expected title bank: $bank"
  }
}
$launcherText =
  [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($launcher))
$launcherWideText =
  [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($launcher))
if (-not $launcherText.Contains('Super Mario 64 DS - Launcher') -or
    -not $launcherText.Contains('SM64DS Adaptive Widescreen') -or
    (-not $launcherText.Contains('--freebios') -and
     -not $launcherWideText.Contains('--freebios'))) {
  throw 'Launcher does not contain the expected SM64DS no-dump release identity.'
}

$out = Join-Path $root 'release-stage'
$stageName = "SuperMario64DSRecomp-windows-x64-v$Version"
$stage = Join-Path $out $stageName
$zip = Join-Path $out "$stageName.zip"
$outFull = [IO.Path]::GetFullPath($out).TrimEnd('\') + '\'
$stageFull = [IO.Path]::GetFullPath($stage)
$zipFull = [IO.Path]::GetFullPath($zip)
if (-not $stageFull.StartsWith(
      $outFull, [StringComparison]::OrdinalIgnoreCase) -or
    -not $zipFull.StartsWith(
      $outFull, [StringComparison]::OrdinalIgnoreCase)) {
  throw 'Refusing to clean release paths outside release-stage.'
}

if (Test-Path -LiteralPath $stage) {
  Remove-Item -LiteralPath $stage -Recurse -Force
}
if (Test-Path -LiteralPath $zip) {
  Remove-Item -LiteralPath $zip -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage 'bios') -Force |
  Out-Null

Copy-Item -LiteralPath $launcher `
  -Destination (Join-Path $stage 'SuperMario64DSRecomp.exe')
Copy-Item -LiteralPath $runner -Destination $stage
Copy-Item -LiteralPath $assets -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $root 'game.toml') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
# The runner links vendored melonDS, so the shipped executable is a
# GPL-3.0-or-later combined work and the release must carry the GPL text.
Copy-Item -LiteralPath (Join-Path $root 'LICENSE-GPL') -Destination $stage
if (Test-Path -LiteralPath (Join-Path $root 'docs\showcase')) {
  New-Item -ItemType Directory -Path (Join-Path $stage 'docs') -Force |
    Out-Null
  Copy-Item -LiteralPath (Join-Path $root 'docs\showcase') `
    -Destination (Join-Path $stage 'docs') -Recurse
}
Copy-Item -LiteralPath (Join-Path $root 'packaging\BIOS_README.txt') `
  -Destination (Join-Path $stage 'bios\README.txt')

$runtimeDlls = @(
  "$SdlBackend.dll",
  'libgcc_s_seh-1.dll',
  'libstdc++-6.dll',
  'libwinpthread-1.dll'
)
if ($SdlBackend -eq 'SDL3') {
  # MSYS2's SDL3 links libiconv; omitting it shipped MPH v0.6.0/v0.6.1 builds
  # that died on player machines with "libiconv-2.dll was not found" --
  # invisible on dev machines, where mingw64\bin on PATH satisfies it.
  $runtimeDlls += 'libiconv-2.dll'
}
foreach ($name in $runtimeDlls) {
  $source = Join-Path $RuntimeBinDir $name
  if (-not (Test-Path -LiteralPath $source)) {
    throw "Required runtime DLL missing: $source"
  }
  Copy-Item -LiteralPath $source -Destination $stage
}

$forbidden = @(Get-ChildItem -LiteralPath $stage -File -Recurse |
  Where-Object {
    $_.Extension -in @('.nds', '.rom', '.sav', '.bin', '.gpr') -or
    $_.Name -in @('biosnds9.rom', 'biosnds7.rom', 'firmware.bin') -or
    $_.FullName -match '[\\/]generated[\\/]'
  })
if ($forbidden.Count -ne 0) {
  throw "Release stage contains forbidden private/derived material: $($forbidden.FullName -join ', ')"
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stagePrefix = $stageFull.TrimEnd('\') + '\'
$files = @(Get-ChildItem -LiteralPath $stage -File -Recurse |
  Sort-Object FullName)
$archive = [IO.Compression.ZipFile]::Open(
  $zipFull, [IO.Compression.ZipArchiveMode]::Create)
try {
  foreach ($file in $files) {
    $fileFull = [IO.Path]::GetFullPath($file.FullName)
    if (-not $fileFull.StartsWith(
        $stagePrefix, [StringComparison]::OrdinalIgnoreCase)) {
      throw "Refusing to archive a file outside release stage: $fileFull"
    }
    $entryName = $fileFull.Substring($stagePrefix.Length).Replace('\', '/')
    if ($entryName.StartsWith('/') -or
        $entryName -match '(^|/)\.\.(/|$)') {
      throw "Unsafe ZIP entry name: $entryName"
    }
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
      $archive, $fileFull, $entryName,
      [IO.Compression.CompressionLevel]::Optimal) | Out-Null
  }
} finally {
  $archive.Dispose()
}

$archive = [IO.Compression.ZipFile]::OpenRead($zipFull)
try {
  $badEntries = @($archive.Entries | Where-Object {
    $_.FullName.Contains('\') -or $_.FullName.StartsWith('/') -or
    $_.FullName -match '(^|/)\.\.(/|$)' -or
    [IO.Path]::GetExtension($_.FullName) -in
      @('.nds', '.rom', '.sav', '.bin', '.gpr') -or
    [IO.Path]::GetFileName($_.FullName) -in
      @('biosnds9.rom', 'biosnds7.rom', 'firmware.bin')
  })
  if ($badEntries.Count -ne 0) {
    throw "ZIP contains unsafe or private entries: $($badEntries.FullName -join ', ')"
  }
  if ($archive.Entries.Count -ne $files.Count) {
    throw "ZIP entry count mismatch: expected $($files.Count), got $($archive.Entries.Count)"
  }
} finally {
  $archive.Dispose()
}

Write-Host "--- $stageName ---"
Get-ChildItem -LiteralPath $stage | Select-Object Name, Length | Out-Host
Get-FileHash -LiteralPath $zip -Algorithm SHA256 | Out-Host
