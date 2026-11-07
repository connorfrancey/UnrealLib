@echo off
rem UnrealLib project manager wrapper (Windows cmd).
rem
rem   build.bat                 interactive menu
rem   build.bat all             configure + build + test the default preset
rem   build.bat --help          all commands
rem
rem Runs scripts\manage.py with uv; all arguments are passed through unchanged and the working directory is kept,
rem so relative paths (e.g. "build.bat tool inspector Core.u") refer to where you are.
setlocal

where uv >nul 2>nul
if errorlevel 1 (
    echo error: uv is not installed or not on PATH. 1>&2
    echo Install it from https://docs.astral.sh/uv/getting-started/installation/ : 1>&2
    echo   powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 ^| iex" 1>&2
    exit /b 127
)

uv run --script "%~dp0scripts\manage.py" %*
exit /b %ERRORLEVEL%
