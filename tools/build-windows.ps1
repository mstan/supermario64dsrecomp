<#
Build and package the Super Mario 64 DS Recomp Windows release.

This verifies the title ROM, builds static and captured runtime title banks,
builds the reviewed framework runner and the recomp-ui launcher, then stages a
portable ZIP with tools\make_release.ps1.

Usage:
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    tools\build-windows.ps1 -Version 0.0.1
#>
param(
  [string]$Version = '0.0.1',
  [string]$CMake = 'C:\msys64\mingw64\bin\cmake.exe',
  [string]$Generator = 'Ninja',
  [int]$Jobs = 12,
  [string]$BuildDir = 'build-release',
  [string]$RunnerBuildDir = 'ndsrecomp\runner\build-sm64-native',
  [string]$RuntimeBinDir = 'C:\msys64\mingw64\bin'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cmakePath = [IO.Path]::GetFullPath($CMake)
if (-not (Test-Path -LiteralPath $cmakePath)) {
  throw "CMake not found: $cmakePath"
}

$build = [IO.Path]::GetFullPath((Join-Path $root $BuildDir))
$runnerBuild = [IO.Path]::GetFullPath((Join-Path $root $RunnerBuildDir))
$titleBankDir = [IO.Path]::GetFullPath((Join-Path $root 'generated\recomp'))
$romSha1 = '1367529f2cb23e76ef295cb1727333ae8f0a6cd7'

Push-Location $root
try {
  & $cmakePath -G $Generator -S $root -B $build `
    -DCMAKE_BUILD_TYPE=Release `
    -DNDSRECOMP_ROOT="$root\ndsrecomp" `
    "-DSM64DS_RUNNER_EXE=$runnerBuild\nds_runner.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Game CMake configure failed.' }
  & $cmakePath --build $build --target supermario64dsrecomp -j $Jobs
  if ($LASTEXITCODE -ne 0) { throw 'Game bank build failed.' }

  $recompiler = Join-Path $build 'ndsrecomp-recompiler\nds_recompile.exe'
  $frameworkGenerated = Join-Path $root 'ndsrecomp\generated'
  $freebios9 = Join-Path $root 'ndsrecomp\third_party\freebios\drastic_bios_arm9.bin'
  $freebios7 = Join-Path $root 'ndsrecomp\third_party\freebios\drastic_bios_arm7.bin'
  if (-not (Test-Path -LiteralPath $freebios9) -or
      -not (Test-Path -LiteralPath $freebios7)) {
    throw 'FreeBIOS submodule is missing. Run: git -C ndsrecomp submodule update --init third_party/freebios'
  }
  New-Item -ItemType Directory -Path $frameworkGenerated -Force | Out-Null
  & $recompiler `
    --config "$root\ndsrecomp\bios\biosnds9.toml" `
    --bin "$root\ndsrecomp\bios\biosnds9.rom" `
    --out $frameworkGenerated `
    --bank arm9_bios
  if ($LASTEXITCODE -ne 0) { throw 'ARM9 BIOS bank generation failed.' }
  & $recompiler `
    --config "$root\ndsrecomp\bios\biosnds7.toml" `
    --bin "$root\ndsrecomp\bios\biosnds7.rom" `
    --out $frameworkGenerated `
    --bank arm7_bios
  if ($LASTEXITCODE -ne 0) { throw 'ARM7 BIOS bank generation failed.' }
  & $recompiler `
    --config "$root\ndsrecomp\bios\freebios9.toml" `
    --bin $freebios9 `
    --out $frameworkGenerated `
    --bank freebios_arm9
  if ($LASTEXITCODE -ne 0) { throw 'FreeBIOS ARM9 bank generation failed.' }
  & $recompiler `
    --config "$root\ndsrecomp\bios\freebios7.toml" `
    --bin $freebios7 `
    --out $frameworkGenerated `
    --bank freebios_arm7
  if ($LASTEXITCODE -ne 0) { throw 'FreeBIOS ARM7 bank generation failed.' }

  & $cmakePath -G $Generator -S "$root\ndsrecomp\runner" -B $runnerBuild `
    -DCMAKE_BUILD_TYPE=Release `
    -DNDS_BOOTSTRAP_FIRMWARE=ON `
    "-DNDS_TITLE_BANK_DIR=$titleBankDir" `
    "-DNDS_TITLE_ROM_SHA1=$romSha1"
  if ($LASTEXITCODE -ne 0) { throw 'Runner CMake configure failed.' }
  & $cmakePath --build $runnerBuild -j $Jobs
  if ($LASTEXITCODE -ne 0) { throw 'Runner build failed.' }

  & $cmakePath --build $build --target sm64ds_launcher -j $Jobs
  if ($LASTEXITCODE -ne 0) { throw 'Launcher build failed.' }

  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    "$root\tools\make_release.ps1" `
    -Version $Version `
    -RunnerBuildDir $RunnerBuildDir `
    -BuildDir $BuildDir `
    -RuntimeBinDir $RuntimeBinDir
  if ($LASTEXITCODE -ne 0) { throw 'Release packaging failed.' }
} finally {
  Pop-Location
}
