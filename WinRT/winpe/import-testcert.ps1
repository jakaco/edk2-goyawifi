<#
  import-testcert.ps1 -Cer GoyaTestCert.cer -Hive GOYART_SOFTWARE
  Adds the Goya test certificate to the ROOT and TrustedPublisher stores of an OFFLINE Windows
  image whose SOFTWARE hive is loaded at HKLM\<Hive>. The registry blob is produced through a
  temporary, untrusted per-user store ("GoyaTmp") that is deleted again: the PC's own trusted
  stores are never touched.
#>
param([Parameter(Mandatory = $true)][string]$Cer, [Parameter(Mandatory = $true)][string]$Hive)
$ErrorActionPreference = 'Stop'
$c = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 (Resolve-Path $Cer).Path
$tmp = New-Object System.Security.Cryptography.X509Certificates.X509Store 'GoyaTmp', 'CurrentUser'
$tmp.Open('ReadWrite'); $tmp.Add($c); $tmp.Close()
try {
    $blob = (Get-ItemProperty "HKCU:\Software\Microsoft\SystemCertificates\GoyaTmp\Certificates\$($c.Thumbprint)").Blob
    foreach ($store in 'ROOT', 'TrustedPublisher') {
        $k = "HKLM:\$Hive\Microsoft\SystemCertificates\$store\Certificates\$($c.Thumbprint)"
        New-Item -Path $k -Force | Out-Null
        New-ItemProperty -Path $k -Name Blob -PropertyType Binary -Value $blob -Force | Out-Null
    }
    Write-Host "  test certificate $($c.Thumbprint) trusted in the offline image (ROOT, TrustedPublisher)"
} finally {
    Remove-Item 'HKCU:\Software\Microsoft\SystemCertificates\GoyaTmp' -Recurse -Force -ErrorAction SilentlyContinue
}
