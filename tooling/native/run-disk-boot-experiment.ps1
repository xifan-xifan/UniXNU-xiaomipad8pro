# The first timed disk boot of the installed set, unattended, and the walk back to HyperOS.
#
# Why: the root (SHNGROOT 17,547,919,360 B) and the installed ESP (default macos, native-ufs-root=1,
# piano-boot-guard=180, piano-usb=2) have both been on the medium since 2026-11-24, and the macOS boot set was
# flashed once on 2026-11-25 without any timing record. This run installs that set again (boot_a v4-avb
# 0d52dd5e..., vendor_boot_a/init_boot_a from Ubuntu v1.1.0 7cbb06b9.../96057eb2...), boots from disk and
# timestamps what the tablet does, which is the diagnostic the guard was designed to make readable:
#   ~30-45 s back in ABL   the pre-guard hang (watchdog bite; same window as the 37 s RAM stops)
#   ~180-210 s back in ABL the guard's "no launchd by 180 s" verdict (a deep stop: root/Data/userland)
#   CDC-ACM 1209:0001 up   PianoDwc3 ran (twelfth native kext: far into the boot)
#   nothing for 15 min     the unknown state (docs/results-2026-11-26-ram-boot-v5-device-drop.md), recorded
# Then it always restores the HyperOS triplet from out/backup-20261125 (7c444a2d.../7f766f1d.../56197797...,
# hash-verified) and proves HyperOS came up. Persistent writes: only these three slot-A partitions.
param([string]$Serial='7ccdd855', [int]$Observe=900, [int]$WaitBoot=300)
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'; $a = Join-Path $pt 'adb.exe'
$recover = Join-Path $PSScriptRoot 'ensure-hyperos.ps1'
$mac = @(
  @{ Part='init_boot_a';  Img=(Join-Path $root 'out\inputs\v110\release\boot\init_boot.img');   Sha='96057eb263fbe09284ba8bbe888128dbc0b36525dec4b01da918dcb7c6fe10ef' },
  @{ Part='vendor_boot_a';Img=(Join-Path $root 'out\inputs\v110\release\boot\vendor_boot.img');  Sha='7cbb06b9eddb8051c201d7a1f672620a5440283f0d5edbf0ace9270c05a60174' },
  @{ Part='boot_a';       Img=(Join-Path $root 'out\persistent\boot-piano-macos-persistent-v4-avb.img'); Sha='0d52dd5e2c6304918b2b6836ff42e63600b1e5811aa96100f390b74fe4a76728' })
$android = @(
  @{ Part='init_boot_a';  Img=(Join-Path $root 'out\backup-20261125\init_boot_a.img');  Sha='56197797f2ec99870a4e42b7a24bbf88407e4409615aba61f401182f13ea5cdc' },
  @{ Part='vendor_boot_a';Img=(Join-Path $root 'out\backup-20261125\vendor_boot_a.img');Sha='7f766f1d728c39d779a3fb7cf0ac1310dac4306cd3b37a98a0143dd4fca5bd23' },
  @{ Part='boot_a';       Img=(Join-Path $root 'out\backup-20261125\boot_a.img');       Sha='7c444a2d6aa930cd79e2d48c569f6c891fbaff054c63fc85c3a57fb61c660ac2' })
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dir = Join-Path $root ("logs\native-runs\disk-boot-$stamp")
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'run.log'
function Line($m){ $line = "$((Get-Date).ToString('HH:mm:ss.fff')) $m"; Add-Content -LiteralPath $log -Value $line; $line }
function ShaH($p){ if(!(Test-Path $p)){ return '' }; (Get-FileHash -LiteralPath $p).Hash.ToLower() }
function FastDevices { ((& $f devices 2>&1 | Out-String)) }
function AdbDevices { ((& $a devices 2>&1 | Out-String)) }
function SerialPorts { (python -c "import serial.tools.list_ports as p; print(';'.join(repr(x.vid).upper()+'/'+repr(x.pid).upper()+':'+x.device for x in p.comports()) if 1 else '')" 2>&1 | Out-String).Trim() }
function InAbl { ((FastDevices) -match "(?m)^$Serial\s+fastboot") }
function WaitAbl([int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if(InAbl){ return $true }; Start-Sleep -Milliseconds 800 }while((Get-Date) -lt $until); return $false }
function ToAbl([string]$why){
  if(InAbl){ return $true }
  Line "walking to ABL ($why)"
  (& $a -s $Serial reboot bootloader 2>&1 | Out-Null)
  if(WaitAbl 150){ return $true }
  (& $a -s $Serial reboot bootloader 2>&1 | Out-Null)   # the loader reboot is known to miss half the time (2026-11-26)
  return (WaitAbl 150)
}
function FlashSet($set,[string]$tag){
  foreach($s in $set){
    if((ShaH $s.Img) -ne $s.Sha){ Line "$tag REFUSED: $($s.Part) source does not hash to its record"; return $false }
    Line "$tag flash $($s.Part) <- $(Split-Path $s.Img -Leaf) $($s.Sha.Substring(0,12))"
    $o = (& $f -s $Serial flash $s.Part $s.Img 2>&1 | Out-String)
    Line (($o -replace '(?m)\s+$',' ') -replace '\s+',' ')
    if($o -notmatch 'OKAY|okay'){ Line "$tag $($s.Part) flash did not report OKAY"; return $false }
  }
  return $true
}
try {
  foreach($s in ($mac + $android)){ if((ShaH $s.Img) -ne $s.Sha){ Line "identity check failed for $($s.Part): $($s.Img)"; exit 1 } }
  Line 'all six partition sources hash to their records'
  if(!(ToAbl 'pre-experiment')){ Line 'no tablet in ABL to write the macOS set'; exit 2 }
  $g = (& $f -s $Serial getvar all 2>&1 | Out-String)
  Set-Content -LiteralPath (Join-Path $dir 'abl-before.txt') -Value $g
  Line ("pre-write slot a: successful=" + (($g -match 'successful:\s*a:\s*yes')) + " unbootable=" + (($g -match 'unbootable:\s*a:\s*yes')) + " retry=" + (($g -replace '(?m)^.*slot-retry-count:a:\s*','') -replace '(?m)\s.*',''))
  if(!(FlashSet $mac 'macOS-set')){ Line 'macOS set not fully written: nothing was booted'; exit 3 }
  (& $f -s $Serial --set-active=a 2>&1 | Out-String) -replace '\s+',' ' | ForEach-Object { Line "set-active: $_" }
  $t0 = Get-Date
  (& $f -s $Serial reboot 2>&1 | Out-Null)
  Line 'reboot issued at t0; observing (no fastboot polling while the macOS boot runs is not a problem: ABL shows up as fastboot when it returns)'
  $state = ''; $end = $t0.AddSeconds($Observe)
  while((Get-Date) -lt $end){
    $d = FastDevices
    $ad = AdbDevices
    $sp = SerialPorts
    $s1 = ''
    if($d -match "(?m)^$Serial\s+fastboot"){ $s1 = 'abl' }
    elseif($ad -match "(?m)^$Serial\s+device"){ $s1 = 'adb-android' }
    elseif($sp -match '0X1209/0X0001'){ $s1 = 'cdc-acm-1209-0001' }
    elseif($sp -match '0X18D1'){ $s1 = "android-usb-other $($sp -replace '\s+',' ')" }
    if($s1 -ne $state){
      $t = ((Get-Date) - $t0).TotalSeconds
      Line ("t+{0:N0}s state={1}" -f $t,$s1)
      if($s1 -ne ''){ Set-Content -LiteralPath (Join-Path $dir ("state-{0:N0}.txt" -f $t)) -Value "fastboot: $d`nadb: $ad`nserial: $sp" }
      $state = $s1
      if($s1 -eq 'abl'){ Line "ABL returned at t+{0:N0}s (see the header for what that window means)" -f $t; break }
      if($s1 -eq 'cdc-acm-1209-0001'){ Line 'the PianoDwc3 console enumerated: macOS is running this far (kept observing for the guard verdict)' }
    }
    Start-Sleep -Seconds 5
  }
  if($state -eq ''){ Line "nothing enumerated for $Observe s: the unknown state, recorded (docs/results-2026-11-26-ram-boot-v5-device-drop.md)" }
} finally {
  Line 'restoring the HyperOS triplet'
  if(!(ToAbl 'restore')){
    Line 'the tablet is not on the bus from the host: HyperOS restore needs it in ABL; leaving it, recorded'
  } else {
    if(FlashSet $android 'HyperOS-restore'){
      (& $f -s $Serial --set-active=a 2>&1 | Out-String) -replace '\s+',' ' | ForEach-Object { Line "set-active: $_" }
      (& $f -s $Serial reboot 2>&1 | Out-Null)
      $until=(Get-Date).AddSeconds($WaitBoot)
      $done=''
      while((Get-Date) -lt $until){
        Start-Sleep -Seconds 5
        if((AdbDevices) -match "(?m)^$Serial\s+device"){ $done=((& $a -s $Serial shell getprop sys.boot_completed 2>&1 | Out-String).Trim()); if($done -eq '1'){ break } }
      }
      $build=((& $a -s $Serial shell getprop ro.mi.os.version.incremental 2>&1 | Out-String).Trim())
      Line ("hyperos=" + $(if($done -eq '1'){'up'}else{'NOT-UP'}) + " build=$build")
    } else { Line 'RESTORE INCOMPLETE: the tablet may sit on the macOS set' }
  }
  Line "log=$dir"
}