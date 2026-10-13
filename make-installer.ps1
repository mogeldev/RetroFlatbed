<#
.SYNOPSIS
  Builds dist\RetroFlatbedSetup.exe - the GUI installer/uninstaller with everything embedded.

.DESCRIPTION
  1. builds the 64-bit and 32-bit binaries (build.bat, build.bat x86),
  2. stages the payload in dist\payload\,
  3. signs the driver catalogs with a self-signed code-signing certificate that lives in the
     current user's certificate store (created on first run; keep it to sign later releases
     with the same identity). Its public part (retroflatbed.cer) is embedded in the installer,
     which trusts it on the target PC so Windows accepts the driver packages,
  4. compiles installer\setup.c + resources into dist\RetroFlatbedSetup.exe.

  Needs Visual Studio 2022 Build Tools and the Windows SDK (makecat.exe). Does not need admin.
#>
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
$dist    = Join-Path $root 'dist'
$payload = Join-Path $dist 'payload'
$stage   = Join-Path $dist 'stage'
$subject = 'CN=RetroFlatbed driver (self-signed)'

function Run-Cmd([string]$line) {
    & $env:ComSpec /c $line
    if ($LASTEXITCODE -ne 0) { throw "command failed ($LASTEXITCODE): $line" }
}

if (-not $SkipBuild) {
    Write-Host '== building 64-bit'; Run-Cmd "`"$root\build.bat`" > nul"
    Write-Host '== building 32-bit'; Run-Cmd "`"$root\build.bat`" x86 > nul"
}

foreach ($f in 'build\RetroFlatbedWIA.dll', 'build\RetroFlatbed.ds', 'build\x86\RetroFlatbed.ds') {
    if (-not (Test-Path (Join-Path $root $f))) { throw "missing $f - run build.bat / build.bat x86" }
}

if (Test-Path $payload) { Remove-Item $payload -Recurse -Force }
if (Test-Path $stage)   { Remove-Item $stage   -Recurse -Force }
New-Item -ItemType Directory -Force $payload, $stage | Out-Null

# ---- payload files ----
Copy-Item "$root\driver\retroflatbed-wia.inf"    $payload
Copy-Item "$root\driver\retroflatbed-winusb.inf" $payload
Copy-Item "$root\build\RetroFlatbedWIA.dll"      $payload
Copy-Item "$root\build\RetroFlatbed.ds"          "$payload\RetroFlatbed64.ds"
Copy-Item "$root\build\x86\RetroFlatbed.ds"      "$payload\RetroFlatbed32.ds"
Copy-Item "$root\third_party\twain-dsm\win64\TWAINDSM.dll" "$payload\TWAINDSM64.dll"
Copy-Item "$root\third_party\twain-dsm\win32\TWAINDSM.dll" "$payload\TWAINDSM32.dll"
foreach ($d in 'TWAINDSM64.dll', 'TWAINDSM32.dll') {
    $s = Get-AuthenticodeSignature "$payload\$d"
    if ($s.Status -ne 'Valid') { throw "$d signature is $($s.Status)" }
}

# ---- certificate ----
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddMonths(6) } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
    Write-Host '== creating code-signing certificate in Cert:\CurrentUser\My'
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyUsage DigitalSignature -HashAlgorithm SHA256 -KeyLength 3072 -NotAfter (Get-Date).AddYears(10)
}
Export-Certificate -Cert $cert -FilePath "$payload\retroflatbed.cer" | Out-Null

# ---- signed catalogs ----
$makecat = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter makecat.exe -ErrorAction SilentlyContinue |
           Where-Object FullName -match '\\x64\\' | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $makecat) { throw 'makecat.exe not found (install the Windows SDK)' }

function New-SignedCatalog([string]$inf, [string[]]$extra) {
    $name = [IO.Path]::GetFileNameWithoutExtension($inf)
    $dir = Join-Path $stage $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item "$payload\$inf" $dir
    foreach ($e in $extra) { Copy-Item "$payload\$e" $dir }
    $cdf = "[CatalogHeader]`r`nName=$name.cat`r`nPublicVersion=0x0000001`r`nEncodingType=0x00010001`r`nCATATTR1=0x10010001:OSAttr:2:10.0`r`n`r`n[CatalogFiles]`r`n"
    foreach ($f in @($inf) + $extra) { $cdf += "<hash>$f=$f`r`n" }
    $cdf += "<hash>${inf}ATTR1=0x10010001:OSAttr:2:10.0`r`n"
    Set-Content "$dir\package.cdf" $cdf -Encoding ASCII
    Push-Location $dir
    & $makecat.FullName -v package.cdf | Out-Null
    Pop-Location
    if (-not (Test-Path "$dir\$name.cat")) { throw "makecat failed for $name" }
    $sig = Set-AuthenticodeSignature -FilePath "$dir\$name.cat" -Certificate $cert -HashAlgorithm SHA256
    if ($sig.Status -eq 'HashMismatch' -or $sig.Status -eq 'NotSigned') { throw "signing $name.cat failed: $($sig.Status)" }
    Copy-Item "$dir\$name.cat" $payload
    Write-Host "== signed $name.cat"
}
New-SignedCatalog 'retroflatbed-wia.inf'    @('RetroFlatbedWIA.dll')
New-SignedCatalog 'retroflatbed-winusb.inf' @()
Remove-Item $stage -Recurse -Force

# ---- installer executable ----
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$out = "$dist\RetroFlatbedSetup.exe"
$obj = "$dist\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
Run-Cmd ("call `"$vcvars`" >nul && cd /d `"$root\installer`" && " +
         "rc /nologo /fo `"$obj\setup.res`" setup.rc && " +
         "cl /nologo /O2 /MT /W3 /wd4996 /D_CRT_SECURE_NO_WARNINGS /Fo`"$obj\\`" setup.c `"$obj\setup.res`" /Fe:`"$out`" " +
         "/link /SUBSYSTEM:WINDOWS setupapi.lib newdev.lib crypt32.lib version.lib user32.lib advapi32.lib comctl32.lib shell32.lib gdi32.lib")
Remove-Item $obj -Recurse -Force

Write-Host ''
Write-Host "Built $out ($([int]((Get-Item $out).Length / 1KB)) KB)"
