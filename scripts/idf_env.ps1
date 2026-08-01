# Activate the ESP-IDF build environment in the current PowerShell session.
#
# WHY THIS EXISTS
# ---------------
# ESP-IDF's own `export.ps1` invokes a bare `python`. On a Windows machine that has the
# Microsoft Store Python *app-execution alias* enabled (the default on Windows 11), that
# resolves to a stub which prints "Python was not found..." and exits. export.ps1 then fails
# with:
#
#     The expression after '.' in a pipeline element produced an object that was not valid.
#     At ...\export.ps1:27 char:3
#
# ...and leaves the session unconfigured, which looks like a broken ESP-IDF install but is
# not. This script bypasses the problem by calling the IDF virtualenv's python.exe by
# absolute path and applying `idf_tools.py export` itself.
#
# USAGE (must be DOT-SOURCED so the variables survive the call):
#
#     . .\scripts\idf_env.ps1
#     idf.py set-target esp32p4
#     idf.py build
#
# Override the install location if yours differs:
#
#     . .\scripts\idf_env.ps1 -EspressifRoot D:\Espressif -IdfVersion v5.4.4
#
# The Start-menu "ESP-IDF 5.4.4 PowerShell" shortcut also works and does the same job; this
# script exists so that automation and non-interactive shells have a deterministic path.

[CmdletBinding()]
param(
    # IDF_TOOLS_PATH — where the installer put tools/, python_env/ and frameworks/.
    [string]$EspressifRoot = 'C:\Espressif',

    # Framework directory suffix, i.e. frameworks\esp-idf-<IdfVersion>.
    [string]$IdfVersion = 'v5.4.4',

    # Print the resolved tool versions after activating.
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'

$idfPath = Join-Path $EspressifRoot "frameworks\esp-idf-$IdfVersion"
if (-not (Test-Path $idfPath)) {
    Write-Error "ESP-IDF not found at $idfPath. Pass -EspressifRoot / -IdfVersion to point at your install."
    return
}

# Locate the IDF virtualenv's interpreter. The directory name encodes the IDF minor version
# and the bundled Python version (e.g. idf5.4_py3.11_env), so it is discovered rather than
# hard-coded.
$pythonEnvRoot = Join-Path $EspressifRoot 'python_env'
$python = Get-ChildItem -Path $pythonEnvRoot -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1

if (-not $python) {
    Write-Error "No ESP-IDF Python environment found under $pythonEnvRoot. Re-run the ESP-IDF installer."
    return
}

$env:IDF_TOOLS_PATH = $EspressifRoot
$env:IDF_PATH = $idfPath

# `idf_tools.py export --format key-value` emits NAME=VALUE lines. PATH arrives with a
# literal %PATH% placeholder that has to be substituted against the *current* PATH, which is
# the one part a naive parse gets wrong.
$exported = & $python (Join-Path $idfPath 'tools\idf_tools.py') export --format key-value
if ($LASTEXITCODE -ne 0) {
    Write-Error "idf_tools.py export failed (exit $LASTEXITCODE)."
    return
}

foreach ($line in $exported) {
    if ($line -match '^([A-Za-z_][A-Za-z_0-9]*)=(.*)$') {
        $name = $Matches[1]
        $value = $Matches[2]
        if ($name -eq 'PATH') {
            # Plain string substitution, NOT -replace: the existing PATH contains backslashes
            # and '$' is a regex/replacement metacharacter, so a regex replace would mangle it.
            $value = $value.Replace('%PATH%', $env:PATH)
        }
        Set-Item -Path "env:$name" -Value $value
    }
}

Write-Host "ESP-IDF $IdfVersion activated (IDF_PATH=$env:IDF_PATH)" -ForegroundColor Green

if ($Verify) {
    Write-Host '--- toolchain ---' -ForegroundColor Cyan
    # Each command's output is captured in full before taking the first line. Piping a native
    # executable straight into `Select-Object -First 1` terminates the pipeline early, which
    # kills the process and leaves $LASTEXITCODE non-zero for no real reason.
    $checks = [ordered]@{
        'riscv32-esp-elf-gcc' = { riscv32-esp-elf-gcc --version }
        'cmake'               = { cmake --version }
        'ninja'               = { ninja --version }
        'python'              = { python --version }
        'esptool'             = { python -m esptool version }
    }
    foreach ($name in $checks.Keys) {
        $lines = @(& $checks[$name] 2>&1)
        $first = if ($lines.Count -gt 0) { $lines[0] } else { '(no output)' }
        Write-Host ("  {0,-20} {1}" -f $name, $first)
    }
    $global:LASTEXITCODE = 0
}

# ---------------------------------------------------------------------------------------
# REMINDER: ESP-IDF cannot build from a path containing spaces (not IDF_PATH, not the
# project directory, not a component directory). Keep the project somewhere like
# C:\dev\m5Dash. See docs/IMPLEMENTATION_PLAN.md §1.1.
# ---------------------------------------------------------------------------------------
