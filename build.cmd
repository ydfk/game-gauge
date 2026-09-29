@echo off
setlocal
rem Avoid inheriting PowerShell 7 module paths in Windows PowerShell 5.1.
set "PSModulePath="
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %errorlevel%
