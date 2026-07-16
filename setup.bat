@echo off
:: VirtuaCam - First-run launcher
:: Elevates once, unblocks all scripts, then opens setup menu.
:: Double-click this after cloning. You won't need it again.

echo.
echo  VirtuaCam Build System
echo  Requesting elevation to initialize setup...
echo.

:: Check if pwsh (PowerShell 7) exists
where pwsh >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    :: PS7 found - launch setup.ps1 directly with elevation
    powershell -ExecutionPolicy Bypass -Command ^
      "Start-Process pwsh -ArgumentList '-ExecutionPolicy Bypass -File \"%~dp0setup.ps1\"' -Verb RunAs -Wait"
) else (
    :: PS7 not found - launch bootstrap mode in Windows PowerShell (5.1)
    :: setup.ps1 will detect this and offer to install PS7
    powershell -ExecutionPolicy Bypass -Command ^
      "Start-Process powershell -ArgumentList '-ExecutionPolicy Bypass -File \"%~dp0setup.ps1\" -Bootstrap' -Verb RunAs -Wait"
)

echo.
echo  Done. You can close this window.
pause
