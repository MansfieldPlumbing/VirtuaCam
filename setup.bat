@echo off
:: VirtuaCam - First-run launcher
:: Elevates once, unblocks all scripts, then opens setup menu.
:: Double-click this after cloning. You won't need it again.

echo.
echo  VirtuaCam Build System
echo  Requesting elevation to unblock downloaded scripts...
echo.

powershell -ExecutionPolicy Bypass -Command ^
  "Start-Process pwsh -ArgumentList '-ExecutionPolicy Bypass -File ""%~dp0setup.ps1""' -Verb RunAs -Wait"

echo.
echo  Done. You can close this window.
pause
