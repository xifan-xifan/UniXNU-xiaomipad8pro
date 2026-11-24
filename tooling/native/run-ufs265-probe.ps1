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
if((ShaH $img) -ne 'ba9285ec8cc73403a34f6383e2b85c2861d8071435c7c1aea4ce906fdcdc7f84'){ throw 'probe image identity' }
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
  & $f -s $s reboot bootloader 2>&1 | Out-Null
  Start-Sleep -Seconds 7
  if(!(WaitFast $s 90)){ 'no fastboot after reboot'; exit 5 }
  & $f -s $s boot $reader 2>&1 | Out-Null
  if(!(WaitReader $WaitReader)){ 'reader did not appear'; exit 6 }
  Start-Sleep -Seconds 2
  $staged = Join-Path $root 'out\k27-acceptprobe\ufs265-slots.bin'
  $ok = $false
  for($i=1; $i -le 3; $i++){
    $o = (& $f -s piano-uefi oem dump 8c5912880 2000 2>&1 | Out-String)
    "dump#$i rc=$LASTEXITCODE : $($o.Trim())"
    if($LASTEXITCODE -eq 0){
      & $f -s piano-uefi get_staged $staged 2>&1 | Out-Null
      $fi = Get-Item -LiteralPath $staged -ErrorAction SilentlyContinue
      if($fi -and $fi.Length -gt 0){ "staged $($fi.Length) bytes"; $ok = $true; break }
    }
    if(!(WaitReader 5)){ 'reader gone'; break }
    Start-Sleep -Milliseconds 800
  }
  if(!$ok){ 'no slots read back'; exit 7 }
  $raw = [IO.File]::ReadAllBytes($staged)
  $hits = @()
  foreach($code in 0xa0..0xa6){
    $off = ($code - 0x0b) * 32
    if($off + 32 -le $raw.Length){
      $w = @([BitConverter]::ToUInt64($raw,$off),[BitConverter]::ToUInt64($raw,$off+8),[BitConverter]::ToUInt64($raw,$off+16),[BitConverter]::ToUInt64($raw,$off+24))
      if(($w | Where-Object { $_ -ne 0 }).Count -gt 0){ $hits += ('{0:x2}' -f $code) }
    }
  }
  "codes seen: " + ($hits -join ' ')
  "readback_ok=$ok"
} finally { Stop-Transcript | Out-Null }
