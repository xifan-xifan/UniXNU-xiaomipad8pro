# Unattended startMatching probe: boot the probe RAM image, read the stage slots
# once through the reader, and always hand the tablet back to HyperOS.
# fastboot boot only; nothing is written to disk.
param([string]$Serial='7ccdd855', [int]$WaitBoot=270, [int]$WaitReader=150)
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'; $a = Join-Path $pt 'adb.exe'
$img = Join-Path $root 'out\k27-smprobe\boot-piano-ufs266-smprobe.img'
$reader = Join-Path $root 'out\no-demura-control\reader.img'
$log = Join-Path $root 'out\k27-smprobe\probe-live.log'
$recover = Join-Path $PSScriptRoot 'ensure-hyperos.ps1'
Set-Location (Join-Path $root 'out\k27-smprobe')

function ShaH($p){ if(!(Test-Path $p)){ return '' }; (Get-FileHash -LiteralPath $p).Hash.ToLower() }
if((ShaH $img) -ne 'a56f132dc2f3f5e89bbd09f90252c6e630d6f977427a2f9e16267830119e43fe'){ throw 'probe image identity' }
if((ShaH $reader) -ne '708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468'){ throw 'reader identity' }

Start-Transcript -Path $log -Force | Out-Null
function FastDevices { ((& $f devices 2>&1) | Out-String) }
function AdbDevices { ((& $a devices 2>&1) | Out-String) }
function PickFastboot {
  $d = FastDevices
  $m = [regex]::Matches($d, '(?m)^([0-9a-fA-F]{8})\s+fastboot')
  foreach($x in $m){ if($Serial -eq '' -or $x.Groups[1].Value -eq $Serial){ return $x.Groups[1].Value } }
  return ''
}
function PickAdb {
  $d = AdbDevices
  $m = [regex]::Matches($d, '(?m)^([0-9a-fA-F]{8})\s+device')
  foreach($x in $m){ if($Serial -eq '' -or $x.Groups[1].Value -eq $Serial){ return $x.Groups[1].Value } }
  return ''
}
function WaitFast([string]$s,[int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match "(?m)^$s\s+fastboot"){ return $true }; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
function WaitReader([int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match '(?m)^piano-uefi\s+fastboot'){ return $true }; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
function Getvar([string]$s,[string]$key){ ((& $f -s $s getvar $key 2>&1 | Out-String)) }

try {
  $s = PickFastboot
  if($s -eq ''){
    $ad = PickAdb
    if($ad -eq ''){ 'no adb and no fastboot device'; exit 2 }
    $s = $ad
    & $a -s $s reboot bootloader 2>&1 | Out-Null
    if(!(WaitFast $s 90)){ 'device did not reach fastboot'; exit 2 }
  }
  "serial=$s"
  # ABL refuses every fastboot boot once the slot runs out of retries, and the
  # only cost of that is an owner in front of the tablet, so the retry count is
  # bought back here rather than discovered by the experiment failing.
  $slot = (((Getvar $s 'current-slot') -replace '(?m)^.*current-slot:\s*','') -replace '\s','')
  if($slot -notmatch '^[ab]$'){ $slot = 'a' }
  $retry = 0
  if(((Getvar $s ('slot-retry-count:' + $slot)) -match ('slot-retry-count:' + $slot + ':\s*(\d+)'))){ $retry = [int]$Matches[1] }
  $bad = ((Getvar $s ('slot-unbootable:' + $slot)) -match 'yes') -or ($retry -eq 0)
  if($bad){
    (& $f -s $s --set-active=$slot 2>&1 | Out-String).Trim()
    "retry count after set-active: $((Getvar $s ('slot-retry-count:' + $slot)).Trim())"
  }
  "ABL slot=$slot unbootable=$(((Getvar $s ('slot-unbootable:' + $slot)) -match 'yes')) retry=$retry"
  if((Getvar $s ('slot-unbootable:' + $slot)) -match 'yes'){ 'ABL still marks the slot unbootable'; exit 7 }
  $t0 = Get-Date
  & $f -s $s boot $img 2>&1 | ForEach-Object { "boot: $_" }
  if($LASTEXITCODE -ne 0){ 'fastboot boot refused the probe image'; exit 3 }
  if(!(WaitFast $s $WaitBoot)){ 'probe image never returned to fastboot'; exit 4 }
  "probe returned after {0:N1}s" -f ((Get-Date)-$t0).TotalSeconds
  $decoder = Join-Path $root 'tooling\native\decode-slots.py'
  # One dump per reader boot, and it has to be the session's first command; a
  # faulted dump kills the session, so retrying on the same reader is pointless
  # and costs a whole reader boot. 0x2000 at 0x8c5912880 is the static
  # _piano_stage_slots page (VA 0xfffffff00c912880 - 0xfffffff007004000 +
  # 0x8c0004000) and has read back before; the 16 MiB region window is a
  # fallback for a slid kernel and is known to fault more often than not.
  $windows = @(
    @{ Name='slots';  Addr='8c5912880'; Size='2000' },
    @{ Name='region'; Addr='8c5000000'; Size='1000000' }
  )
  $done = $false
  foreach($w in $windows){
    & $f -s $s reboot bootloader 2>&1 | Out-Null
    Start-Sleep -Seconds 7
    if(!(WaitFast $s 90)){ 'no fastboot after reboot'; break }
    & $f -s $s boot $reader 2>&1 | Out-Null
    if(!(WaitReader $WaitReader)){ 'reader did not appear'; break }
    Start-Sleep -Seconds 2
    $staged = Join-Path $root ("out\k27-smprobe\ufs266-{0}.bin" -f $w.Name)
    $ok = $false
    for($i=1; $i -le 1; $i++){
      $o = (& $f -s piano-uefi oem dump $w.Addr $w.Size 2>&1 | Out-String)
      "{0} dump#{1} rc={2} : {3}" -f $w.Name,$i,$LASTEXITCODE,$o.Trim()
      if($LASTEXITCODE -eq 0){
        & $f -s piano-uefi get_staged $staged 2>&1 | Out-Null
        $fi = Get-Item -LiteralPath $staged -ErrorAction SilentlyContinue
        if($fi -and $fi.Length -gt 0){ "{0} staged {1} bytes" -f $w.Name,$fi.Length; $ok = $true; break }
      }
      if(!(WaitReader 5)){ 'reader gone'; break }
      Start-Sleep -Milliseconds 800
    }
    # A faulted dump takes the USB link with it: the next fastboot call after it
    # hangs forever, as it did twice on 2026-11-26. One window per run, and the
    # finally block walks the tablet back to HyperOS.
    if(!$ok){ "no bytes read back from {0}" -f $w.Name; break }
    python $decoder $staged --scan --codes b0 b7
    if($LASTEXITCODE -eq 0){ "readback_ok=$($w.Name)"; $done = $true; break }
    "{0} window holds no probe entry" -f $w.Name
  }
  if(!$done){ 'probe code not found in either window'; exit 8 }
} finally {
  Stop-Transcript | Out-Null
  'returning the tablet to HyperOS'
  & $recover -Serial $Serial -WaitBoot 300 -Log $log
  "ensure-hyperos exit=$LASTEXITCODE"
}
