# =============================================================================
# build.ps1  --  Build VirtuaCam
# =============================================================================
# Requires Visual Studio 2022 (Desktop development with C++) and CMake 3.21+.
# WIL is fetched automatically if no vcpkg/installed copy is found.
#
#   .\build.ps1                  Release build  -> build\bin\Release
#   .\build.ps1 -Config Debug
#   .\build.ps1 -Clean
#   .\build.ps1 -Register        also register VirtuaCamSource.dll (elevates)
#   .\build.ps1 -Installer       also compile installer\VirtuaCam.iss (Inno Setup 6)
# =============================================================================

param(
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Register,
    [switch]$Installer
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'
$bin = Join-Path $build "bin\$Config"

function Invoke-Step([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

$configure = @('-S', (Join-Path $root 'src'), '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($env:VCPKG_ROOT) {
    $configure += "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake')"
}
Invoke-Step cmake $configure
Invoke-Step cmake @('--build', $build, '--config', $Config, '--parallel')
Write-Host "Built: $bin" -ForegroundColor Green

if ($Register) {
    # The Camera Frame Server only sees machine-wide COM registrations.
    $dll = Join-Path $bin 'VirtuaCamSource.dll'
    $process = Start-Process regsvr32.exe -ArgumentList "/s `"$dll`"" -Verb RunAs -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($process.ExitCode)" }
    Write-Host "Registered: $dll" -ForegroundColor Green
}

if ($Installer) {
    $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup).' }
    Invoke-Step $iscc @("/DBinDir=$bin", (Join-Path $root 'installer\VirtuaCam.iss'))
}
