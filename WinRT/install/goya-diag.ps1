<#
  goya-diag.ps1 - inspect / change the Windows RT installation on the goyawifi microSD card OFFLINE,
  from a Windows 10/11 PC (elevated). Use through goya-diag.cmd.

    goya-diag.cmd <W:> info                    verifier, Goya services, crash settings, last boot log lines
    goya-diag.cmd <W:> collect                 copy ntbtlog, minidumps, setup logs, diag.txt to .\goya-collect-<time>
    goya-diag.cmd <W:> verifier-off            remove Driver Verifier settings from the image
    goya-diag.cmd <W:> bootlog on|off          \Windows\ntbtlog.txt lists every driver loaded (last line = crash point)
    goya-diag.cmd <W:> minidump on             small crash dumps to \Windows\Minidump (needs a page file; set)
    goya-diag.cmd <W:> disable <service|all>   Start=4 (goyasdhc, goyatouch, goyakeys, goyai2c, goyagpio, goyapinmux)
    goya-diag.cmd <W:> enable <service|all>    back to the INF start type
    goya-diag.cmd <W:> only <service>          enable just this Goya driver, disable the others (WinPE: goyasdhc too)
    goya-diag.cmd <W:> bsod-details            stop screen stays up (no auto restart) and shows the 4 parameters
    goya-diag.cmd <W:> win-files               copy the image's GpioClx/SpbCx/KMDF/Storport/HID drivers into a zip
    goya-diag.cmd <W:> halext off|on           experiment: boot without / with HalExtMmpTimer (OSEDB entry)
    goya-diag.cmd <W:> halext-files            copy System32\HalExt*.dll, hal.dll, ntoskrnl.exe + versions to .\goya-halext-<time>
    goya-diag.cmd <W:> halext-install <dll>    put a (signed) HalExtMmpTimer.dll into System32 and register it

  <W:> is the card as it appears on the PC: either the Windows partition of a full Windows RT install, or
  a WinPE card (\sources\boot.wim). On a WinPE card the registry changes are made inside boot.wim (it is
  mounted with DISM, changed and committed; takes a minute). The BCD is found automatically.
#>
param(
    [Parameter(Mandatory = $true)][string]$Win,
    [Parameter(Mandatory = $true)][string]$Action,
    [string]$Arg
)
$ErrorActionPreference = 'Stop'
$Win = $Win.TrimEnd('\').TrimEnd(':') + ':'
$Mount = Join-Path $env:TEMP 'goyadiag-mount'
$IsPE = $false
if (Test-Path "$Win\Windows\System32\config\SYSTEM") {
    $Root = $Win
} elseif (Test-Path "$Win\sources\boot.wim") {
    $IsPE = $true
    $Root = $Mount
} else {
    throw "$Win holds neither a Windows installation (\Windows) nor a WinPE card (\sources\boot.wim)"
}
# Native tools through cmd.exe with their own redirection: Windows PowerShell 5.1 turns any stderr
# output of a native program into a terminating error under ErrorActionPreference=Stop, even for
# expected failures (e.g. unloading a hive that is not loaded). Success is judged by the exit code.
function Run([string]$CmdLine, [switch]$Show) {
    if ($Show) { $out = cmd.exe /c "$CmdLine 2>&1"; $out | ForEach-Object { Write-Host $_ } }
    else { $out = cmd.exe /c "$CmdLine 2>&1" }
    $script:RunOutput = $out
    return $LASTEXITCODE
}

$Changes = @('verifier-off', 'bsod-details', 'minidump', 'disable', 'enable', 'only', 'halext', 'halext-install')

function Mount-PE {
    if (-not $IsPE) { return }
    [void](Run "dism.exe /English /Unmount-Image /MountDir:""$Mount"" /Discard")
    if (Test-Path $Mount) { Remove-Item $Mount -Recurse -Force -ErrorAction SilentlyContinue }
    New-Item -ItemType Directory $Mount | Out-Null
    [void](Run "attrib.exe -r ""$Win\sources\boot.wim""")
    Write-Host "mounting $Win\sources\boot.wim ..."
    $rc = Run "dism.exe /English /Mount-Image /ImageFile:""$Win\sources\boot.wim"" /Index:1 /MountDir:""$Mount"""
    if ($rc) { $script:RunOutput | Select-Object -Last 5 | ForEach-Object { Write-Host $_ }; throw "DISM could not mount $Win\sources\boot.wim (exit $rc)" }
}

function Dismount-PE([bool]$Commit) {
    if (-not $IsPE) { return }
    $mode = if ($Commit) { '/Commit' } else { '/Discard' }
    Write-Host "unmounting boot.wim ($mode) ..."
    $rc = Run "dism.exe /English /Unmount-Image /MountDir:""$Mount"" $mode"
    if ($rc) { $script:RunOutput | Select-Object -Last 5 | ForEach-Object { Write-Host $_ }; throw "DISM unmount failed (exit $LASTEXITCODE): run 'dism /Cleanup-Wim' and try again" }
}

$GoyaServices = [ordered]@{ goyasdhc = 0; goyapinmux = 3; goyagpio = 3; goyai2c = 3; goyatouch = 3; goyakeys = 3; mshidkmdf = 3 }
$HiveName = 'GOYADIAG_SYSTEM'

function With-SystemHive([scriptblock]$Body) {
    [void](Run "reg.exe unload HKLM\$HiveName")
    $rc = Run "reg.exe load HKLM\$HiveName ""$Root\Windows\System32\config\SYSTEM"""
    if ($rc) { $script:RunOutput | ForEach-Object { Write-Host $_ }; throw "cannot load the SYSTEM hive of $Win (is Windows on the card still mounted elsewhere?)" }
    try {
        $sel = (Get-ItemProperty "HKLM:\$HiveName\Select").Current
        $cs = "HKLM:\$HiveName\ControlSet{0:d3}" -f $sel
        & $Body $cs
    } finally {
        [gc]::Collect(); [gc]::WaitForPendingFinalizers()
        if (Run "reg.exe unload HKLM\$HiveName") { Start-Sleep 2; [gc]::Collect(); [void](Run "reg.exe unload HKLM\$HiveName") }
    }
}

function Find-Bcd {
    if (Test-Path "$Win\efi\microsoft\boot\BCD") { return @{ Bcd = "$Win\efi\microsoft\boot\BCD"; Added = $false } }
    $disk = (Get-Partition -DriveLetter $Win.TrimEnd(':')).DiskNumber
    foreach ($p in Get-Partition -DiskNumber $disk) {
        $isSys = ($p.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}') -or ($p.IsActive -and $p.Size -lt 1GB)
        if (-not $isSys) { continue }
        $letter = $p.DriveLetter
        $added = $false
        if (-not $letter) {
            $letter = [char[]](83..90 + 77..82) | Where-Object { -not (Test-Path "$($_):\") } | Select-Object -First 1
            Add-PartitionAccessPath -DiskNumber $disk -PartitionNumber $p.PartitionNumber -AccessPath "$($letter):\"
            $added = $true
        }
        $bcd = "$($letter):\EFI\Microsoft\Boot\BCD"
        if (Test-Path $bcd) { return @{ Bcd = $bcd; Letter = $letter; Disk = $disk; Part = $p.PartitionNumber; Added = $added } }
        if ($added) { Remove-PartitionAccessPath -DiskNumber $disk -PartitionNumber $p.PartitionNumber -AccessPath "$($letter):\" }
    }
    throw "no SYSTEM partition with \EFI\Microsoft\Boot\BCD on the disk of $Win"
}

function Release-Bcd($b) {
    if ($b.Added) { Remove-PartitionAccessPath -DiskNumber $b.Disk -PartitionNumber $b.Part -AccessPath "$($b.Letter):\" }
}

$act = $Action.ToLower()
if ($IsPE -and $act -eq 'minidump') {
    throw "'$Action' does not apply to a WinPE card (WinPE runs from RAM, there is no page file)"
}
if ($act -in $Changes -or $act -in 'info', 'halext-files', 'win-files') { Mount-PE }
$ok = $false
try {
switch ($act) {
    'info' {
        With-SystemHive {
            param($cs)
            $mm = Get-ItemProperty "$cs\Control\Session Manager\Memory Management"
            Write-Host "Driver Verifier: VerifyDrivers='$($mm.VerifyDrivers)' VerifyDriverLevel=$($mm.VerifyDriverLevel)"
            if ($mm.VerifyDrivers -or $mm.VerifyDriverLevel) { Write-Host '  -> verifier is configured: run "verifier-off"' -ForegroundColor Yellow }
            foreach ($s in $GoyaServices.Keys) {
                $k = "$cs\Services\$s"
                if (Test-Path $k) { $v = Get-ItemProperty $k; Write-Host ("service {0,-11} Start={1} ImagePath={2}" -f $s, $v.Start, $v.ImagePath) }
                else { Write-Host ("service {0,-11} (not installed yet - PnP installs it on first boot)" -f $s) }
            }
            $osedb = "$cs\Control\OSExtensionDatabase"
            if (Test-Path $osedb) {
                Get-ChildItem $osedb -Recurse | ForEach-Object {
                    $v = (Get-ItemProperty $_.PSPath).HALExtension
                    if (-not $v) { Write-Host "OSEDB $($_.PSChildName): (no HALExtension value)"; return }
                    $file = if ($v -match '[\\:]') { $null } else { "$Root\Windows\System32\$v" }
                    $state = if ($file -and (Test-Path $file)) { 'file found in System32' } elseif ($file) { 'FILE MISSING in System32' } else { 'NOT a bare file name - run "halext on" to fix' }
                    Write-Host "OSEDB $($_.PSChildName): $v   [$state]"
                }
            }
            $files = @(Get-ChildItem "$Root\Windows\System32\HalExtMmpTimer.dll", "$Root\Windows\System32\drivers\goya*.sys", "$Root\Windows\System32\goyaosk.exe" -ErrorAction SilentlyContinue)
            foreach ($f in $files) {
                $sig = Get-AuthenticodeSignature $f.FullName
                $who = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { 'NOT SIGNED' }
                Write-Host ("file {0,-20} {1,7} bytes  {2}" -f $f.Name, $f.Length, $who)
                if (-not $sig.SignerCertificate -and $f.Name -eq 'HalExtMmpTimer.dll') { Write-Host '  -> unsigned HAL extensions are never loaded: run "halext-install <signed HalExtMmpTimer.dll>"' -ForegroundColor Yellow }
            }
            $cc = Get-ItemProperty "$cs\Control\CrashControl" -ErrorAction SilentlyContinue
            Write-Host "CrashControl: CrashDumpEnabled=$($cc.CrashDumpEnabled) MinidumpDir=$($cc.MinidumpDir)"
        }
        if ($IsPE) { Write-Host 'WinPE card: boot log and dumps stay in RAM (X:), see \goyape\diag.txt and \goyaosk.log on the card' }
        $log = "$Win\Windows\ntbtlog.txt"
        if (Test-Path $log) { Write-Host "--- last lines of ntbtlog.txt (last 'Loaded' = last driver that loaded):"; Get-Content $log -Encoding Unicode | Select-Object -Last 12 }
        else { Write-Host 'no ntbtlog.txt yet (run "bootlog on" and boot once)' }
        $md = Get-ChildItem "$Win\Windows\Minidump" -ErrorAction SilentlyContinue
        Write-Host "minidumps: $(@($md).Count)"; $md | Format-Table Name, Length, LastWriteTime
    }
    'collect' {
        $out = Join-Path (Get-Location) ("goya-collect-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory $out | Out-Null
        foreach ($f in 'Windows\ntbtlog.txt', 'Windows\Panther\setupact.log', 'Windows\Panther\setuperr.log',
                       'Windows\Panther\UnattendGC\setupact.log', 'Windows\inf\setupapi.dev.log', 'Windows\inf\setupapi.offline.log',
                       'goya\diag.txt', 'Windows\System32\LogFiles\Srt\SrtTrail.txt',
                       'goyape\diag.txt', 'goyaosk.log') {
            if (Test-Path "$Win\$f") { Copy-Item "$Win\$f" (Join-Path $out ($f -replace '\\', '_')) }
        }
        if (Test-Path "$Win\Windows\Minidump") { Copy-Item "$Win\Windows\Minidump" (Join-Path $out 'Minidump') -Recurse }
        if (Test-Path "$Win\Windows\MEMORY.DMP") { Write-Host "MEMORY.DMP present ($((Get-Item "$Win\Windows\MEMORY.DMP").Length) bytes), not copied" }
        Write-Host "collected into $out"; Get-ChildItem $out -Recurse | Format-Table FullName, Length
    }
    'verifier-off' {
        With-SystemHive {
            param($cs)
            $k = "$cs\Control\Session Manager\Memory Management"
            foreach ($n in 'VerifyDrivers', 'VerifyDriverLevel', 'VerifierOptions', 'VerifierFlags') { Remove-ItemProperty $k -Name $n -ErrorAction SilentlyContinue }
            Write-Host 'Driver Verifier settings removed'
        }
    }
    'bootlog' {
        $b = Find-Bcd
        try {
            if ($Arg -eq 'off') { [void](Run "bcdedit.exe /store ""$($b.Bcd)"" /deletevalue {default} bootlog" -Show) }
            else { [void](Run "bcdedit.exe /store ""$($b.Bcd)"" /set {default} bootlog yes" -Show) }
            [void](Run "bcdedit.exe /store ""$($b.Bcd)"" /enum {default}" -Show)
        } finally { Release-Bcd $b }
    }
    'minidump' {
        With-SystemHive {
            param($cs)
            $k = "$cs\Control\CrashControl"
            if (-not (Test-Path $k)) { New-Item $k -Force | Out-Null }
            Set-ItemProperty $k -Name CrashDumpEnabled -Value 3 -Type DWord
            Set-ItemProperty $k -Name MinidumpDir -Value '%SystemRoot%\Minidump' -Type ExpandString
            Set-ItemProperty $k -Name AutoReboot -Value 0 -Type DWord
            $mm = "$cs\Control\Session Manager\Memory Management"
            Set-ItemProperty $mm -Name PagingFiles -Value @('C:\pagefile.sys 256 512') -Type MultiString
            Write-Host 'minidumps on (C:\pagefile.sys 256-512 MB, no auto reboot). Open them with WinDbg: !analyze -v, lm'
        }
    }
    'bsod-details' {
        With-SystemHive {
            param($cs)
            $k = "$cs\Control\CrashControl"
            if (-not (Test-Path $k)) { New-Item $k -Force | Out-Null }
            Set-ItemProperty $k -Name AutoReboot -Value 0 -Type DWord
            Set-ItemProperty $k -Name DisplayParameters -Value 1 -Type DWord
            Write-Host 'stop screen: no automatic restart, stop code parameters shown (photo it, then hold Power)'
        }
    }
    { $_ -in 'disable', 'enable' } {
        # 'all' = every Goya driver: for finding which one crashes (disable all, then enable one per boot)
        # 'all' leaves goyagpio out (it crashes the 8520 GPIO class extension); name it explicitly to change it
        $names = if ($Arg -eq 'all') { @($GoyaServices.Keys | Where-Object { $_ -ne 'mshidkmdf' -and ($_ -ne 'goyagpio' -or $Action -eq 'disable') }) } else { @($Arg) }
        foreach ($n in $names) {
            if (-not $n -or -not $GoyaServices.Contains($n)) { throw "service must be 'all' or one of: $($GoyaServices.Keys -join ', ')" }
        }
        # A WinPE card boots from boot.wim in RAM, so goyasdhc is not needed to boot it; a full install boots from it
        if (-not $IsPE -and $Action -eq 'disable' -and $names -contains 'goyasdhc') {
            if ($Arg -eq 'all') { $names = @($names | Where-Object { $_ -ne 'goyasdhc' }) }
            else { throw 'goyasdhc is the boot disk driver of a full install: disabling it makes Windows unbootable' }
        }
        With-SystemHive {
            param($cs)
            foreach ($n in $names) {
                $k = "$cs\Services\$n"
                if (-not (Test-Path $k)) { Write-Host "$n is not installed in this image (skipped)"; continue }
                $start = if ($Action -eq 'disable') { 4 } else { $GoyaServices[$n] }
                Set-ItemProperty $k -Name Start -Value $start -Type DWord
                Write-Host "$n Start=$start"
            }
        }
    }
    'only' {
        if (-not $Arg -or -not $GoyaServices.Contains($Arg) -or $Arg -eq 'mshidkmdf') { throw "service must be one of: $(($GoyaServices.Keys | Where-Object { $_ -ne 'mshidkmdf' }) -join ', ')" }
        With-SystemHive {
            param($cs)
            foreach ($n in @($GoyaServices.Keys | Where-Object { $_ -ne 'mshidkmdf' })) {
                $k = "$cs\Services\$n"
                if (-not (Test-Path $k)) { continue }
                if ($n -eq $Arg) { $start = $GoyaServices[$n] }
                elseif ($n -eq 'goyasdhc' -and -not $IsPE) { continue }   # boot disk of a full install stays on
                else { $start = 4 }
                Set-ItemProperty $k -Name Start -Value $start -Type DWord
                Write-Host "$n Start=$start"
            }
        }
    }
    'halext' {
        With-SystemHive {
            param($cs)
            $k = "$cs\Control\OSExtensionDatabase\VEN_MRVL&DEV_0001"
            if ($Arg -eq 'off') {
                $v = (Get-ItemProperty $k -ErrorAction SilentlyContinue).HALExtension
                if ($v) { Set-ItemProperty $k -Name HALExtension.goyaoff -Value $v; Remove-ItemProperty $k -Name HALExtension }
                Write-Host 'HAL extension disabled for the next boot (expect a different stop code if it is involved)'
            } else {
                if (-not (Test-Path $k)) { New-Item $k -Force | Out-Null }
                $v = 'HalExtMmpTimer.dll'      # bare name in System32, like the inbox HalExtPL310.dll entry
                if (-not (Test-Path "$Root\Windows\System32\$v")) { throw "$v is not in System32 of the image: rebuild the card with make-goya-sd.cmd / install-winrt-sd.cmd" }
                Set-ItemProperty $k -Name HALExtension -Value $v
                Remove-ItemProperty $k -Name HALExtension.goyaoff -ErrorAction SilentlyContinue
                Write-Host "HAL extension enabled: $v"
            }
        }
    }
    'win-files' {
        # The Windows driver frameworks our drivers plug into, from THIS image (8520 is pre-release:
        # its GpioClx/SpbCx/KMDF/Storport may differ from the final Windows 8 ones the drivers target)
        $out = Join-Path (Get-Location) ("goya-winfiles-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory $out | Out-Null
        $drv = "$Root\Windows\System32\drivers"
        $names = 'msgpioclx.sys', 'gpioclx0100.sys', 'spbcx.sys', 'wdfldr.sys', 'wdf01000.sys', 'storport.sys',
                 'mshidkmdf.sys', 'hidclass.sys', 'hidparse.sys', 'acpi.sys', 'acpiex.sys', 'disk.sys', 'classpnp.sys'
        $info = foreach ($n in $names) {
            $f = Join-Path $drv $n
            if (Test-Path $f) { Copy-Item $f $out; $i = Get-Item $f; "{0,-16} {1,9} bytes  {2}" -f $i.Name, $i.Length, $i.VersionInfo.FileVersion }
            else { "{0,-16} not in this image" -f $n }
        }
        $info | Set-Content (Join-Path $out 'versions.txt')
        $info | ForEach-Object { Write-Host $_ }
        $zip = "$out.zip"
        Compress-Archive -Path "$out\*" -DestinationPath $zip -Force
        Write-Host "send this file: $zip"
    }
    'halext-files' {
        $out = Join-Path (Get-Location) ("goya-halext-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory $out | Out-Null
        $sys = "$Root\Windows\System32"
        Get-ChildItem "$sys\HalExt*.dll" | ForEach-Object { Copy-Item $_.FullName $out }
        # kernel + HAL: checked for SMC / Generic Timer / system-register use (platform compatibility)
        foreach ($k in 'hal.dll', 'ntoskrnl.exe') { if (Test-Path "$sys\$k") { Copy-Item "$sys\$k" $out } }
        $info = foreach ($f in @(Get-ChildItem "$sys\hal*.dll") + @(Get-Item "$sys\ntoskrnl.exe")) {
            "{0,-22} {1,9} bytes  {2}" -f $f.Name, $f.Length, $f.VersionInfo.FileVersion
        }
        $info | Set-Content (Join-Path $out 'versions.txt')
        $info | ForEach-Object { Write-Host $_ }
        Write-Host "copied into $out :"; Get-ChildItem $out | Format-Table Name, Length
    }
    'halext-install' {
        if (-not $Arg -or -not (Test-Path $Arg)) { throw 'usage: halext-install <path to HalExtMmpTimer.dll>' }
        $sig = Get-AuthenticodeSignature $Arg
        if (-not $sig.SignerCertificate) { throw "$Arg is not signed: HAL extensions must be signed (use the goya-drivers-signed package)" }
        $eku = $sig.SignerCertificate.EnhancedKeyUsageList | ForEach-Object { $_.ObjectId }
        if ($eku -notcontains '1.3.6.1.4.1.311.61.5.1') { throw "the signing certificate of $Arg lacks the HAL extension EKU (1.3.6.1.4.1.311.61.5.1)" }
        Copy-Item $Arg "$Root\Windows\System32\HalExtMmpTimer.dll" -Force
        With-SystemHive {
            param($cs)
            $k = "$cs\Control\OSExtensionDatabase\VEN_MRVL&DEV_0001"
            if (-not (Test-Path $k)) { New-Item $k -Force | Out-Null }
            Set-ItemProperty $k -Name HALExtension -Value 'HalExtMmpTimer.dll'
            Remove-ItemProperty $k -Name HALExtension.goyaoff -ErrorAction SilentlyContinue
        }
        Write-Host "installed $Arg (signed by $($sig.SignerCertificate.Subject)) and registered HalExtMmpTimer.dll"
    }
    default { throw "unknown action '$Action' (info, collect, verifier-off, bootlog on|off, minidump on, disable|enable <service>, halext off|on, halext-files, halext-install <dll>, win-files, only <service>, bsod-details)" }
}
$ok = $true
} finally {
    if ($IsPE -and ($act -in $Changes -or $act -in 'info', 'halext-files', 'win-files')) { Dismount-PE ($ok -and ($act -in $Changes)) }
}
