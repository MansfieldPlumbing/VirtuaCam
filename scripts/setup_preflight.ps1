#Requires -Version 7.5
# scripts/setup_preflight.ps1
# Validates all build dependencies for VirtuaCam, discovers tool paths, writes config.ini.
# Called by setup.ps1 - do not run directly.

param(
    [Parameter(Mandatory)][string]       $ProjectRoot,
    [Parameter(Mandatory)][string]       $ConfigFile,
    [Parameter(Mandatory)][System.Collections.Specialized.OrderedDictionary] $Manifest
)

$ErrorActionPreference = "Continue"

trap {
    Write-Host ""
    Write-Host "  Preflight hit an unexpected error:" -ForegroundColor Red
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
    $status = if ($Info) { "[..]" } elseif ($Ok) { "[OK]" } else { "[!!]" }
    $color  = if ($Info) { 'Cyan' } elseif ($Ok) { 'Green' } else { 'Red' }
    Write-Host "  $status  $($Label.PadRight($pad)) $Detail" -ForegroundColor $color
    if (-not $Ok -and -not $Info -and $Hint) {
        Write-Host "         $(' ' * $pad) > $Hint" -ForegroundColor DarkYellow
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

function Test-InPath ([string]$Dir) {
    $systemPath = [Environment]::GetEnvironmentVariable('PATH', 'Machine')
    $userPath   = [Environment]::GetEnvironmentVariable('PATH', 'User')
    return ($systemPath -like "*$Dir*") -or ($userPath -like "*$Dir*")
}

function Read-YN ([string]$Prompt) {
    return (Read-Host $Prompt).Trim().ToUpper() -eq 'Y'
}

# Fast VS detection - checks common locations first, then uses vswhere
function Find-VSBuildTools {
    # Check if already in a developer command prompt
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return @{ Found = $true; Version = 'cl.exe in PATH'; VcVars = 'already active' }
    }
    
    # Try vswhere with prerelease flag for VS 2026
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    
    if (Test-Path $vswhere) {
        $products = @(
            'Microsoft.VisualStudio.Product.BuildTools',
            'Microsoft.VisualStudio.Product.Community',
            'Microsoft.VisualStudio.Product.Professional',
            'Microsoft.VisualStudio.Product.Enterprise'
        )
        
        foreach ($product in $products) {
            try {
                $info = & $vswhere -latest -prerelease -products $product `
                            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                            -format json 2>$null | ConvertFrom-Json -ErrorAction SilentlyContinue
                if ($info -and $info.installationPath) {
                    $candidate = "$($info.installationPath)\VC\Auxiliary\Build\vcvars64.bat"
                    if (Test-Path $candidate) {
                        return @{ 
                            Found = $true
                            Version = $info.installationVersion
                            Path = $info.installationPath
                            VcVars = $candidate
                        }
                    }
                }
            } catch {}
        }
        
        # Check if VS exists but without C++ workload
        $anyVS = try { 
            & $vswhere -latest -prerelease -products * -format json 2>$null | 
            ConvertFrom-Json -ErrorAction SilentlyContinue 
        } catch { $null }
        
        if ($anyVS -and $anyVS.installationPath) {
            return @{ 
                Found = $false
                Version = $anyVS.installationVersion
                Path = $anyVS.installationPath
                MissingCpp = $true
            }
        }
    }
    
    # Fallback: scan likely installation roots (fast targeted scan)
    $likelyRoots = @(
        "${env:ProgramFiles}\Microsoft Visual Studio",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio",
        "C:\BuildTools",
        "D:\BuildTools",
        "C:\VS",
        "D:\VS"
    )
    
    foreach ($root in $likelyRoots) {
        if (Test-Path $root) {
            $vcvars = Get-ChildItem -Path $root -Filter "vcvars64.bat" -Recurse -Depth 3 -ErrorAction SilentlyContinue | 
                      Select-Object -First 1
            if ($vcvars) {
                return @{
                    Found = $true
                    Version = 'detected via scan'
                    Path = (Split-Path $vcvars.DirectoryName -Parent)
                    VcVars = $vcvars.FullName
                }
            }
        }
    }
    
    return @{ Found = $false }
}

# Fast CMake detection
function Find-CMake {
    # Check PATH first
    if (Get-Command cmake -ErrorAction SilentlyContinue) {
        $cmakePath = (Get-Command cmake).Source
        $version = (cmake --version 2>&1 | Select-Object -First 1)
        if ($version -match '(\d+\.\d+\.\d+)') {
            return @{ Found = $true; Exe = $cmakePath; Version = $Matches[1] }
        }
    }
    
    # Check common install locations
    $commonPaths = @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe",
        "$env:LOCALAPPDATA\Microsoft\WindowsApps\cmake.exe"
    )
    
    foreach ($path in $commonPaths) {
        if (Test-Path $path) {
            $version = (& $path --version 2>&1 | Select-Object -First 1)
            if ($version -match '(\d+\.\d+\.\d+)') {
                return @{ Found = $true; Exe = $path; Version = $Matches[1] }
            }
        }
    }
    
    # Check VS bundled CMake
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsCmake = & $vswhere -latest -prerelease -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                   -find "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" 2>$null
        if ($vsCmake -and (Test-Path $vsCmake)) {
            $version = (& $vsCmake --version 2>&1 | Select-Object -First 1)
            if ($version -match '(\d+\.\d+\.\d+)') {
                return @{ Found = $true; Exe = $vsCmake; Version = $Matches[1] }
            }
        }
    }
    
    return @{ Found = $false }
}

# Fast vcpkg detection
function Find-Vcpkg {
    # Check common locations
    $commonPaths = @(
        "C:\vcpkg\vcpkg.exe",
        "$env:LOCALAPPDATA\vcpkg\vcpkg.exe",
        "$env:USERPROFILE\vcpkg\vcpkg.exe",
        "$env:USERPROFILE\scoop\apps\vcpkg\current\vcpkg.exe"
    )
    
    foreach ($path in $commonPaths) {
        if (Test-Path $path) {
            return @{ Found = $true; Exe = $path }
        }
    }
    
    # Check if on PATH
    if (Get-Command vcpkg -ErrorAction SilentlyContinue) {
        return @{ Found = $true; Exe = (Get-Command vcpkg).Source }
    }
    
    return @{ Found = $false }
}

# ---------------------------------------------------------------------------
# STATE
# ---------------------------------------------------------------------------
$allPassed = $true
$cfg       = @{}

Write-Host ""
Write-Host "  -- [2] Preflight Checks ----------------------------------------------------------" -ForegroundColor Cyan
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
    $(if ($wingetOk) { "v$wingetVer" } else { 'not found' }) `
    -Hint "Install App Installer from Microsoft Store: ms-windows-store://pdp/?productid=9NBLGGH4NNS1"

if (-not $wingetOk) {
    $allPassed = $false
}

# ===========================================================================
# 2. VS BUILD TOOLS
# ===========================================================================
Write-Host ""
$vsResult = Find-VSBuildTools

if ($vsResult.Found) {
    Write-Check 'VS Build Tools' $true "$($vsResult.Version)"
    $cfg['vcvars'] = $vsResult.VcVars
} else {
    $detail = 'not found'
    if ($vsResult.MissingCpp) {
        $detail = "$($vsResult.Version) - C++ workload missing"
    }
    
    Write-Check 'VS Build Tools' $false $detail
    
    if ($vsResult.MissingCpp) {
        Write-Host ""
        Write-Host "  Visual Studio is installed but the C++ workload is missing." -ForegroundColor DarkYellow
        Write-Host "  Open Visual Studio Installer > Modify > check:" -ForegroundColor DarkYellow
        Write-Host "    'Desktop development with C++'" -ForegroundColor White
    } else {
        Write-Host ""
        Write-Host "  Install VS 2022 Build Tools (or full VS with C++ workload):" -ForegroundColor DarkYellow
        Write-Host "    winget install Microsoft.VisualStudio.2022.BuildTools ``" -ForegroundColor White
        Write-Host "      --override '--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'" -ForegroundColor White
        Write-Host "  Or: $($Manifest['buildtools'].Url)" -ForegroundColor White
    }
    $allPassed = $false
}

# ===========================================================================
# 3. CMAKE
# ===========================================================================
Write-Host ""
$cmakeResult = Find-CMake

if ($cmakeResult.Found) {
    $cmakeOk = [Version]$cmakeResult.Version -ge $Manifest['cmake'].MinVersion
    Write-Check 'CMake' $cmakeOk "$($cmakeResult.Version) - $($cmakeResult.Exe)"
    if ($cmakeOk) {
        $cfg['cmake_exe'] = $cmakeResult.Exe
    }
} else {
    Write-Check 'CMake' $false 'not found'
    Write-Host ""
    Write-Host "  CMake can be installed via winget or downloaded from:" -ForegroundColor DarkYellow
    Write-Host "    winget install Kitware.CMake" -ForegroundColor White
    Write-Host "    $($Manifest['cmake'].Url)" -ForegroundColor White
    $allPassed = $false
}

# ===========================================================================
# 4. VCPKG
# ===========================================================================
Write-Host ""
$vcpkgResult = Find-Vcpkg

if ($vcpkgResult.Found) {
    Write-Check 'vcpkg' $true "$($vcpkgResult.Exe)" -Info
    $cfg['vcpkg_root'] = Split-Path $vcpkgResult.Exe
} else {
    Write-Check 'vcpkg' $false 'not found' -Info
    Write-Host "         $(' ' * 22) Can be cloned during [3] Install dependencies" -ForegroundColor DarkGray
}

# ===========================================================================
# 5. .NET SDK
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
    $(if ($dotnetOk) { $dotnetVer } elseif ($dotnetVer) { "$dotnetVer (need >= $($Manifest['dotnet'].MinVersion))" } else { 'not found' }) `
    -Hint "winget install $($Manifest['dotnet'].WingetId)"

if (-not $dotnetOk) {
    $allPassed = $false
}

# ===========================================================================
# 6. INNO SETUP
# ===========================================================================
Write-Host ""
$innoOk = $false
$innoVer = ''

try {
    $innoPath = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    if (Test-Path $innoPath) {
        $innoOk = $true
        $innoVer = '6.x'
    } elseif (Get-Command ISCC -ErrorAction SilentlyContinue) {
        $innoOk = $true
        $innoVer = 'found on PATH'
    }
} catch {}

Write-Check 'Inno Setup' $innoOk `
    $(if ($innoOk) { $innoVer } else { 'not found' }) `
    -Hint "winget install $($Manifest['inno'].WingetId)"

if (-not $innoOk) {
    $allPassed = $false
}

# ===========================================================================
# SUMMARY + WRITE config.ini
# ===========================================================================
Write-Host ""
Write-Host "  -----------------------------------------------------------------------------------" -ForegroundColor DarkGray
Write-Host ""

if ($allPassed) {
    Write-Host "  [OK] All checks passed." -ForegroundColor Green
    Write-Host ""

    $timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'

@"
; VirtuaCam - machine config
; Generated by setup_preflight.ps1 on $timestamp
; Do not commit - see .gitignore
; Re-run [2] Preflight to regenerate.

[machine]
preflight_passed  = true
preflight_date    = $timestamp
vcvars            = $($cfg['vcvars'])
cmake_exe         = $($cfg['cmake_exe'])
vcpkg_root        = $($cfg['vcpkg_root'])
"@ | Set-Content $ConfigFile -Encoding UTF8

    Write-Host "  + config.ini written." -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "  You can now proceed to [4] Build." -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  Press any key to return to menu..." -ForegroundColor DarkGray
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")

} else {
    Write-Host "  [!!] One or more checks failed." -ForegroundColor Red
    Write-Host "       Resolve the items above and re-run [2] Preflight." -ForegroundColor Yellow
    Write-Host "       [3] Install dependencies can handle winget-installable items." -ForegroundColor Yellow

    # stamp config.ini so Build stays gated
    if (Test-Path $ConfigFile) {
        $content = (Get-Content $ConfigFile -Raw) -replace 'preflight_passed\s*=\s*true', 'preflight_passed = false'
        $content | Set-Content $ConfigFile -Encoding UTF8
    }

    Write-Host ""
    Write-Host "  Press any key to return to menu..." -ForegroundColor DarkGray
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
}
