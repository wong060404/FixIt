@echo off
rem Portable entry point for cmd.exe: forwards every argument to fixit.ps1,
rem which builds the tool on first use and finds the compiler by itself.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0fixit.ps1" %*
exit /b %ERRORLEVEL%