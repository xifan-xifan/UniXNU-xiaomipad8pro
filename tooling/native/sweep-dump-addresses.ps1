# Which physical addresses can the reader's oem dump still serve? One fresh reader boot per
# address, one dump as the session's first command; cold from ABL. fastboot boot only, no writes.
# Why: docs/results-2026-11-25-dump-channel.md narrowed the channel to one address; the ufs266
# warm slot read at 0x8c5912880 faulted again on 2026-11-26, so probe-record readback needs the
# current address/health map before any more probe runs spend device slots.
param([string]$Serial='7ccdd855', [string]$Addrs='8c0004000:100,8c5912880:100,8c5912000:100,8c5000000:100', [int]$WaitBoot=150, [int]$WaitReader=150)
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'; $a = Join-Path $pt 'adb.exe'
$reader = Join-Path $root 'out\no-demura-control\reader.img'
$recover = Join-Path $PSScriptRoot 'ensure-hyperos.ps1'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dir = Join-Path $root ("logs\native-runs\dump-sweep-$stamp")
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'run.log'
function Line($m){ $line = "$((Get-Date).ToString('HH:mm:ss.fff')) $m"; Add-Content -LiteralPath $log -Value $line; $line }
function ShaH($p){ if(!(Test-Path $p)){ return '' }; (Get-FileHash -LiteralPath $p).Hash.ToLower() }
function FastDevices { ((& $f devices 2>&1 | Out-String)) }
function AdbDevices { ((& $a devices 2>&1 | Out-String)) }
function WaitFast([string]$s,[int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match "(?m)^$s\s+fastboot"){ return $true }; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
function WaitReader([int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match '(?m)^piano-uefi\s+fastboot'){ return $true }; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
$results = @()
try {
  if((ShaH $reader) -ne '708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468'){ Line 'reader identity mismatch'; exit 1 }
  # The reader has to be booted from ABL, so an adb device is walked into the loader first.
  $s = ''
  foreach($x in [regex]::Matches((FastDevices), '(?m)^([0-9a-fA-F]{8})\s+fastboot')){ if($Serial -eq '' -or $x.Groups[1].Value -eq $Serial){ $s = $x.Groups[1].Value } }
  if($s -eq ''){
    foreach($x in [regex]::Matches((AdbDevices), '(?m)^([0-9a-fA-F]{8})\s+device')){ if($Serial -eq '' -or $x.Groups[1].Value -eq $Serial){ $s = $x.Groups[1].Value } }
    if($s -ne ''){
      $r = (& $a -s $s reboot bootloader 2>&1 | Out-String)
      Line ('on adb; rebooting into ABL: ' + ($r -replace '\s+',' ').Trim())
    }
  }
  foreach($item in ($Addrs -split ',')){
    $addr = ($item -split ':')[0]; $size = ($item -split ':')[1]
    if($s -eq ''){
      foreach($x in [regex]::Matches((FastDevices), '(?m)^([0-9a-fA-F]{8})\s+fastboot')){ if($Serial -eq '' -or $x.Groups[1].Value -eq $Serial){ $s = $x.Groups[1].Value } }
    }
    if($s -eq ''){ Line "addr=$addr no tablet in ABL"; break }
    if(!(WaitFast $s $WaitBoot)){
      Line "addr=$addr not in fastboot after $WaitBoot s; ABL devices: $((FastDevices) -replace '\s+',' ')"
      Line "adb: $((AdbDevices) -replace '\s+',' ')"
      Line 'retrying the reboot into the loader'
      (& $a -s $s reboot bootloader 2>&1 | Out-String) -replace '\s+',' ' | ForEach-Object { Line "reboot2: $_" }
      if(!(WaitFast $s $WaitBoot)){ Line "addr=$addr device not in fastboot after a second reboot"; break }
    }
    (& $f -s $s boot $reader 2>&1 | Out-Null)
    if(!(WaitReader $WaitReader)){ Line "addr=$addr reader never enumerated"; break }
    $o = (& $f -s piano-uefi oem dump $addr $size 2>&1 | Out-String)
    $rc = $LASTEXITCODE
    $txt = ($o -replace '\s+',' ').Trim()
    if($txt.Length -gt 160){ $txt = $txt.Substring(0,160) }
    Line ("addr={0} size={1} rc={2} : {3}" -f $addr,$size,$rc,$txt)
    $ok = $false
    if($rc -eq 0){
      $staged = Join-Path $dir ("dump-{0}.bin" -f $addr)
      (& $f -s piano-uefi get_staged $staged 2>&1 | Out-Null)
      $fi = Get-Item -LiteralPath $staged -ErrorAction SilentlyContinue
      if($fi -and $fi.Length -gt 0){
        $b = [IO.File]::ReadAllBytes($staged)
        $nz = 0; for($i=0;$i -lt $b.Length;$i++){ if($b[$i] -ne 0){ $nz++ } }
        Line ("addr={0} staged {1} bytes nonzero={2} sha256={3}" -f $addr,$fi.Length,$nz,(Get-FileHash -LiteralPath $staged).Hash.ToLower())
        $ok = $true
      } else { Line "addr=$addr staged empty" }
    }
    $results += "{0} rc={1} ok={2}" -f $addr,$rc,$ok
    # Any dump spends the reader session, and the reader is a firmware boot: the host-side way back
    # to ABL for the next address is the reader's own reset action.
    # Walk back through Android between addresses: a faulted dump leaves the reader session dead and a
    # hung USB link, and only the Android boot is provably reachable from there (ensure-hyperos.ps1).
    $s = ''
    & $recover -Serial $Serial -WaitBoot 300 -Log $log | Out-Null
    Start-Sleep -Seconds 5
  }
  Line ("summary: " + ($results -join ' | '))
} finally {
  Line 'returning the tablet to HyperOS'
  & $recover -Serial $Serial -WaitBoot 300 -Log $log
  Line "ensure-hyperos exit=$LASTEXITCODE"
}