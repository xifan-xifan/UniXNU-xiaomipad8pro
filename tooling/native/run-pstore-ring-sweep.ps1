# Walk the pstore console ring while the macOS boot set stays installed: one dump per reader boot,
# the reader then self-boots the installed system, which stops again and returns to ABL by itself.
#
# docs/results-2026-11-26-timed-disk-boot-72s.md proved the loop: disk boot -> stop -> ABL at ~70 s.
# This turn showed pstore-console=0x9cb00000 holds real XNU text after a reset (8 KiB at the ring head
# read fine and cold). The ring is 0x700000 bytes and the head is early boot; this script probes other
# offsets, one 8 KiB page per cycle, with no Android in between, so the ring is never overwritten by
# anything but the next identical macOS boot. A successful dump hangs the next host fastboot call (2026-11-26):
# after staging we kill fastboot.exe and let the reader time out into the installed system on its own.
# Always restores the HyperOS triplet at the end.
param([string]$Serial='7ccdd855', [string]$Base='9cb00000', [string]$Offsets='2000,10000,80000,200000,600000', [string]$Size='2000', [int]$WaitAbl=300, [int]$WaitReader=150)
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'; $a = Join-Path $pt 'adb.exe'
$reader = Join-Path $root 'out\no-demura-control\reader.img'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dir = Join-Path $root ("logs\native-runs\pstore-sweep-$stamp")
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'run.log'
function Line($m){ $line = "$((Get-Date).ToString('HH:mm:ss.fff')) $m"; Add-Content -LiteralPath $log -Value $line; $line }
function ShaH($p){ if(!(Test-Path $p)){ return '' }; (Get-FileHash -LiteralPath $p).Hash.ToLower() }
function FastDevices { ((& $f devices 2>&1 | Out-String)) }
function AdbDevices { ((& $a devices 2>&1 | Out-String)) }
function InAbl { ((FastDevices) -match "(?m)^$Serial\s+fastboot") }
function WaitAbl([int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if(InAbl){ return $true }; Start-Sleep -Milliseconds 800 }while((Get-Date) -lt $until); return $false }
function WaitReader([int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match '(?m)^piano-uefi\s+fastboot'){ return $true }; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
$android = @(
  @{ Part='init_boot_a';  Img=(Join-Path $root 'out\backup-20261125\init_boot_a.img');  Sha='56197797f2ec99870a4e42b7a24bbf88407e4409615aba61f401182f13ea5cdc' },
  @{ Part='vendor_boot_a';Img=(Join-Path $root 'out\backup-20261125\vendor_boot_a.img');Sha='7f766f1d728c39d779a3fb7cf0ac1310dac4306cd3b37a98a0143dd4fca5bd23' },
  @{ Part='boot_a';       Img=(Join-Path $root 'out\backup-20261125\boot_a.img');       Sha='7c444a2d6aa930cd79e2d48c569f6c891fbaff054c63fc85c3a57fb61c660ac2' })
try {
  if((ShaH $reader) -ne '708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468'){ Line 'reader identity mismatch'; exit 1 }
  foreach($off in ($Offsets -split ',')){
    if(!(WaitAbl $WaitAbl)){ Line "offset $off : no ABL within $WaitAbl s (a macOS boot has to stop and come back)"; break }
    (& $f -s $Serial boot $reader 2>&1 | Out-Null)
    if(!(WaitReader $WaitReader)){ Line "offset $off : reader never enumerated"; break }
    $addr = ('{0:x}' -f ([Convert]::ToInt64($Base,16) + [Convert]::ToInt64($off,16)))
    $o = (& $f -s piano-uefi oem dump $addr $Size 2>&1 | Out-String)
    $rc = $LASTEXITCODE
    Line ("offset $off dump rc=$rc : " + (($o -replace '\s+',' ').Trim() -replace '^(.{0,110}).*','$1'))
    if($rc -eq 0){
      $staged = Join-Path $dir ("ring-{0}.bin" -f $off)
      (& $f -s piano-uefi get_staged $staged 2>&1 | Out-Null)
      $fi = Get-Item -LiteralPath $staged -ErrorAction SilentlyContinue
      if($fi -and $fi.Length -gt 0){ $b=[IO.File]::ReadAllBytes($staged); $nz=0; foreach($x in $b){ if($x -ne 0){ $nz++ } }; Line ("ring offset {0}: {1} bytes nonzero={2}" -f $off,$fi.Length,$nz) } else { Line "ring offset ${off}: staged empty" }
    }
    # The dump spent this session and the host link hangs on the next call: kill fastboot, let the
    # reader boot the installed system by itself, and the next macOS stop brings ABL back.
    Stop-Process -Name fastboot -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 8
  }
} finally {
  Line 'restoring the HyperOS triplet'
  Stop-Process -Name fastboot -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 5
  $ok = WaitAbl 300
  if(!$ok){
    # Nothing but the reader (or macOS stopping) can be in front; wait once more through a boot.
    $until=(Get-Date).AddSeconds(600); while((Get-Date) -lt $until){ if((FastDevices) -match "(?m)^$Serial\s+fastboot"){ $ok=$true; break }; if((AdbDevices) -match "(?m)^$Serial\s+device"){ (& $a -s $Serial reboot bootloader 2>&1 | Out-Null) }; Start-Sleep 5 }
  }
  if(!$ok){ Line 'restore skipped: the tablet is not reachable from the host'; exit 9 }
  foreach($p in $android){ (& $f -s $Serial flash $p.Part $p.Img 2>&1 | Out-Null); Start-Sleep 1 }
  (& $f -s $Serial --set-active=a 2>&1 | Out-Null)
  (& $f -s $Serial reboot 2>&1 | Out-Null)
  $until=(Get-Date).AddSeconds(300); $done=''
  while((Get-Date) -lt $until){ Start-Sleep 5; if((AdbDevices) -match "(?m)^$Serial\s+device"){ $done=((& $a -s $Serial shell getprop sys.boot_completed 2>&1 | Out-String).Trim()); if($done -eq '1'){ break } } }
  Line ("hyperos=" + $(if($done -eq '1'){'up'}else{'NOT-UP'}))
  Line "log=$dir"
}