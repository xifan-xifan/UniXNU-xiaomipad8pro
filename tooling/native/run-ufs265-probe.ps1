# Unattended acceptNewMedia probe: boot the probe RAM image, then read the stage
# slots once through the reader. fastboot boot only; nothing is written to disk.
# Serial is discovered from fastboot/adb instead of being hard-coded.
param([string]$Serial='', [int]$WaitBoot=240, [int]$WaitReader=150)
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'; $a = Join-Path $pt 'adb.exe'
$img = Join-Path $root 'out\k27-acceptprobe\boot-piano-ufs265-probe.img'
$reader = Join-Path $root 'out\no-demura-control\reader.img'
$log = Join-Path $root 'out\k27-acceptprobe\probe-live.log'
Set-Location (Join-Path $root 'out\k27-acceptprobe')

function ShaH($p){ if(!(Test-Path $p)){ return '' }; (Get-FileHash -LiteralPath $p).Hash.ToLower() }
if((ShaH $img) -ne 'e820a349d3cfc1a98797faa18e2c4fecd87035262de4a9b1b57505534b15dde3'){ throw 'probe image identity' }
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
  $t0 = Get-Date
  & $f -s $s boot $img 2>&1 | ForEach-Object { "boot: $_" }
  if($LASTEXITCODE -ne 0){ 'fastboot boot refused the probe image'; exit 3 }
  if(!(WaitFast $s $WaitBoot)){ 'probe image never returned to fastboot'; exit 4 }
  "probe returned after {0:N1}s" -f ((Get-Date)-$t0).TotalSeconds
  $decoder = Join-Path $root 'tooling\native\decode-slots.py'
  # One dump per reader boot is all this channel allows, so every window costs a
  # fresh reader. 1 MiB at 0x8c5000000 tolerates a slide; 0x2000 at
  # 0x8c5912880 is the static _piano_stage_slots page that already read back once
  # and is the smaller, safer retry.
  $windows = @(
    @{ Name='region'; Addr='8c5000000'; Size='1000000' },
    @{ Name='slots';  Addr='8c5912880'; Size='2000' }
  )
  $done = $false
  foreach($w in $windows){
    & $f -s $s reboot bootloader 2>&1 | Out-Null
    Start-Sleep -Seconds 7
    if(!(WaitFast $s 90)){ 'no fastboot after reboot'; exit 5 }
    & $f -s $s boot $reader 2>&1 | Out-Null
    if(!(WaitReader $WaitReader)){ 'reader did not appear'; exit 6 }
    Start-Sleep -Seconds 2
    $staged = Join-Path $root ("out\k27-acceptprobe\ufs265-{0}.bin" -f $w.Name)
    $ok = $false
    for($i=1; $i -le 3; $i++){
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
    if(!$ok){ "no bytes read back from {0}" -f $w.Name; continue }
    python $decoder $staged --scan
    if($LASTEXITCODE -eq 0){ "readback_ok=$($w.Name)"; $done = $true; break }
    "{0} window holds no probe entry" -f $w.Name
  }
  if(!$done){ 'probe code not found in either window'; exit 8 }
} finally { Stop-Transcript | Out-Null }
