<#
Build and package the Super Mario 64 DS Recomp Windows release.

This verifies the title ROM, builds static and captured runtime title banks,
builds the reviewed framework runner and the recomp-ui launcher, then stages a
portable ZIP with tools\make_release.ps1.

Usage:
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    tools\build-windows.ps1 -Version 0.0.4
#>
param(
  [string]$Version = '0.0.4',
  [string]$CMake = 'C:\msys64\mingw64\bin\cmake.exe',
  [string]$Generator = 'Ninja',
  [int]$Jobs = 12,
  [string]$BuildDir = 'build-release',
  [string]$RunnerBuildDir = 'ndsrecomp\runner\build-sm64-native',
  [string]$RuntimeBinDir = 'C:\msys64\mingw64\bin',
  [string]$BankCCompiler,
  [string]$PrebuiltBankArchive,
  [string]$PrebuiltBankArchiveSha256,
  [ValidateSet('SDL3', 'SDL2')]
  [string]$SdlBackend = 'SDL3',
  [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cmakePath = [IO.Path]::GetFullPath($CMake)
if (-not (Test-Path -LiteralPath $cmakePath)) {
  throw "CMake not found: $cmakePath"
}

function Resolve-BuildPath([string]$path) {
  if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $root $path }
  [IO.Path]::GetFullPath($path)
}

$build = Resolve-BuildPath $BuildDir
$runnerBuild = Resolve-BuildPath $RunnerBuildDir
$titleBankDir = [IO.Path]::GetFullPath((Join-Path $root 'generated\recomp'))
$frameworkGenerated = Join-Path $build 'generated-bios'

if ([bool]$PrebuiltBankArchive -ne [bool]$PrebuiltBankArchiveSha256) {
  throw 'Supply both the prebuilt bank archive and its expected SHA-256.'
}
if ($PrebuiltBankArchive) {
  $PrebuiltBankArchive = Resolve-BuildPath $PrebuiltBankArchive
}

function Update-BiosBank([string]$name, [string]$config, [string]$image) {
  $inputs = Get-Item -LiteralPath $recompiler, $config, $image
  $newest = ($inputs | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
  $outputs = @(
    "$frameworkGenerated\$name.c",
    "$frameworkGenerated\${name}_dispatch.c",
    "$frameworkGenerated\$name.h"
  )
  $outdated = $outputs | Where-Object {
    -not (Test-Path -LiteralPath $_ -PathType Leaf) -or
      (Get-Item -LiteralPath $_).LastWriteTimeUtc -lt $newest
  }
  if (-not $outdated) { return }
  & $recompiler --config $config --bin $image --out $frameworkGenerated --bank $name
  if ($LASTEXITCODE -ne 0) { throw "Bank generation failed for $name." }
}

Push-Location $root
try {
  & $cmakePath -G $Generator -S $root -B $build `
    -DCMAKE_BUILD_TYPE=Release `
    -DNDSRECOMP_ROOT="$root\ndsrecomp" `
    "-DSM64DS_LAUNCHER_SDL_BACKEND=$SdlBackend" `
    -DSM64DS_BUILD_LAUNCHER=ON
  if ($LASTEXITCODE -ne 0) { throw 'Game CMake configure failed.' }
  & $cmakePath --build $build --target sm64ds_generate_banks sm64ds_launcher -j $Jobs
  if ($LASTEXITCODE -ne 0) { throw 'Game bank generation or launcher build failed.' }

  $recompiler = Join-Path $build 'ndsrecomp-recompiler\nds_recompile.exe'
  $freebios9 = Join-Path $root 'ndsrecomp\third_party\freebios\drastic_bios_arm9.bin'
  $freebios7 = Join-Path $root 'ndsrecomp\third_party\freebios\drastic_bios_arm7.bin'
  if (-not (Test-Path -LiteralPath $freebios9) -or
      -not (Test-Path -LiteralPath $freebios7)) {
    throw 'FreeBIOS submodule is missing. Run: git -C ndsrecomp submodule update --init third_party/freebios'
  }
  New-Item -ItemType Directory -Path $frameworkGenerated -Force | Out-Null
  Update-BiosBank 'arm9_bios' "$root\ndsrecomp\bios\biosnds9.toml" "$root\ndsrecomp\bios\biosnds9.rom"
  Update-BiosBank 'arm7_bios' "$root\ndsrecomp\bios\biosnds7.toml" "$root\ndsrecomp\bios\biosnds7.rom"
  Update-BiosBank 'freebios_arm9' "$root\ndsrecomp\bios\freebios9.toml" $freebios9
  Update-BiosBank 'freebios_arm7' "$root\ndsrecomp\bios\freebios7.toml" $freebios7

  $runnerArgs = @(
    '-G', $Generator, '-S', "$root\ndsrecomp\runner",
    '-DCMAKE_BUILD_TYPE=Release', '-DNDS_BOOTSTRAP_FIRMWARE=ON',
    '-DNDS_3D_GEOMETRY=NATIVE', "-DNDS_SDL_BACKEND=$SdlBackend",
    "-DNDS_RECOMP_UI_ROOT=$root\third_party\recomp-ui",
    "-DCMAKE_PROJECT_nds_runner_INCLUDE=$root\cmake\runner-ui.cmake",
    "-DNDS_GENERATED_DIR=$frameworkGenerated",
    "-DNDS_SM64DS_BANK_DIR=$titleBankDir"
  )
  if (-not $PrebuiltBankArchive) {
    if (-not $BankCCompiler) {
      $clang = Get-Command clang.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
      if ($clang -and ((& $clang.Source -dumpmachine) -match '^x86_64-.*(windows-gnu|mingw32)$')) {
        $BankCCompiler = $clang.Source
      } else {
        $BankCCompiler = Join-Path $RuntimeBinDir 'gcc.exe'
      }
    }
    $BankCCompiler = (Get-Command $BankCCompiler -CommandType Application).Source
    if ((& $BankCCompiler -dumpmachine) -notmatch '^x86_64-.*(windows-gnu|mingw32)$') {
      throw 'The bank compiler must target Windows x64 with the GNU ABI.'
    }
    $compilerName = [IO.Path]::GetFileNameWithoutExtension($BankCCompiler)
    $bankBuild = Join-Path $build "nds-banks-$compilerName"
    Write-Host "Building or reusing the title bank archive with $BankCCompiler"
    & $cmakePath @runnerArgs -B $bankBuild `
      "-DCMAKE_C_COMPILER=$BankCCompiler" `
      -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    if ($LASTEXITCODE -ne 0) { throw 'Bank CMake configure failed.' }
    & $cmakePath --build $bankBuild --target nds_banks -j $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Bank archive build failed.' }
    $PrebuiltBankArchive = Join-Path $bankBuild 'libnds_banks.a'
    $PrebuiltBankArchiveSha256 = (Get-FileHash -LiteralPath $PrebuiltBankArchive -Algorithm SHA256).Hash.ToLowerInvariant()
  }

  & $cmakePath @runnerArgs -B $runnerBuild `
    "-DNDS_PREBUILT_BANK_ARCHIVE=$PrebuiltBankArchive" `
    "-DNDS_PREBUILT_BANK_ARCHIVE_SHA256=$PrebuiltBankArchiveSha256"
  if ($LASTEXITCODE -ne 0) { throw 'Runner CMake configure failed.' }
  & $cmakePath --build $runnerBuild --target nds_runner -j $Jobs
  if ($LASTEXITCODE -ne 0) { throw 'Runner build failed.' }

  if ($BuildOnly) { return }

  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    "$root\tools\make_release.ps1" `
    -Version $Version `
    -RunnerBuildDir $RunnerBuildDir `
    -BuildDir $BuildDir `
    -RuntimeBinDir $RuntimeBinDir `
    -SdlBackend $SdlBackend
  if ($LASTEXITCODE -ne 0) { throw 'Release packaging failed.' }
} finally {
  Pop-Location
}
