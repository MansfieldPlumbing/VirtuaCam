#Requires -Version 7.5
# scripts/setup_preflight.ps1
# Validates build dependencies for VirtuaCam via Targeted Probing.
# Called by setup.ps1 - do not run directly.
#
# Strategy:
# 1. Check PATH first (fastest).
# 2. Check standard installation roots (C:\Program Files, C:\bin, etc.).
# 3. Check VS internal paths for vcpkg.
# NO recursive scanning, NO MFT raw access (requires Admin/AV triggers).

param(
    [Parameter(Mandatory)][string] $ProjectRoot,
    [Parameter(Mandatory)][string] $ConfigFile
)

$ErrorActionPreference = "Continue"

trap {
    Write-Host "`n  [FATAL] Preflight encountered an unhandled exception:" -ForegroundColor Red
    Write-Host "  $_`n" -ForegroundColor DarkYellow
    exit 1
}

# ---------------------------------------------------------------------------
# HELPERS
# ---------------------------------------------------------------------------
function Write-Check {
    param([string]$Label, [bool]$Ok, [string]$Detail = '', [string]$Hint = '')
    $pad    = 22
    $status = if ($Ok) { "[OK]" } else { "[!!]" }
    $color  = if ($Ok) { 'Green' } else { 'Red' }
    Write-Host "  $status  $($Label.PadRight($pad)) $Detail" -ForegroundColor $color
    if (-not $Ok -and $Hint) {
        Write-Host "         $(' ' * $pad) > $Hint" -ForegroundColor DarkYellow
    }
}

function Find-Executable {
    param(
        [string]$Name,
        [string[]]$ExtraPaths
    )
    
    # 1. Check PATH
    $found = Get-Command $Name -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }

    # 2. Check Extra Paths (Targeted Probe)
    foreach ($path in $ExtraPaths) {
        if (Test-Path $path) { return $path }
    }

    return $null
}

function Find-VcpkgRoot {
    # vcpkg.exe lives in the root of the repo, not a bin folder usually
    
    # 1. Environment Variable
    if ($env:VCPKG_ROOT) {
        $exe = Join-Path $env:VCPKG_ROOT 'vcpkg.exe'
        if (Test-Path $exe) { return $env:VCPKG_ROOT }
    }

    # 2. Standard Locations
    $probes = @(
        "C:\vcpkg\vcpkg.exe",
        "C:\bin\vcpkg\vcpkg.exe",
        "$env:USERPROFILE\vcpkg\vcpkg.exe",
        "$env:LOCALAPPDATA\vcpkg\vcpkg.exe"
    )
    
    foreach ($path in $probes) {
        if (Test-Path $path) { return Split-Path $path -Parent }
    }

    # 3. Visual Studio Bundled vcpkg (Common in VS 2022/2026)
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        try {
            $vsInstalls = & $vsWhere -format json -products * | ConvertFrom-Json
            foreach ($vs in $vsInstalls) {
                $root = $vs.installationPath
                $candidates = @(
                    "$root\VC\vcpkg\vcpkg.exe",
                    "$root\Common7\IDE\CommonExtensions\Microsoft\Vcpkg\vcpkg.exe"
                )
                foreach ($c in $candidates) {
                    if (Test-Path $c) { return Split-Path $c -Parent }
                }
            }
        } catch {}
    }

    return $null
}

# ---------------------------------------------------------------------------
# MAIN CHECKS
# ---------------------------------------------------------------------------
Write-Host "`n  -- [2] Preflight Checks (Targeted Probe) ----------------------------------------`n" -ForegroundColor Cyan

$cfg = @{
    vcvars     = $null
    cmake_exe  = $null
    vcpkg_root = $null
    iscc_exe   = $null
}

$AllPassed = $true

# 1. VS Build Tools (vcvars64.bat)
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvarsPath = $null

if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    $vcvarsPath = "Active Developer Shell (cl.exe found)"
} elseif (Test-Path $vsWhere) {
    $vs = & $vsWhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json 2>$null | ConvertFrom-Json
    if ($vs) {
        $candidate = "$($vs.installationPath)\VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $candidate) { $vcvarsPath = $candidate }
    }
}

if (-not $vcvarsPath) {
    $probes = @(
        "C:\Program Files\Microsoft Visual Studio\2026\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2026\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2026\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\bin\vs\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($p in $probes) {
        if (Test-Path $p) { $vcvarsPath = $p; break }
    }
}

if ($vcvarsPath) {
    Write-Check 'VS Build Tools' $true $vcvarsPath
    $cfg['vcvars'] = $vcvarsPath
} else {
    Write-Check 'VS Build Tools' $false 'vcvars64.bat not found' 'Install VS 2022/2026 with "Desktop development with C++"'
    $AllPassed = $false
}

# 2. CMake
$cmakeProbes = @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\bin\cmake\bin\cmake.exe",
    "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Kitware.CMake\cmake.exe"
)
if (Test-Path "$env:LOCALAPPDATA\micromamba") {
    $mambaCmake = Get-ChildItem "$env:LOCALAPPDATA\micromamba\envs\*\Scripts\cmake.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($mambaCmake) { $cmakeProbes += $mambaCmake.FullName }
}

$cmakeExe = Find-Executable 'cmake.exe' -ExtraPaths $cmakeProbes

if ($cmakeExe) {
    Write-Check 'CMake' $true $cmakeExe
    $cfg['cmake_exe'] = $cmakeExe
} else {
    Write-Check 'CMake' $false 'cmake.exe not found' 'winget install Kitware.CMake'
    $AllPassed = $false
}

# 3. vcpkg
$vcpkgRoot = Find-VcpkgRoot

if ($vcpkgRoot) {
    Write-Check 'vcpkg' $true $vcpkgRoot
    $cfg['vcpkg_root'] = $vcpkgRoot
} else {
    Write-Check 'vcpkg' $false 'vcpkg.exe not found' 'Clone to C:\vcpkg or set VCPKG_ROOT'
    $AllPassed = $false
}

# 4. Inno Setup (ISCC.exe) - OPTIONAL
$isccProbes = @(
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe",
    "C:\bin\inno\ISCC.exe"
)
$isccExe = Find-Executable 'ISCC.exe' -ExtraPaths $isccProbes

if ($isccExe) {
    Write-Check 'Inno Setup' $true $isccExe
    $cfg['iscc_exe'] = $isccExe
} else {
    Write-Check 'Inno Setup' $false 'ISCC.exe not found' 'Optional: winget install JRSoftware.InnoSetup'
}

# ---------------------------------------------------------------------------
# SUMMARY & CONFIG
# ---------------------------------------------------------------------------
Write-Host "`n  -----------------------------------------------------------------------------------`n" -ForegroundColor DarkGray

$Timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'

if ($AllPassed) {
    Write-Host "  [OK] Preflight validation completed successfully.`n" -ForegroundColor Green

@"
; VirtuaCam - machine config
; Generated by setup_preflight.ps1 on $Timestamp
; Do not commit - see .gitignore

[machine]
preflight_passed  = true
preflight_date    = $Timestamp
vcvars            = $($cfg['vcvars'])
cmake_exe         = $($cfg['cmake_exe'])
vcpkg_root        = $($cfg['vcpkg_root'])
iscc_exe          = $($cfg['iscc_exe'])
"@ | Set-Content $ConfigFile -Encoding UTF8

    Write-Host "  + Configuration written to: $ConfigFile" -ForegroundColor DarkGray
} else {
    Write-Host "  [!!] Core dependencies are missing.`n" -ForegroundColor Red
    Write-Host "       Note: Inno Setup is optional (only for creating installer).`n" -ForegroundColor DarkGray
    
    if (Test-Path $ConfigFile) {
        $Content = (Get-Content $ConfigFile -Raw) -replace 'preflight_passed\s*=\s*true', 'preflight_passed = false'
        $Content | Set-Content $ConfigFile -Encoding UTF8
    }
}

Write-Host "`n  Press any key to return to menu..." -ForegroundColor DarkGray
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
