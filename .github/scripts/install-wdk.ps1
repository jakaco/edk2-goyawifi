# Installs the Windows 10 2004 (19041) SDK if missing and the matching WDK, with retries.
# Chocolatey's community feed returns 503 now and then, so nothing here depends on it.
$ErrorActionPreference = 'Stop'
$kits  = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkOk = { Test-Path "$kits\Include\10.0.19041.0\um\windows.h" }
$wdkOk = { (Test-Path "$kits\Include\10.0.19041.0\km\ntddk.h") -and (Test-Path "$kits\Include\10.0.19041.0\wdf\kmdf") }

function Get-File($url, $out) {
  for ($i = 1; $i -le 5; $i++) {
    try { Invoke-WebRequest $url -OutFile $out -UseBasicParsing; return } catch { Write-Host "download attempt $i failed: $_"; Start-Sleep (5 * $i) }
  }
  throw "could not download $url"
}

function Wait-For($test, $what, $minutes) {
  $end = (Get-Date).AddMinutes($minutes)
  while ((Get-Date) -lt $end) { if (& $test) { return }; Start-Sleep 10 }
  throw "$what did not appear after $minutes minutes"
}

if (-not (& $sdkOk)) {
  Write-Host "Installing Windows SDK 19041"
  Get-File 'https://go.microsoft.com/fwlink/?linkid=2120843' sdksetup.exe
  Start-Process -Wait -FilePath .\sdksetup.exe -ArgumentList '/quiet /norestart /features OptionId.DesktopCPPARM OptionId.DesktopCPPx86 OptionId.WindowsDesktopDebuggers'
  Wait-For $sdkOk 'Windows SDK' 15
} else { Write-Host "Windows SDK 19041 already present" }

if (-not (& $wdkOk)) {
  Write-Host "Installing WDK 19041"
  Get-File 'https://go.microsoft.com/fwlink/?linkid=2128854' wdksetup.exe
  Start-Process -Wait -FilePath .\wdksetup.exe -ArgumentList '/quiet /norestart'
  Wait-For $wdkOk 'WDK' 20
} else { Write-Host "WDK 19041 already present" }

Get-ChildItem "$kits\Include\10.0.19041.0\wdf\kmdf" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }
Write-Host "ARM kernel libs of interest:"
Get-ChildItem "$kits\Lib\10.0.19041.0" -Recurse -Include halextlib.lib,gpioclx*.lib,spbcx*.lib,wdfldr.lib,wdfdriverentry.lib |
  Where-Object { $_.FullName -match '\\arm\\' } | ForEach-Object { $_.FullName }
