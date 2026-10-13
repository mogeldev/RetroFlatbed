<#
.SYNOPSIS
  Installs RetroFlatbed, the user-space driver for the Canon CanoScan LiDE 20 (WIA by default).

.DESCRIPTION
  Windows 11 only installs driver packages that carry a trusted signature, even when
  they merely bind the inbox WinUSB driver. This script therefore:
    1. creates a self-signed code-signing certificate (CN=RetroFlatbed driver),
    2. adds it to this machine's Trusted Root and Trusted Publishers stores,
    3. builds + signs a catalog for the driver package,
    4. replaces any earlier package (also ones installed under the former name) and installs the new one with pnputil,
    5. copies the TWAIN data sources to %WINDIR%\twain_64\RetroFlatbed\ (64-bit, build\) and
       %WINDIR%\twain_32\RetroFlatbed\ (32-bit, build\x86\ - needed by 32-bit TWAIN apps such as NAPS2)
       (skip with -SkipTwain), plus the bundled, Authenticode-signed TWAIN Data Source Managers
       twaindsm.dll to System32 / SysWOW64 unless an equal or newer one is present (-SkipDsm
       to opt out). -TwainOnly performs just this step, leaving the driver package untouched.
  Step 2 changes machine trust settings - remove it again with uninstall.ps1.

  Packages:
    default      retroflatbed-wia.inf  - WIA scanner ("Imaging devices"; Windows Scan, Paint,
                                  Fax and Scan ...) built on WinUSB, installs RetroFlatbedWIA.dll
    -WinUsbOnly  retroflatbed-winusb.inf     - only binds WinUSB (for retroflatbed-scan.exe / the TWAIN source)

  Alternative without a certificate: bind WinUSB with Zadig (https://zadig.akeo.ie).
  Must run elevated; it relaunches itself with a UAC prompt.
#>
param(
    [switch]$WinUsbOnly,
    [switch]$SkipTwain,
    [switch]$SkipDsm,
    [switch]$TwainOnly,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build')
)

$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $BuildDir = (Resolve-Path $BuildDir).Path
    $a = "-NoProfile -ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Path)`" -BuildDir `"$BuildDir`""
    if ($WinUsbOnly) { $a += ' -WinUsbOnly' }
    if ($SkipTwain)  { $a += ' -SkipTwain' }
    if ($SkipDsm)    { $a += ' -SkipDsm' }
    if ($TwainOnly)  { $a += ' -TwainOnly' }
    Start-Process powershell -Verb RunAs -ArgumentList $a -Wait
    exit
}

function Remove-OldPackages {
    # removes earlier packages of this project, including ones installed under the former name
    $text = & pnputil.exe /enum-drivers | Out-String
    foreach ($block in ($text -split "(\r?\n){2,}")) {
        if ($block -match 'Provider Name:\s+(RetroFlatbed|LiDE20 user-space driver)' -and $block -match '(oem\d+\.inf)') {
            Write-Host "Removing old package $($Matches[1])"
            & pnputil.exe /delete-driver $Matches[1] /uninstall /force | Out-Host
        }
    }
}

function Install-Dsm([string]$bits) {
    # 64-bit apps use System32\twaindsm.dll, 32-bit apps (e.g. NAPS2's TWAIN worker) SysWOW64\twaindsm.dll
    $src = Join-Path $PSScriptRoot "..\third_party\twain-dsm\win$bits\TWAINDSM.dll"
    $dir = if ($bits -eq '64') { 'System32' } else { 'SysWOW64' }
    $dst = Join-Path $env:WINDIR "$dir\twaindsm.dll"
    if (-not (Test-Path $src)) { Write-Warning "Bundled $bits-bit TWAIN DSM not found at $src"; return }
    $sig = Get-AuthenticodeSignature $src
    if ($sig.Status -ne 'Valid') { Write-Warning "Bundled $bits-bit TWAINDSM.dll signature is $($sig.Status) - not installing."; return }
    $newVer = [version](Get-Item $src).VersionInfo.FileVersion
    if (Test-Path $dst) {
        $curVer = [version](Get-Item $dst).VersionInfo.FileVersion
        if ($curVer -ge $newVer) { Write-Host "$bits-bit TWAIN DSM $curVer already installed in $dir - keeping it."; return }
    }
    Copy-Item $src $dst -Force
    Write-Host "$bits-bit TWAIN Data Source Manager $newVer installed to $dst"
}

function Install-Twain {
    # data sources: 64-bit -> twain_64, 32-bit -> twain_32 (each bitness only sees its own folder)
    $targets = @(
        @{ src = Join-Path $BuildDir 'RetroFlatbed.ds';      dir = 'twain_64'; bits = '64' },
        @{ src = Join-Path $BuildDir 'x86\RetroFlatbed.ds';  dir = 'twain_32'; bits = '32' }
    )
    foreach ($t in $targets) {
        $legacy = Join-Path $env:WINDIR "$($t.dir)\LiDE20"       # folder used before the rename
        if (Test-Path $legacy) { Remove-Item $legacy -Recurse -Force; Write-Host "Removed old data source folder $legacy" }
        if (Test-Path $t.src) {
            $dest = Join-Path $env:WINDIR "$($t.dir)\RetroFlatbed"
            New-Item -ItemType Directory -Force $dest | Out-Null
            Copy-Item $t.src (Join-Path $dest 'RetroFlatbed.ds') -Force
            Write-Host "$($t.bits)-bit TWAIN data source installed to $dest"
        } else {
            Write-Warning "$($t.bits)-bit RetroFlatbed.ds not found ($($t.src)) - build it with build.bat$(if ($t.bits -eq '32') { ' x86' })."
        }
        if (-not $SkipDsm) { Install-Dsm $t.bits }
    }
}
try {
    if ($TwainOnly) { Install-Twain; Read-Host 'Press Enter to close'; return }
    $infName = if ($WinUsbOnly) { 'retroflatbed-winusb.inf' } else { 'retroflatbed-wia.inf' }
    $catName = [IO.Path]::ChangeExtension($infName, '.cat')
    $subject = 'CN=RetroFlatbed driver (self-signed)'
    $work    = Join-Path $env:TEMP 'retroflatbed-driver'
    if (Test-Path $work) { Remove-Item $work -Recurse -Force }
    New-Item -ItemType Directory $work | Out-Null

    Copy-Item (Join-Path $PSScriptRoot $infName) $work
    $files = @($infName)
    if (-not $WinUsbOnly) {
        $dll = Join-Path $BuildDir 'RetroFlatbedWIA.dll'
        if (-not (Test-Path $dll)) { throw "RetroFlatbedWIA.dll not found in $BuildDir - run build.bat first." }
        Copy-Item $dll $work
        $files += 'RetroFlatbedWIA.dll'
    }

    $cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq $subject | Select-Object -First 1
    if (-not $cert) {
        Write-Host 'Creating self-signed code-signing certificate...'
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
            -CertStoreLocation Cert:\LocalMachine\My -KeyUsage DigitalSignature `
            -HashAlgorithm SHA256 -KeyLength 3072 -NotAfter (Get-Date).AddYears(10)
    }
    $cer = Join-Path $work 'retroflatbed.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    foreach ($store in 'Root', 'TrustedPublisher') {
        Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
    }

    $makecat = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter makecat.exe -ErrorAction SilentlyContinue |
               Where-Object FullName -match '\\x64\\' | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $makecat) { throw 'makecat.exe not found (install the Windows SDK).' }

    $cdf = "[CatalogHeader]`r`nName=$catName`r`nPublicVersion=0x0000001`r`nEncodingType=0x00010001`r`nCATATTR1=0x10010001:OSAttr:2:10.0`r`n`r`n[CatalogFiles]`r`n"
    foreach ($f in $files) { $cdf += "<hash>$f=$f`r`n" }
    $cdf += "<hash>${infName}ATTR1=0x10010001:OSAttr:2:10.0`r`n"
    Set-Content -Path (Join-Path $work 'package.cdf') -Value $cdf -Encoding ASCII
    Push-Location $work
    & $makecat.FullName -v package.cdf | Out-Host
    Pop-Location
    if (-not (Test-Path (Join-Path $work $catName))) { throw 'catalog creation failed' }

    $sig = Set-AuthenticodeSignature -FilePath (Join-Path $work $catName) -Certificate $cert -HashAlgorithm SHA256
    if ($sig.Status -ne 'Valid') { throw "catalog signature status: $($sig.Status) $($sig.StatusMessage)" }

    Remove-OldPackages

    Write-Host "Installing $infName ..."
    & pnputil.exe /add-driver (Join-Path $work $infName) /install | Out-Host
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil failed ($LASTEXITCODE)" }

    if (-not $SkipTwain) { Install-Twain }

    # drop the certificate created under the former project name (the old packages are gone)
    foreach ($store in 'Root', 'TrustedPublisher', 'My') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq 'CN=LiDE20 WinUSB driver (self-signed)' | Remove-Item -Force
    }

    Write-Host ''
    Write-Host 'Done. If the scanner does not show up, unplug and replug it.'
}
catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
}
Read-Host 'Press Enter to close'
