@echo off
rem One-time setup after cloning: compiler, build, and the `fixit` command on PATH.
rem Double-click this file, or run it from a terminal.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
set "FIXIT_SETUP_EXIT=%ERRORLEVEL%"
rem Keep the window open on a double-click (no arguments), so the result can be read.
if "%~1"=="" pause
exit /b %FIXIT_SETUP_EXIT%