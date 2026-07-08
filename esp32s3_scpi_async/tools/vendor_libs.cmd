@echo off
setlocal
cd /d "%~dp0.."
powershell -ExecutionPolicy Bypass -File ".\tools\vendor_libs.ps1"
endlocal
