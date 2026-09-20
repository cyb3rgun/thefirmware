<#
.SYNOPSIS
    Build, flash and monitor one thefirmware target.

.DESCRIPTION
    Fills in the CGTARGET, build directory and sdkconfig flags of D-002 so the
    flash method of D-004 fits on one line. Run it from an ESP-IDF PowerShell,
    or from any PowerShell after sourcing the ESP-IDF export script.

.EXAMPLE
    tools\cgflash.ps1 module COM6 build flash monitor
    tools\cgflash.ps1 pistolstub COM7 build flash monitor
    tools\cgflash.ps1 cambench COM8 monitor
    tools\cgflash.ps1 module COM6 erase-flash

.NOTES
    erase-flash wipes NVS: pairings, the stored channel and the zeroing
    offset are gone afterwards. The script asks before it erases.
#>
param(
    [Parameter(Mandatory = $true)][ValidateSet('module', 'pistolstub', 'cambench')]
    [string]$Target,

    [Parameter(Mandatory = $true)]
    [string]$Port,

    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Commands
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if ($Commands -contains 'erase-flash') {
    Write-Host ''
    Write-Host 'WARNING: erase-flash wipes the whole flash, NVS included.' -ForegroundColor Yellow
    Write-Host "Target '$Target' on $Port loses its pairings, its stored channel" -ForegroundColor Yellow
    Write-Host 'and its zeroing offset. They do not come back.' -ForegroundColor Yellow
    Write-Host ''
    $answer = Read-Host "Type the port name ($Port) to confirm"
    if ($answer -ne $Port) {
        Write-Host 'Not confirmed, nothing was erased.' -ForegroundColor Green
        exit 1
    }
}

$argv = @(
    '-D', "CGTARGET=$Target"
    '-B', (Join-Path $root "build/$Target")
    '-D', "SDKCONFIG=$(Join-Path $root "sdkconfig.$Target")"
    '-D', "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.$Target"
    '-p', $Port
) + $Commands

Write-Host "idf.py $($argv -join ' ')" -ForegroundColor Cyan
Push-Location $root
try {
    & idf.py @argv
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
