# Verbose console of the upstream-lineage 27.3 RAM boot (k273 re-linked kernel + kc-build273 collection), read from the pstore mailbox.
# fastboot boot of out/k273-ram/boot-piano-macos-ram-A-bundled.img: upstream-lineage (k273 kernel re-linked once,
# default entry macos-8cpu, -v,
# native-ufs-root=1 native-ufs-root-part=linux_rootfs, data-persist=0, piano-reset-after=45,
# piano-reset-to=bootloader) -> the image resets itself into ABL -> one reader boot -> one cold dump
# of 9cb00000 700000 (the k273 mailbox; today proved readable from the session right after a stop,
# 7 MiB staged fine) -> kill fastboot and let the reader boot the installed system (HyperOS now).
param([string]$Serial='7ccdd855', [string]$Base='9cb00000', [string]$Size='700000', [int]$WaitBack=760, [int]$WaitReader=150,
      [string]$ImgRel='out\k273-ram-verified2\boot-piano-macos-ram-A-bundled.img', [string]$Tag='verified2', [string]$ImageSha='')
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'; $a = Join-Path $pt 'adb.exe'
$img = Join-Path $root $ImgRel
$reader = Join-Path $root 'out\no-demura-control\reader.img'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dir = Join-Path $root ("logs\native-runs\ram-k273-$Tag-$stamp")
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'run.log'
function Line($m){ $line = "$((Get-Date).ToString('HH:mm:ss.fff')) $m"; [IO.File]::AppendAllText($log, $line + [Environment]::NewLine); Write-Output $m; $line }
$h = [Security.Cryptography.SHA256]::Create()
function ShaH($p){ if(!(Test-Path $p)){ return '' }
  $s = [IO.File]::OpenRead($p); try { $b = $h.ComputeHash($s) } finally { $s.Dispose() }
  (-join ($b | ForEach-Object { $_.ToString('x2') })) }
function FastDevices { ((& $f devices 2>&1 | Out-String)) }
function AdbDevices { ((& $a devices 2>&1 | Out-String)) }
function WaitFast([string]$s,[int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match "(?m)^$s\s+fastboot"){ return $true }; Start-Sleep -Milliseconds 800 }while((Get-Date) -lt $until); return $false }
function WaitReader([int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match '(?m)^piano-uefi\s+fastboot'){ return $true }; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
try {
  $want = $ImageSha.Trim().ToLower()
  $side = "$img.sha256"
  if($want -eq '' -and (Test-Path $side)){ $want = (((Get-Content -LiteralPath $side -Raw) -split '\s+')[0]).ToLower() }
  if($want -eq ''){ Line 'no expected image sha (pass -ImageSha or ship a .sha256 sidecar)'; exit 1 }
  if((ShaH $img) -ne $want){ Line 'probe image identity mismatch'; exit 1 }
  Line ("image " + $want.Substring(0,16) + " verified")
  if((ShaH $reader) -ne '708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468'){ Line 'reader identity mismatch'; exit 1 }
  $s = ''
  if(!(WaitFast $Serial 5)){
    if((AdbDevices) -match "(?m)^$Serial\s+device"){ (& $a -s $Serial reboot bootloader 2>&1 | Out-Null) }
    if(!(WaitFast $Serial 150)){ (& $a -s $Serial reboot bootloader 2>&1 | Out-Null); if(!(WaitFast $Serial 150)){ Line 'no tablet in ABL'; exit 2 } }
  }
  $t0 = Get-Date
  (& $f -s $Serial boot $img 2>&1 | Out-String) -replace '\s+',' ' | ForEach-Object { Line ("boot: " + (($_ -replace '^(.{0,120}).*') )) }
  Line 'probe image booted; waiting for its own reset back to ABL (piano-reset-after=420 baked into the image)'
  $until=(Get-Date).AddSeconds($WaitBack); $back=$false
  while((Get-Date) -lt $until){ if((FastDevices) -match "(?m)^$Serial\s+fastboot"){ $back=$true; break }; Start-Sleep -Milliseconds 800 }
  Line ("ABL back after {0:N0} s: {1}" -f ((Get-Date)-$t0).TotalSeconds,$back)
  if(!$back){ Line 'the probe never came back; recorded'; exit 4 }
  (& $f -s $Serial boot $reader 2>&1 | Out-Null)
  if(!(WaitReader $WaitReader)){ Line 'reader never enumerated'; exit 5 }
  $o = (& $f -s piano-uefi oem dump $Base $Size 2>&1 | Out-String)
  Line ("dump rc=$LASTEXITCODE : " + (($o -replace '\s+',' ').Trim() -replace '^(.{0,110}).*','$1'))
  if($LASTEXITCODE -eq 0){
    $staged = Join-Path $dir ("mailbox-{0}-{1}.bin" -f $Base,$Size)
    (& $f -s piano-uefi get_staged $staged 2>&1 | Out-Null)
    $fi = Get-Item -LiteralPath $staged -ErrorAction SilentlyContinue
    if($fi -and $fi.Length -gt 0){ $b=[IO.File]::ReadAllBytes($staged); $nz=0; foreach($x in $b){ if($x -ne 0){ $nz++ } }; Line ("mailbox staged {0} bytes nonzero={1}" -f $fi.Length,$nz) } else { Line 'mailbox staged empty' }
  }
} finally {
  Stop-Process -Name fastboot -Force -ErrorAction SilentlyContinue
  Line 'letting the reader boot the installed system by itself; waiting for HyperOS on adb'
  $until=(Get-Date).AddSeconds(420); $done=''
  while((Get-Date) -lt $until){ Start-Sleep 5; if((AdbDevices) -match "(?m)^$Serial\s+device"){ $done=((& $a -s $Serial shell getprop sys.boot_completed 2>&1 | Out-String).Trim()); if($done -eq '1'){ break } } }
  Line ("hyperos=" + $(if($done -eq '1'){'up'}else{'NOT-UP'}))
  Line "log=$dir"
}
