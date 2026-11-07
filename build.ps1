#Requires -Version 5.1
<#
.SYNOPSIS
    UnrealLib project manager wrapper (PowerShell on Windows, Linux or macOS).

.DESCRIPTION
    Runs scripts/manage.py with uv. All arguments are passed through unchanged and the working directory is kept,
    so relative paths (e.g. `./build.ps1 tool inspector Core.u`) refer to where you are.

.EXAMPLE
    ./build.ps1                # interactive menu
.EXAMPLE
    ./build.ps1 all            # configure + build + test the default preset
.EXAMPLE
    ./build.ps1 --help         # all commands
#>

$ErrorActionPreference = 'Stop'

if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
    Write-Error -ErrorAction Continue 'uv is not installed or not on PATH.'
    Write-Host 'Install it from https://docs.astral.sh/uv/getting-started/installation/ :'
    Write-Host '  powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"'
    exit 127
}

& uv run --script (Join-Path $PSScriptRoot 'scripts/manage.py') @args
exit $LASTEXITCODE
