# Installs the Windows 10 2004 (19041) SDK + matching WDK on a GitHub Windows runner.
#
# Lessons from the CI logs:
#  * Chocolatey's community feed returns 503 now and then -> retry it, then fall back to
#    Microsoft's own SDK installer.
#  * The WDK 2004 installer only installs when the SDK is the exact matching build (10.0.19041.685).
#    The runner image's preinstalled 19041 SDK is a different build: with it wdksetup.exe exits
#    after about a minute and installs nothing. So the matching SDK is installed first, always.
$ErrorActionPreference = 'Stop'
$kits  = "${env:ProgramFiles(x86)}\Windows Kits\10"
$wdkOk = { (Test-Path "$kits\Include\10.0.19041.0\km\ntddk.h") -and (Test-Path "$kits\Include\10.0.19041.0\wdf\kmdf") }

function Get-File($url, $out) {
  for ($i = 1; $i -le 5; $i++) {
    try { Invoke-WebRequest $url -OutFile $out -UseBasicParsing; return } catch { Write-Host "download attempt $i failed: $_"; Start-Sleep (5 * $i) }
  }
  throw "could not download $url"
}

function Show-State {
  Write-Host "---- diagnostics ($(Get-Date)) ----"
  Get-ChildItem "$kits\Include" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ("Include\" + $_.Name) }
  Write-Host "ntddk.h / wdf.h / gpioclx.h / nthalext.h under Windows Kits:"
  Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits" -Recurse -Include ntddk.h,wdf.h,gpioclx.h,nthalext.h -ErrorAction SilentlyContinue |
    Select-Object -First 12 | ForEach-Object { Write-Host $_.FullName }
  Write-Host "installed SDK/WDK products:"
  Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*','HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue |
    Where-Object { $_.DisplayName -match 'Windows Software Development Kit|Driver Kit|WDK' } | ForEach-Object { Write-Host ($_.DisplayName + "  " + $_.DisplayVersion) }
  foreach ($l in 'wdk-setup.log') { if (Test-Path $l) { Write-Host "== $l"; Get-Content $l -Tail 40 } }
  Get-ChildItem $env:TEMP -Filter '*wdk*.log' -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ("== " + $_.FullName); Get-Content $_.FullName -Tail 30 }
}

if (-not (& $wdkOk)) {
  # 1. matching SDK
  $done = $false
  for ($i = 1; $i -le 4 -and -not $done; $i++) {
    choco install windows-sdk-10-version-2004-all -y --no-progress --version 10.0.19041.685 --force
    if ($LASTEXITCODE -eq 0) { $done = $true } else { Write-Host "choco attempt $i failed ($LASTEXITCODE)"; Start-Sleep (10 * $i) }
  }
  if (-not $done) {
    Write-Host "Chocolatey unavailable, using Microsoft's SDK installer"
    Get-File 'https://go.microsoft.com/fwlink/?linkid=2120843' sdksetup.exe
    $p = Start-Process -PassThru -Wait -FilePath .\sdksetup.exe -ArgumentList '/quiet /norestart'
    Write-Host ("sdksetup exit code " + $p.ExitCode)
  }

  # 2. WDK
  Get-File 'https://go.microsoft.com/fwlink/?linkid=2128854' wdksetup.exe
  $p = Start-Process -PassThru -Wait -FilePath .\wdksetup.exe -ArgumentList '/quiet /norestart /log wdk-setup.log'
  Write-Host ("wdksetup exit code " + $p.ExitCode + " at " + (Get-Date))

  $end = (Get-Date).AddMinutes(5)
  while (-not (& $wdkOk) -and (Get-Date) -lt $end) { Start-Sleep 10 }
  if (-not (& $wdkOk)) { Show-State; throw "WDK headers did not appear" }
} else { Write-Host "WDK 19041 already present" }

Write-Host "KMDF headers:"
Get-ChildItem "$kits\Include\10.0.19041.0\wdf\kmdf" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }
Write-Host "ARM kernel libs of interest:"
Get-ChildItem "$kits\Lib\10.0.19041.0" -Recurse -Include halextlib.lib,gpioclx*.lib,spbcx*.lib,wdfldr.lib,wdfdriverentry.lib |
  Where-Object { $_.FullName -match '\\arm\\' } | ForEach-Object { $_.FullName }
