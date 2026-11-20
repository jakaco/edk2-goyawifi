<#
  sign-goya-drivers.ps1 - test-sign the Goya Windows RT drivers and build their catalogs.

    powershell -ExecutionPolicy Bypass -File sign-goya-drivers.ps1 -Dir <folder> [-Thumbprint <cert>]

  <folder> holds the *.inf and their *.sys / *.dll (goyagpio ... goyasdhc, HalExtMmpTimer).
  Without -Thumbprint a new self-signed test certificate is created in CurrentUser\My:
    - EKUs: Code Signing, Windows HAL Extension (1.3.6.1.4.1.311.61.5.1; winload refuses HAL
      extensions without it), Windows Hardware Driver Verification
    - valid from 2011-01-01: the Windows RT firmware starts the clock at 2012-08-01
  Every .sys/.dll gets an embedded signature (SHA-1 primary for old/pre-release builds, SHA-256
  appended); every .inf gets <name>.cat (makecat, SHA-1 catalog) signed with the same certificate.
  The public certificate is written to <folder>\GoyaTestCert.cer; the install scripts add it to the
  ROOT and TrustedPublisher stores of the offline Windows image only.
  Needs signtool.exe and makecat.exe from the Windows 10/11 SDK. No timestamp server is used.
#>
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string]$Thumbprint
)
$ErrorActionPreference = 'Stop'
$Dir = (Resolve-Path $Dir).Path

function Find-Tool($name) {
    $c = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\$name" -ErrorAction SilentlyContinue |
         Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $c) { throw "$name not found (install the Windows 10/11 SDK)" }
    return $c.FullName
}
$signtool = Find-Tool 'signtool.exe'
$makecat  = Find-Tool 'makecat.exe'
Write-Host "signtool: $signtool"
Write-Host "makecat:  $makecat"

$created = $false
if (-not $Thumbprint) {
    $cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=Goya Test Signing (goyawifi, not trusted)' `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3,1.3.6.1.4.1.311.61.5.1,1.3.6.1.4.1.311.10.3.5',
                         '2.5.29.19={text}false') `
        -NotBefore (Get-Date '2011-01-01') -NotAfter (Get-Date '2040-12-31') `
        -CertStoreLocation 'Cert:\CurrentUser\My'
    $Thumbprint = $cert.Thumbprint
    $created = $true
} else {
    $cert = Get-Item "Cert:\CurrentUser\My\$Thumbprint"
}
Export-Certificate -Cert $cert -FilePath (Join-Path $Dir 'GoyaTestCert.cer') -Type CERT | Out-Null
Write-Host "certificate: $($cert.Subject)  $Thumbprint  valid $($cert.NotBefore.ToString('yyyy-MM-dd')) .. $($cert.NotAfter.ToString('yyyy-MM-dd'))"

function Sign($file) {
    & $signtool sign /q /s My /sha1 $Thumbprint /fd sha1 $file
    if ($LASTEXITCODE) { throw "signtool (sha1) failed on $file" }
    & $signtool sign /q /as /s My /sha1 $Thumbprint /fd sha256 $file
    if ($LASTEXITCODE) { throw "signtool (sha256) failed on $file" }
}

Push-Location $Dir
try {
    $infs = Get-ChildItem -Filter *.inf
    if (-not $infs) { throw "no .inf in $Dir" }
    foreach ($inf in $infs) {
        $name = $inf.BaseName
        $bins = @(Get-ChildItem -File | Where-Object { $_.BaseName -eq $name -and ($_.Extension -in '.sys', '.dll') })
        if (-not $bins) { throw "$($inf.Name): no $name.sys / $name.dll next to it" }
        foreach ($b in $bins) { Sign $b.FullName }

        $cdf = "$name.cdf"
        $lines = @('[CatalogHeader]', "Name=$name.cat", 'ResultDir=.', 'PublicVersion=0x0000001',
                   'CatalogVersion=1', 'HashAlgorithms=SHA1', 'CATATTR1=0x10010001:OSAttr:2:6.2', '',
                   '[CatalogFiles]', "<hash>$($inf.Name)=$($inf.Name)")
        foreach ($b in $bins) { $lines += "<hash>$($b.Name)=$($b.Name)" }
        Set-Content -Path $cdf -Value $lines -Encoding ASCII
        & $makecat -v $cdf | Out-Null
        if ($LASTEXITCODE -or -not (Test-Path "$name.cat")) { throw "makecat failed for $name" }
        Remove-Item $cdf
        Sign (Join-Path $Dir "$name.cat")
        Write-Host ("  signed {0,-16} {1}" -f $name, ((@($bins.Name) + "$name.cat") -join ', '))
    }
    Get-ChildItem -File | Where-Object { $_.Extension -in '.sys', '.dll', '.cat' } | ForEach-Object {
        $s = Get-AuthenticodeSignature $_.FullName
        if (-not $s.SignerCertificate -or $s.SignerCertificate.Thumbprint -ne $Thumbprint) { throw "$($_.Name) is not signed" }
    }
    Write-Host 'all binaries and catalogs carry the Goya test signature (untrusted root: that is expected)'
} finally {
    Pop-Location
    if ($created) { Remove-Item "Cert:\CurrentUser\My\$Thumbprint" -DeleteKey -ErrorAction SilentlyContinue }
}
