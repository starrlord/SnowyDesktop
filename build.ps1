# Builds snowystarr.exe (and optionally the installer) with CMake + Visual Studio 2022.
#   .\build.ps1                       # Release build -> build\Release\snowystarr.exe
#   .\build.ps1 -Installer            # ...and dist\snowystarr-<version>-setup.exe (needs Inno Setup 6)
#   .\build.ps1 -Version 1.2.0 -Installer
#   .\build.ps1 -Config Debug
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [string]$Version = '',
    [switch]$Installer
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'CMake was not found on PATH. Install it from https://cmake.org or via Visual Studio.'
}

$configure = @('-S', $root, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($Version) { $configure += "-DSNOWYSTARR_VERSION=$Version" }
cmake @configure
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build --config $Config --parallel
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$exe = Join-Path $build "$Config\snowystarr.exe"
Write-Host "`nBuilt: $exe"

if ($Installer) {
    $iscc = @(
        (Get-Command iscc -ErrorAction SilentlyContinue).Source,
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    ) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 was not found (winget install JRSoftware.InnoSetup).' }
    if (-not $Version) {
        $Version = (Select-String -Path (Join-Path $build 'CMakeCache.txt') -Pattern '^SNOWYSTARR_VERSION:STRING=(.+)$').Matches[0].Groups[1].Value
    }
    & $iscc "/DAppVersion=$Version" "/DSourceExe=$exe" (Join-Path $root 'installer\snowystarr.iss')
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
