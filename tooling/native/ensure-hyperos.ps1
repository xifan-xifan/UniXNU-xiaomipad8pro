# Returns the tablet to HyperOS and proves it got there, so no unattended run ends
# with the tablet sitting in ABL fastboot.  An exhausted ABL retry count marks the
# slot unbootable and ABL then cold-boots straight to fastboot, so that state is
# cleared here rather than reported; --set-active costs no data.
param([string]$Serial='', [int]$WaitBoot=300, [string]$Log='')
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'
$a = Join-Path $pt 'adb.exe'

function Line($m){ if($Log -ne ''){ Add-Content -LiteralPath $Log -Message $m } else { $m } }
function FastDevices { ((& $f devices 2>&1) | Out-String) }
function AdbDevices { ((& $a devices 2>&1) | Out-String) }
function PickFastboot {
  $m = [regex]::Matches((FastDevices), '(?m)^([0-9a-fA-F]{8})\s+fastboot')
  foreach($x in $m){ if($Serial -eq '' -or $x.Groups[1].Value -eq $Serial){ return $x.Groups[1].Value } }
  return ''
}
function PickAdb {
  $m = [regex]::Matches((AdbDevices), '(?m)^([0-9a-fA-F]{8})\s+device')
  foreach($x in $m){ if($Serial -eq '' -or $x.Groups[1].Value -eq $Serial){ return $x.Groups[1].Value } }
  return ''
}
function WaitAdb([string]$s,[int]$seconds){
  $until=(Get-Date).AddSeconds($seconds)
  do { if((AdbDevices) -match "(?m)^$s\s+device"){ return $true }; Start-Sleep -Milliseconds 800 } while((Get-Date) -lt $until)
  return $false
}
function Getvar([string]$s,[string]$key){ ((& $f -s $s getvar $key 2>&1 | Out-String)) }

try {
  $s = PickAdb
  if($s -ne '' -and ((& $a -s $s shell getprop sys.boot_completed 2>&1 | Out-String).Trim() -eq '1')){
    $slot=((& $a -s $s shell getprop ro.boot.slot_suffix 2>&1 | Out-String).Trim())
    Line "hyperos=up serial=$s slot=$slot"
    exit 0
  }
  $s = PickFastboot
  if($s -eq ''){ $s = $Serial }
  if($s -eq ''){ Line 'no tablet on adb or fastboot: it needs a power cycle'; exit 2 }
  $slot = ((Getvar $s 'current-slot') -replace '(?m)^.*current-slot:\s*','').Trim()
  if($slot -notmatch '^[ab]$'){ $slot = 'a' }
  $unbootable = ((Getvar $s 'slot-unbootable:' + $slot) -match 'unbootable.*:\s*yes')
  $retry = 0
  if((Getvar $s ('slot-retry-count:' + $slot)) -match ('slot-retry-count:' + $slot + ':\s*(\d+)')){ $retry = [int]$Matches[1] }
  Line "slot=$slot unbootable=$unbootable retry=$retry"
  if($unbootable -or $retry -eq 0){
    # A slot ABL gave up on refuses its own boot image and every fastboot boot,
    # so this has to happen before the reboot can be expected to work.
    $cleared = (& $f -s $s --set-active=$slot 2>&1 | Out-String)
    Line ("set-active=" + $slot + ': ' + $cleared.Trim())
  }
  Line 'reboot'
  & $f -s $s reboot 2>&1 | Out-Null
  if(!(WaitAdb $s $WaitBoot)){ Line "HyperOS never came up on slot $slot after $WaitBoot s"; exit 3 }
  $until=(Get-Date).AddSeconds(180)
  do { if(((& $a -s $s shell getprop sys.boot_completed 2>&1 | Out-String).Trim()) -eq '1'){ break }; Start-Sleep -Seconds 5 } while((Get-Date) -lt $until)
  $done=((& $a -s $s shell getprop sys.boot_completed 2>&1 | Out-String).Trim())
  $build=((& $a -s $s shell getprop ro.mi.os.version.incremental 2>&1 | Out-String).Trim())
  Line "hyperos=$(if($done -eq '1'){'up'}else{'partial'}) build=$build"
  if($done -ne '1'){ exit 3 }
  exit 0
} finally { }
