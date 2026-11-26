# Disk boot, then read the pstore console out of DRAM from the same ABL session.
#
# The installed ESP's macos entry says pstore-console=0x9cb00000 pstore-console-size=0x700000: XNU mirrors its console
# there, and a reset (not a power cycle) leaves DRAM. docs/results-2026-11-25-dump-channel.md once failed a read of
# 0x9cb00000, but in a warm session with the old, since-overturned rules, and a cold session now reads 8 KiB fine.
# One run answers whether the XNU console text of the 72 s stop can be read at all. Always restores HyperOS.
param([string]$Serial='7ccdd855', [string]$Addrs='9cb00000:2000,9cb00000:100000', [int]$WaitAbl=240, [int]$WaitReader=150)
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'; $a = Join-Path $pt 'adb.exe'
$reader = Join-Path $root 'out\no-demura-control\reader.img'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dir = Join-Path $root ("logs\native-runs\pstore-$stamp")
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'run.log'
function Line($m){ $line = "$((Get-Date).ToString('HH:mm:ss.fff')) $m"; Add-Content -LiteralPath $log -Value $line; $line }
function ShaH($p){ if(!(Test-Path $p)){ return '' }; (Get-FileHash -LiteralPath $p).Hash.ToLower() }
function FastDevices { ((& $f devices 2>&1 | Out-String)) }
function AdbDevices { ((& $a devices 2>&1 | Out-String)) }
function InAbl { ((FastDevices) -match "(?m)^$Serial\s+fastboot") }
function WaitAbl([int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if(InAbl){ return $true }; Start-Sleep -Milliseconds 800 }while((Get-Date) -lt $until); return $false }
function WaitReader([int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match '(?m)^piano-uefi\s+fastboot'){ return $true }; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
$mac = @(
  @{ Part='init_boot_a';  Img=(Join-Path $root 'out\inputs\v110\release\boot\init_boot.img');   Sha='96057eb263fbe09284ba8bbe888128dbc0b36525dec4b01da918dcb7c6fe10ef' },
  @{ Part='vendor_boot_a';Img=(Join-Path $root 'out\inputs\v110\release\boot\vendor_boot.img');  Sha='7cbb06b9eddb8051c201d7a1f672620a5440283f0d5edbf0ace9270c05a60174' },
  @{ Part='boot_a';       Img=(Join-Path $root 'out\persistent\boot-piano-macos-persistent-v4-avb.img'); Sha='0d52dd5e2c6304918b2b6836ff42e63600b1e5811aa96100f390b74fe4a76728' })
$android = @(
  @{ Part='init_boot_a';  Img=(Join-Path $root 'out\backup-20261125\init_boot_a.img');  Sha='56197797f2ec99870a4e42b7a24bbf88407e4409615aba61f401182f13ea5cdc' },
  @{ Part='vendor_boot_a';Img=(Join-Path $root 'out\backup-20261125\vendor_boot_a.img');Sha='7f766f1d728c39d779a3fb7cf0ac1310dac4306cd3b37a98a0143dd4fca5bd23' },
  @{ Part='boot_a';       Img=(Join-Path $root 'out\backup-20261125\boot_a.img');       Sha='7c444a2d6aa930cd79e2d48c569f6c891fbaff054c63fc85c3a57fb61c660ac2' })
try {
  foreach($s in ($mac + $android)){ if((ShaH $s.Img) -ne $s.Sha){ Line "identity check failed for $($s.Part)"; exit 1 } }
  if((ShaH $reader) -ne '708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468'){ Line 'reader identity mismatch'; exit 1 }
  $s = ''
  if(!(InAbl)){
    if((AdbDevices) -match "(?m)^$Serial\s+device"){ (& $a -s $Serial reboot bootloader 2>&1 | Out-Null) }
    if(!(WaitAbl 150)){ (& $a -s $Serial reboot bootloader 2>&1 | Out-Null); if(!(WaitAbl 150)){ Line 'no tablet in ABL'; exit 2 } }
  }
  foreach($p in $mac){
    Line ("flash " + $p.Part)
    (& $f -s $Serial flash $p.Part $p.Img 2>&1 | Out-String) -replace '\s+',' ' | ForEach-Object { Line $_ }
  }
  (& $f -s $Serial --set-active=a 2>&1 | Out-Null)
  $t0 = Get-Date
  (& $f -s $Serial reboot 2>&1 | Out-Null)
  Line 'disk boot issued; waiting for the guard/backstop to bring it back to ABL'
  $until=(Get-Date).AddSeconds($WaitAbl)
  $back=$false
  while((Get-Date) -lt $until){ if((FastDevices) -match "(?m)^$Serial\s+fastboot"){ $back=$true; break }; Start-Sleep -Milliseconds 800 }
  Line ("ABL back after {0:N0} s: {1}" -f ((Get-Date)-$t0).TotalSeconds, $back)
  if(!$back){ Line 'the tablet never returned to ABL in this window'; exit 4 }
  foreach($item in ($Addrs -split ',')){
    $addr = ($item -split ':')[0]; $size = ($item -split ':')[1]
    (& $f -s $Serial boot $reader 2>&1 | Out-Null)
    if(!(WaitReader $WaitReader)){ Line 'reader never enumerated'; break }
    $o = (& $f -s piano-uefi oem dump $addr $size 2>&1 | Out-String)
    $rc = $LASTEXITCODE
    Line ("dump $addr/$size rc=$rc : " + (($o -replace '\s+',' ').Trim() -replace '^(.{0,120}).*','$1'))
    if($rc -eq 0){
      $staged = Join-Path $dir ("pstore-{0}-{1}.bin" -f $addr,$size)
      (& $f -s piano-uefi get_staged $staged 2>&1 | Out-Null)
      $fi = Get-Item -LiteralPath $staged -ErrorAction SilentlyContinue
      if($fi -and $fi.Length -gt 0){
        $b=[IO.File]::ReadAllBytes($staged); $nz=0; foreach($x in $b){ if($x -ne 0){ $nz++ } }
        Line ("staged {0} bytes nonzero={1} sha256={2}" -f $fi.Length,$nz,(Get-FileHash -LiteralPath $staged).Hash.ToLower())
      } else { Line 'staged empty (a large accepted dump may not stage; the next address is a fresh session)' }
    }
    # Any dump spends the session and the link: go back to ABL the patient way, through Android.
    Start-Sleep -Seconds 10
    $until=(Get-Date).AddSeconds(240)
    while((Get-Date) -lt $until){ if((AdbDevices) -match "(?m)^$Serial\s+device"){ break }; Start-Sleep -Seconds 3 }
    (& $a -s $Serial reboot bootloader 2>&1 | Out-Null)
    if(!(WaitAbl 150)){ (& $a -s $Serial reboot bootloader 2>&1 | Out-Null); if(!(WaitAbl 150)){ Line 'cannot get back to ABL for the next address'; break } }
  }
} finally {
  Line 'restoring the HyperOS triplet'
  $ok=$false
  $until=(Get-Date).AddSeconds(120)
  while((Get-Date) -lt $until){ if((FastDevices) -match "(?m)^$Serial\s+fastboot"){ $ok=$true; break }; if((AdbDevices) -match "(?m)^$Serial\s+device"){ (& $a -s $Serial reboot bootloader 2>&1 | Out-Null); Start-Sleep 8 } }
  if(!$ok -and !(WaitAbl 150)){ Line 'restore skipped: the tablet is not in ABL'; exit 9 }
  foreach($p in $android){ (& $f -s $Serial flash $p.Part $p.Img 2>&1 | Out-Null) }
  (& $f -s $Serial --set-active=a 2>&1 | Out-Null)
  (& $f -s $Serial reboot 2>&1 | Out-Null)
  $until=(Get-Date).AddSeconds(300); $done=''
  while((Get-Date) -lt $until){ Start-Sleep 5; if((AdbDevices) -match "(?m)^$Serial\s+device"){ $done=((& $a -s $Serial shell getprop sys.boot_completed 2>&1 | Out-String).Trim()); if($done -eq '1'){ break } } }
  Line ("hyperos=" + $(if($done -eq '1'){'up'}else{'NOT-UP'}))
  Line "log=$dir"
}