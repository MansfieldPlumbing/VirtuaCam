#Requires -Version 7.5
# setup.ps1
# Root orchestrator for VirtuaCam.
# Builds dependency manifests, presents the menu loop, delegates to child scripts.
# Nothing happens without the user opting in. Every action returns to this menu.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# POWERSHELL VERSION GATE
# ---------------------------------------------------------------------------
if ($PSVersionTable.PSVersion -lt [Version]"7.5") {
    Write-Host ""
    Write-Host "  ERROR: PowerShell $($PSVersionTable.PSVersion) detected." -ForegroundColor Red
    Write-Host "  VirtuaCam requires PowerShell 7.5 or later." -ForegroundColor Red
    Write-Host ""
    Write-Host "  winget install Microsoft.PowerShell" -ForegroundColor White
    Write-Host "  https://github.com/PowerShell/PowerShell/releases/latest " -ForegroundColor DarkYellow
    Write-Host ""
    pause
    exit 1
}

# ---------------------------------------------------------------------------
# PATHS
# ---------------------------------------------------------------------------
$ProjectRoot = $PSScriptRoot
$ConfigFile  = "$ProjectRoot\config.ini"
$ScriptsDir  = "$ProjectRoot\scripts"

# ---------------------------------------------------------------------------
# MANIFEST — single source of truth for all dependency requirements.
# These constants are never hardcoded anywhere else. Scripts read from here.
# config.ini is generated from discovery — this defines what we are looking for.
# ---------------------------------------------------------------------------
$Manifest = [ordered]@{

    winget = [PSCustomObject]@{
        Label      = 'winget'
        MinVersion = [Version]'1.0'
        WingetId   = $null
        WingetArgs = $null
        Url        = 'https://aka.ms/getwinget'
        UrlLabel   = 'aka.ms/getwinget'
        Note       = $null
    }

    buildtools = [PSCustomObject]@{
        Label      = 'VS Build Tools'
        MinVersion = [Version]'2022.0'
        WingetId   = 'Microsoft.VisualStudio.2022.BuildTools'
        WingetArgs = '--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
        Url        = 'https://aka.ms/vs/17/release/vs_buildtools.exe'
        UrlLabel   = 'aka.ms/vs/17/release/vs_buildtools.exe'
        Note       = 'C++ build tools only — no IDE'
    }

    cmake = [PSCustomObject]@{
        Label      = 'CMake'
        MinVersion = [Version]'3.20'
        WingetId   = 'Kitware.CMake'
        WingetArgs = $null
        Url        = 'https://cmake.org/download/'
        UrlLabel   = 'cmake.org/download'
        Note       = $null
    }

    vcpkg = [PSCustomObject]@{
        Label      = 'vcpkg'
        MinVersion = [Version]'1.0'
        WingetId   = $null
        WingetArgs = $null
        Url        = 'https://vcpkg.io/en/getting-started'
        UrlLabel   = 'vcpkg.io/en/getting-started'
        Note       = 'git clone + bootstrap — not winget'
        Glob       = 'C:\vcpkg\vcpkg.exe'
        Globs      = @(
            "$env:LOCALAPPDATA\vcpkg\vcpkg.exe",
            "$env:USERPROFILE\vcpkg\vcpkg.exe"
        )
    }

    dotnet = [PSCustomObject]@{
        Label      = '.NET SDK'
        MinVersion = [Version]'8.0'
        WingetId   = 'Microsoft.DotNet.SDK.8'
        WingetArgs = $null
        Url        = 'https://dotnet.microsoft.com/download/dotnet/8.0'
        UrlLabel   = 'dotnet.microsoft.com/download/dotnet/8.0'
        Note       = '$null'
    }

    inno = [PSCustomObject]@{
        Label      = 'Inno Setup'
        MinVersion = [Version]'6.0'
        WingetId   = 'JRSoftware.InnoSetup'
        WingetArgs = $null
        Url        = 'https://jrsoftware.org/isinfo.php'
        UrlLabel   = 'jrsoftware.org/isinfo.php'
        Note       = 'for building installer .exe'
    }
}

# ---------------------------------------------------------------------------
# HELPERS
# ---------------------------------------------------------------------------
function Read-Config {
    $cfg = @{}
    if (Test-Path $ConfigFile) {
        $section = ''
        Get-Content $ConfigFile | ForEach-Object {
            $line = $_.Trim()
            if ($line -match '^\[(.+)\]$')          { $section = $Matches[1] }
            elseif ($line -match '^([^;=]+)=(.*)$') { $cfg["$section.$($Matches[1].Trim())"] = $Matches[2].Trim() }
        }
    }
    return $cfg
}

function Get-ConfigValue([string]$Key) {
    $cfg = Read-Config
    return $cfg[$Key]
}

function Test-PreflightPassed {
    return (Get-ConfigValue 'machine.preflight_passed') -eq 'true'
}

function Get-PreflightStatus {
    if (-not (Test-Path $ConfigFile)) { return 'never' }
    $val = Get-ConfigValue 'machine.preflight_passed'
    if ($val -eq 'true')  { return 'passed' }
    if ($val -eq 'false') { return 'failed' }
    return 'never'
}

function Build-Paths {
    $cfg = Read-Config
    return [PSCustomObject]@{
        ProjectRoot  = $ProjectRoot
        ConfigFile   = $ConfigFile
        ScriptsDir   = $ScriptsDir
        VcpkgRoot    = $cfg['machine.vcpkg_root']
        CmakeExe     = $cfg['machine.cmake_exe']
        EnvName      = 'virtuacam'
        SrcDir       = "$ProjectRoot\src"
        BuildDir     = "$ProjectRoot\build"
        InstallerDir = "$ProjectRoot\installer"
        Manifest     = $Manifest
    }
}

function Show-Banner {
    Clear-Host
    Write-Host ""
    Write-Host "██╗   ██╗██╗██████╗ ████████╗██╗   ██╗ █████╗  ██████╗ █████╗ ███╗   ███╗" -ForegroundColor Cyan
    Write-Host "██║   ██║██║██╔══██╗╚══██╔══╝██║   ██║██╔══██╗██╔════╝██╔══██╗████╗ ████║" -ForegroundColor Cyan
    Write-Host "██║   ██║██║██████╔╝   ██║   ██║   ██║███████║██║     ███████║██╔████╔██║" -ForegroundColor Cyan
    Write-Host "╚██╗ ██╔╝██║██╔══██╗   ██║   ██║   ██║██╔══██║██║     ██╔══██║██║╚██╔╝██║" -ForegroundColor Cyan
    Write-Host " ╚████╔╝ ██║██║  ██║   ██║   ╚██████╔╝██║  ██║╚██████╗██║  ██║██║ ╚═╝ ██║" -ForegroundColor Cyan
    Write-Host "  ╚═══╝  ╚═╝╚═╝  ╚═╝   ╚═╝    ╚═════╝ ╚═╝  ╚═╝ ╚═════╝╚═╝  ╚═╝╚═╝     ╚═╝" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  PowerShell $($PSVersionTable.PSVersion)  |  $ProjectRoot" -ForegroundColor DarkGray
    Write-Host ""
}

function Show-Dependencies {
    Write-Host "  ── What you need ───────────────────────────────────────────────────────" -ForegroundColor DarkGray
    Write-Host ""

    $col1 = 20
    $col2 = 10

    foreach ($key in $Manifest.Keys) {
        $dep = $Manifest[$key]
        $label   = $dep.Label.PadRight($col1)
        $version = "≥ $($dep.MinVersion.Major).$($dep.MinVersion.Minor)".PadRight($col2)
        Write-Host "  $label $version $($dep.Url)" -ForegroundColor White
        if ($dep.Note) {
            Write-Host "  $(' ' * ($col1 + $col2 + 1)) ↑ $($dep.Note)" -ForegroundColor DarkGray
        }
        if ($dep.WingetId) {
            Write-Host "  $(' ' * ($col1 + $col2 + 1)) winget install $($dep.WingetId)" -ForegroundColor DarkGray
        }
    }

    Write-Host ""
    Write-Host "  ────────────────────────────────────────────────────────────────────────" -ForegroundColor DarkGray
    Write-Host ""
}

function Show-Menu {
    $status = Get-PreflightStatus
    $preflightTag = switch ($status) {
        'passed' { "  ✅ passed" }
        'failed' { "  ❌ last run failed" }
        default  { "  ⚠  not yet run" }
    }
    $buildTag  = if ($status -eq 'passed') { "" } else { "  ⚠  preflight required" }
    $buildColor = if ($status -eq 'passed') { 'White' } else { 'DarkGray' }

    Write-Host "  What would you like to do?" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "    [1]  Unblock scripts" -ForegroundColor White
    Write-Host "    [2]  Preflight checks$preflightTag" -ForegroundColor White
    Write-Host "    [3]  Install dependencies" -ForegroundColor White
    Write-Host "    [4]  Build$buildTag" -ForegroundColor $buildColor
    Write-Host "    [5]  Create installer" -ForegroundColor DarkGray
    Write-Host "    [Q]  Quit" -ForegroundColor DarkGray
    Write-Host ""
}

# ---------------------------------------------------------------------------
# STEP 1 — UNBLOCK
# ---------------------------------------------------------------------------
function Invoke-Unblock {
    Write-Host ""
    Write-Host "  ── [1] Unblock Scripts ─────────────────────────────────────────────────" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  When you download files from the internet, Windows attaches a" -ForegroundColor DarkGray
    Write-Host "  'Mark of the Web' (MOTW) tag that blocks scripts from running." -ForegroundColor DarkGray
    Write-Host "  This step removes those tags from all .ps1 / .bat / .iss files" -ForegroundColor DarkGray
    Write-Host "  in this repo so setup can proceed normally." -ForegroundColor DarkGray
    Write-Host "  You only need to do this once after cloning." -ForegroundColor DarkGray
    Write-Host ""

    $files = Get-ChildItem $ProjectRoot -Recurse -Include "*.ps1","*.bat","*.iss" -ErrorAction SilentlyContinue
    $count = 0
    foreach ($f in $files) {
        try {
            Unblock-File $f.FullName -ErrorAction SilentlyContinue
            Write-Host "  + $($f.Name)" -ForegroundColor DarkGray
            $count++
        } catch {}
    }

    Write-Host ""
    Write-Host "  ✅ $count files unblocked." -ForegroundColor Green
    Write-Host ""
    Start-Sleep -Milliseconds 800
}

# ---------------------------------------------------------------------------
# STEP 2 — PREFLIGHT
# ---------------------------------------------------------------------------
function Invoke-Preflight {
    Write-Host ""
    & "$ScriptsDir\setup_preflight.ps1" -ProjectRoot $ProjectRoot -ConfigFile $ConfigFile -Manifest $Manifest
}

# ---------------------------------------------------------------------------
# STEP 3 — INSTALL DEPENDENCIES
# ---------------------------------------------------------------------------
function Invoke-InstallDeps {
    Write-Host ""
    & "$ScriptsDir\setup_deps.ps1" -Manifest $Manifest
    Write-Host ""
    Write-Host "  Press any key to return to menu..." -ForegroundColor DarkGray
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
}

# ---------------------------------------------------------------------------
# STEP 4 — BUILD
# ---------------------------------------------------------------------------
function Invoke-Build {
    if (-not (Test-PreflightPassed)) {
        Write-Host ""
        Write-Host "  ✋ Preflight has not been run or has unresolved failures." -ForegroundColor Red
        Write-Host "     Run [2] Preflight checks before building." -ForegroundColor DarkYellow
        Write-Host ""
        Write-Host "  Press any key to return to menu..." -ForegroundColor DarkGray
        $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
        return
    }

    Write-Host ""
    $Paths = Build-Paths
    & "$ScriptsDir\build_virtuacam.ps1" -Paths $Paths
    Write-Host ""
    Write-Host "  Press any key to return to menu..." -ForegroundColor DarkGray
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
}

# ---------------------------------------------------------------------------
# STEP 5 — CREATE INSTALLER
# ---------------------------------------------------------------------------
function Invoke-CreateInstaller {
    if (-not (Test-PreflightPassed)) {
        Write-Host ""
        Write-Host "  ✋ Preflight has not been run or has unresolved failures." -ForegroundColor Red
        Write-Host "     Run [2] Preflight checks before creating an installer." -ForegroundColor DarkYellow
        Write-Host ""
        Write-Host "  Press any key to return to menu..." -ForegroundColor DarkGray
        $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
        return
    }

    Write-Host ""
    $Paths = Build-Paths
    & "$ScriptsDir\create_installer.ps1" -Paths $Paths
    Write-Host ""
    Write-Host "  Press any key to return to menu..." -ForegroundColor DarkGray
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
}

# ---------------------------------------------------------------------------
# MENU LOOP
# ---------------------------------------------------------------------------
do {
    Show-Banner
    Show-Dependencies
    Show-Menu

    $choice = Read-Host "  Enter selection"
    Write-Host ""

    switch ($choice.Trim().ToUpper()) {
        "1" { Invoke-Unblock }
        "2" { Invoke-Preflight }
        "3" { Invoke-InstallDeps }
        "4" { Invoke-Build }
        "5" { Invoke-CreateInstaller }
        "Q" {
            Write-Host "  Bye." -ForegroundColor DarkGray
            Write-Host ""
            exit 0
        }
        default {
            Write-Host "  Invalid selection." -ForegroundColor Red
            Start-Sleep -Seconds 1
        }
    }

} while ($true)
