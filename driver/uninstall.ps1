<#
.SYNOPSIS
  Removes everything install.ps1 added: driver package, TWAIN data source, and the
  self-signed certificate from the machine trust stores.
#>
$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Start-Process powershell -Verb RunAs -Wait -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Path)`""
    exit
}

try {
    $subjects = 'CN=RetroFlatbed driver (self-signed)', 'CN=LiDE20 WinUSB driver (self-signed)'   # current + former name

    $drivers = & pnputil.exe /enum-drivers | Out-String
    foreach ($block in ($drivers -split "(\r?\n){2,}")) {
        if ($block -match 'Provider Name:\s+(RetroFlatbed|LiDE20 user-space driver)' -and $block -match '(oem\d+\.inf)') {
            Write-Host "Removing $($Matches[1])"
            & pnputil.exe /delete-driver $Matches[1] /uninstall /force | Out-Host
        }
    }

    foreach ($d in 'twain_64', 'twain_32') {
        foreach ($n in 'RetroFlatbed', 'LiDE20') {
            $dir = Join-Path $env:WINDIR "$d\$n"
            if (Test-Path $dir) { Remove-Item $dir -Recurse -Force; Write-Host "Removed TWAIN data source from $d\$n." }
        }
    }

    # remove a TWAIN DSM only if it is byte-identical to the one we bundle
    foreach ($e in @(@('64', 'System32'), @('32', 'SysWOW64'))) {
        $dsmSrc = Join-Path $PSScriptRoot "..\third_party\twain-dsm\win$($e[0])\TWAINDSM.dll"
        $dsmDst = Join-Path $env:WINDIR "$($e[1])\twaindsm.dll"
        if ((Test-Path $dsmSrc) -and (Test-Path $dsmDst) -and
            ((Get-FileHash $dsmSrc).Hash -eq (Get-FileHash $dsmDst).Hash)) {
            Remove-Item $dsmDst -Force
            Write-Host "Removed bundled $($e[0])-bit TWAIN DSM from $($e[1]) (other TWAIN apps will need their own)."
        }
    }

    foreach ($store in 'Root', 'TrustedPublisher', 'My') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $subjects -contains $_.Subject } | Remove-Item -Force
    }
    Write-Host 'Removed certificate from trust stores.'
}
catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
}
Read-Host 'Press Enter to close'
