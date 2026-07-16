#Requires -Version 7.5
# scripts/setup_preflight.ps1
# Validates all build dependencies, discovers SDK paths, writes config.ini.
# Called by setup.ps1 — do not run directly.
#
# Detection sources:
#   cmake        →  CMake version + path
#   vcpkg.exe    →  vcpkg presence + root path
#   vswhere      →  Build Tools / VS + vcvars64.bat path
#   dotnet       →  .NET SDK version (optional for some builds)
#   iscc         →  Inno Setup Compiler version (for installer creation)

param(
    [Parameter(Mandatory)][string]       $ProjectRoot,
    [Parameter(Mandatory)][string]       $ConfigFile,
    [Parameter(Mandatory)][System.Collections.Specialized.OrderedDictionary] $Manifest
)

$ErrorActionPreference = "Continue"

trap {
    Write-Host ""
    Write-Host "  ❌ Preflight hit an unexpected error:" -ForegroundColor Red
    Write-Host "  $_" -ForegroundColor DarkYellow
    Write-Host ""
    Write-Host "  Press any key to return to menu..." -ForegroundColor DarkGray
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    exit 1
}

# ---------------------------------------------------------------------------
# HELPERS
# ---------------------------------------------------------------------------
function Write-Check {
    param(
        [string]$Label,
        [bool]  $Ok,
        [string]$Detail = '',
        [string]$Hint   = '',
        [switch]$Info
    )
    $pad    = 22
    $status = if ($Info) { "ℹ " } elseif ($Ok) { "✅" } else { "❌" }
    $color  = if ($Info) { 'Cyan' } elseif ($Ok) { 'Green' } else { 'Red' }
    Write-Host "  $status  $($Label.PadRight($pad)) $Detail" -ForegroundColor $color
    if (-not $Ok -and -not $Info -and $Hint) {
        Write-Host "         $(' ' * $pad) → $Hint" -ForegroundColor DarkYellow
    }
}

function Add-ToUserPath ([string[]]$Dirs) {
    $current = [Environment]::GetEnvironmentVariable('PATH', 'User')
    $toAdd   = @($Dirs | Where-Object { -not (Test-InPath $_) })
    if ($toAdd.Count -eq 0) { return $false }
    $newPath = ($current.TrimEnd(';') + ';' + ($toAdd -join ';')).TrimStart(';')
    [Environment]::SetEnvironmentVariable('PATH', $newPath, 'User')
    $env:PATH = $env:PATH.TrimEnd(';') + ';' + ($toAdd -join ';')
    return $true
}

function Read-YN ([string]$Prompt) {
    return (Read-Host $Prompt).Trim().ToUpper() -eq 'Y'
}

function Test-InPath ([string]$Dir) {
    $systemPath = [Environment]::GetEnvironmentVariable('PATH', 'Machine')
    $userPath   = [Environment]::GetEnvironmentVariable('PATH', 'User')
    return ($systemPath -like "*$Dir*") -or ($userPath -like "*$Dir*")
}

function Offer-AddToPath ([string]$Label, [string]$Dir) {
    $missing = -not (Test-InPath $Dir)
    if (-not $missing) { return }

    Write-Host ""
    Write-Host "  ⚠  $Label is not on your PATH:" -ForegroundColor DarkYellow
    Write-Host "       · $Dir" -ForegroundColor White
    Write-Host ""
    if (Read-YN "     Add to your user PATH now? [Y/N]") {
        Add-ToUserPath -Dirs @($Dir) | Out-Null
        Write-Host "     ✅ Added to user PATH." -ForegroundColor Green
    } else {
        Write-Host "     Skipped." -ForegroundColor DarkGray
    }
}

# ---------------------------------------------------------------------------
# STATE
# ---------------------------------------------------------------------------
$allPassed = $true
$cfg       = @{}

Write-Host ""
Write-Host "  ── [2] Preflight Checks ────────────────────────────────────────────────" -ForegroundColor Cyan
Write-Host ""

# ===========================================================================
# 1. WINGET
# ===========================================================================
$wingetOk  = $false
$wingetVer = ''
try {
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        $raw = winget --version 2>&1
        if ($raw -match 'v?(\d+[\.\d]+)') {
            $wingetVer = $Matches[1]
            $parts     = $wingetVer -split '\.' | Select-Object -First 2
            $wingetOk  = [Version]($parts -join '.') -ge $Manifest['winget'].MinVersion
        }
    }
} catch {}

Write-Check 'winget' $wingetOk `
    $(if ($wingetOk) { "v$wingetVer" } else { 'not found' })

if (-not $wingetOk) {
    Write-Host ""
    Write-Host "  winget is required to install dependencies automatically." -ForegroundColor DarkYellow
    Write-Host "  Install App Installer from the Microsoft Store:" -ForegroundColor DarkYellow
    Write-Host "    ms-windows-store://pdp/?productid=9NBLGGH4NNS1" -ForegroundColor White
    Write-Host "  Or:" -ForegroundColor DarkYellow
    Write-Host "    https://aka.ms/getwinget " -ForegroundColor White
    Write-Host ""
    $allPassed = $false
}

# ===========================================================================
# 2. VS BUILD TOOLS  →  vswhere
# ===========================================================================
Write-Host ""
$btOk   = $false
$btVer  = ''
$btPath = ''
$vcvars = ''

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    # already in a developer shell
    $btOk   = $true
    $btVer  = 'cl.exe in PATH'
    $vcvars = 'already active'
    Write-Check 'VS Build Tools' $true $btVer
} elseif (Test-Path $vswhere) {
    # search Build Tools then full VS editions
    $products = @(
        'Microsoft.VisualStudio.Product.BuildTools',
        'Microsoft.VisualStudio.Product.Community',
        'Microsoft.VisualStudio.Product.Professional',
        'Microsoft.VisualStudio.Product.Enterprise'
    )
    foreach ($product in $products) {
        try {
            $info = & $vswhere -latest -products $product `
                        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                        -format json 2>$null | ConvertFrom-Json -ErrorAction SilentlyContinue
            if ($info -and $info.installationPath) {
                $candidate = "$($info.installationPath)\VC\Auxiliary\Build\vcvars64.bat"
                if (Test-Path $candidate) {
                    $btVer  = $info.installationVersion
                    $btPath = $info.installationPath
                    $vcvars = $candidate
                    $btOk   = $true
                    break
                }
            }
        } catch {}
    }

    if ($btOk) {
        Write-Check 'VS Build Tools' $true "$btVer   $btPath"
    } else {
        # VS exists but no C++ workload
        $anyVS = try { & $vswhere -latest -products * -format json 2>$null |
                       ConvertFrom-Json -ErrorAction SilentlyContinue } catch { $null }
        if ($anyVS -and $anyVS.installationPath) {
            Write-Check 'VS Build Tools' $false `
                "$($anyVS.installationVersion) — C++ workload not installed"
            Write-Host ""
            Write-Host "  Visual Studio is installed but the C++ workload is missing." -ForegroundColor DarkYellow
            Write-Host "  Open Visual Studio Installer → Modify → check:" -ForegroundColor DarkYellow
            Write-Host "    'Desktop development with C++'" -ForegroundColor White
            Write-Host ""
            Write-Host "  Or reinstall via winget:" -ForegroundColor DarkYellow
            Write-Host "    winget install $($Manifest['buildtools'].WingetId) ``" -ForegroundColor White
            Write-Host "      --override `"$($Manifest['buildtools'].WingetArgs)`"" -ForegroundColor White
        } else {
            Write-Check 'VS Build Tools' $false 'not found'
            Write-Host ""
            Write-Host "  MSVC C++ compiler not found." -ForegroundColor DarkYellow
            Write-Host "  You need VS 2022 Build Tools — not the full IDE (~6 GB)." -ForegroundColor White
            Write-Host ""
            Write-Host "  Option A — winget (recommended, run as admin):" -ForegroundColor White
            Write-Host "    winget install $($Manifest['buildtools'].WingetId) ``" -ForegroundColor Cyan
            Write-Host "      --override `"$($Manifest['buildtools'].WingetArgs)`"" -ForegroundColor Cyan
            Write-Host ""
            Write-Host "  Option B — manual:" -ForegroundColor White
            Write-Host "    $($Manifest['buildtools'].Url)" -ForegroundColor Cyan
            Write-Host "    → select 'Desktop development with C++'" -ForegroundColor DarkGray
            Write-Host ""
            Write-Host "  After installing, restart your terminal and re-run preflight." -ForegroundColor DarkGray
            Write-Host "  You will never need to open Visual Studio." -ForegroundColor DarkGray
        }
        $allPassed = $false
    }
} else {
    Write-Check 'VS Build Tools' $false 'not found'
    Write-Host ""
    Write-Host "  MSVC C++ compiler not found." -ForegroundColor DarkYellow
    Write-Host "  You need VS 2022 Build Tools — not the full IDE (~6 GB)." -ForegroundColor White
    Write-Host ""
    Write-Host "  Option A — winget (recommended, run as admin):" -ForegroundColor White
    Write-Host "    winget install $($Manifest['buildtools'].WingetId) ``" -ForegroundColor Cyan
    Write-Host "      --override `"$($Manifest['buildtools'].WingetArgs)`"" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  Option B — manual:" -ForegroundColor White
    Write-Host "    $($Manifest['buildtools'].Url)" -ForegroundColor Cyan
    Write-Host "    → select 'Desktop development with C++'" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "  After installing, restart your terminal and re-run preflight." -ForegroundColor DarkGray
    Write-Host "  You will never need to open Visual Studio." -ForegroundColor DarkGray
    $allPassed = $false
}

if ($btOk) { $cfg['vcvars'] = $vcvars }

# ===========================================================================
# 3. CMAKE  →  cmake --version
# ===========================================================================
Write-Host ""
$cmakeOk  = $false
$cmakeVer = ''
$cmakeExe = ''

try {
    $cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCmd) {
        $cmakeExe = $cmakeCmd.Source
        $raw = (cmake --version 2>&1) | Select-Object -First 1
        if ($raw -match 'cmake\s+version\s+(\d+\.\d+)') {
            $cmakeVer = $Matches[1]
            $cmakeOk  = [Version]$cmakeVer -ge $Manifest['cmake'].MinVersion
        }
    }
} catch {}

Write-Check 'CMake' $cmakeOk `
    $(if ($cmakeOk)      { "$cmakeVer   $cmakeExe" }
      elseif ($cmakeVer) { "$cmakeVer (need ≥ $($Manifest['cmake'].MinVersion))" }
      else               { 'not found' })

if (-not $cmakeOk) {
    Write-Host ""
    Write-Host "  Install CMake $($Manifest['cmake'].MinVersion)+:" -ForegroundColor DarkYellow
    Write-Host "    winget install $($Manifest['cmake'].WingetId)" -ForegroundColor White
    Write-Host "    $($Manifest['cmake'].Url)" -ForegroundColor Cyan
    $allPassed = $false
} else {
    $cmakeDir = Split-Path $cmakeExe
    Offer-AddToPath 'CMake' $cmakeDir
    $cfg['cmake_exe'] = $cmakeExe
}

# ===========================================================================
# 4. VCPKG  →  filesystem discovery
# ===========================================================================
Write-Host ""
$vcpkgOk   = $false
$vcpkgVer  = ''
$vcpkgRoot = ''

# Probe locations in priority order
$probePaths = @(
    $env:VCPKG_ROOT,
    (Join-Path $env:LOCALAPPDATA "vcpkg"),
    "C:\vcpkg",
    (Join-Path $env:USERPROFILE "vcpkg")
) | Where-Object { $_ }

foreach ($candidate in $probePaths) {
    if (-not $candidate) { continue }
    $vcpkgExe = Join-Path $candidate "vcpkg.exe"
    $toolchain = Join-Path $candidate "scripts\buildsystems\vcpkg.cmake"
    
    if (Test-Path $toolchain) {
        $vcpkgRoot = $candidate
        $vcpkgOk = $true
        
        # Try to get version
        try {
            $verOut = & $vcpkgExe version 2>&1
            if ($verOut -match '(\d+\.\d+)') {
                $vcpkgVer = $Matches[1]
            }
        } catch {}
        
        break
    }
}

Write-Check 'vcpkg' $vcpkgOk `
    $(if ($vcpkgOk)      { "$vcpkgVer   $vcpkgRoot" }
      elseif ($vcpkgRoot){ "found (incomplete installation)" }
      else               { 'not found' })

if (-not $vcpkgOk) {
    Write-Host ""
    Write-Host "  vcpkg not found. Setup steps:" -ForegroundColor DarkYellow
    Write-Host ""
    Write-Host "    1. Clone vcpkg:" -ForegroundColor White
    Write-Host "       git clone https://github.com/microsoft/vcpkg.git" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "    2. Bootstrap:" -ForegroundColor White
    Write-Host "       .\vcpkg\bootstrap-vcpkg.bat" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "    3. Set environment variable (permanent):" -ForegroundColor White
    Write-Host "       [Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\path\to\vcpkg', 'User')" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "    4. Install required packages:" -ForegroundColor White
    Write-Host "       vcpkg install wil" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "  Or download from: $($Manifest['vcpkg'].Url)" -ForegroundColor Cyan
    $allPassed = $false
} else {
    $cfg['vcpkg_root'] = $vcpkgRoot
}

# ===========================================================================
# 5. .NET SDK  →  dotnet  (optional for some VirtuaCam builds)
# ===========================================================================
Write-Host ""
$dotnetOk  = $false
$dotnetVer = ''

try {
    if (Get-Command dotnet -ErrorAction SilentlyContinue) {
        $raw = dotnet --version 2>&1
        if ($raw -match '(\d+\.\d+)') {
            $dotnetVer = $Matches[1]
            $dotnetOk  = [Version]$dotnetVer -ge $Manifest['dotnet'].MinVersion
        }
    }
} catch {}

Write-Check '.NET SDK' $dotnetOk `
    $(if ($dotnetOk)      { $dotnetVer }
      elseif ($dotnetVer) { "$dotnetVer (need ≥ $($Manifest['dotnet'].MinVersion))" }
      else                { 'not found (optional)' }) -Info

if (-not $dotnetOk) {
    Write-Host "         $(' ' * 22) Required only if building .NET components" -ForegroundColor DarkGray
}

# ===========================================================================
# 6. INNO SETUP  →  iscc  (for installer creation)
# ===========================================================================
Write-Host ""
$innoOk  = $false
$innoVer = ''
$innoExe = ''

try {
    $isccCmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($isccCmd) {
        $innoExe = $isccCmd.Source
        # Inno Setup doesn't have a clean --version, check file version
        $innoVer = (Get-Item $innoExe).VersionInfo.FileVersion
        $innoOk  = [Version]$innoVer -ge $Manifest['inno'].MinVersion
    }
} catch {}

Write-Check 'Inno Setup' $innoOk `
    $(if ($innoOk)      { "$innoVer   $innoExe" }
      elseif ($innoVer) { "$innoVer (need ≥ $($Manifest['inno'].MinVersion))" }
      else              { 'not found (optional)' }) -Info

if (-not $innoOk) {
    Write-Host "         $(' ' * 22) Required only for [5] Create installer" -ForegroundColor DarkGray
    Write-Host "         $(' ' * 22) $($Manifest['inno'].Url)" -ForegroundColor DarkGray
}

# ===========================================================================
# SUMMARY + WRITE config.ini
# ===========================================================================
Write-Host ""
Write-Host "  ────────────────────────────────────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host ""

if ($allPassed) {
    Write-Host "  ✅ All required checks passed." -ForegroundColor Green
    Write-Host ""

    $timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'

@"
; VirtuaCam — machine config
; Generated by setup_preflight.ps1 on $timestamp
; Do not commit — see .gitignore
; Re-run [2] Preflight to regenerate.

[machine]
preflight_passed  = true
preflight_date    = $timestamp
vcvars            = $($cfg['vcvars'])
vcpkg_root        = $($cfg['vcpkg_root'])
cmake_exe         = $($cfg['cmake_exe'])
"@ | Set-Content $ConfigFile -Encoding UTF8

    Write-Host "  + config.ini written." -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "  You can now proceed to [4] Build." -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  Press any key to return to menu..." -ForegroundColor DarkGray
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")

} else {
    Write-Host "  ❌ One or more required checks failed." -ForegroundColor Red
    Write-Host "     Resolve the items above and re-run [2] Preflight." -ForegroundColor DarkYellow
    Write-Host "     [3] Install dependencies can handle winget-installable items." -ForegroundColor DarkYellow

    # stamp config.ini so Build stays gated
    if (Test-Path $ConfigFile) {
        $content = (Get-Content $ConfigFile -Raw) -replace 'preflight_passed\s*=\s*true', 'preflight_passed = false'
        $content | Set-Content $ConfigFile -Encoding UTF8
    }

    Write-Host ""
    Write-Host "  Press any key to return to menu..." -ForegroundColor DarkGray
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
}
