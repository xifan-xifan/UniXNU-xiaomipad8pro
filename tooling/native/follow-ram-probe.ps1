# Follower for an already-running macOS RAM probe: wait for its self-reset back to ABL, one reader boot, one cold
# mailbox dump, then let the reader boot the installed system.
param([string]$Serial='7ccdd855', [string]$Base='9cb00000', [string]$Size='700000', [int]$WaitAbl=900, [string]$Tag='rescue2')
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'; $a = Join-Path $pt 'adb.exe'
$reader = Join-Path $root 'out\no-demura-control\reader.img'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dir = Join-Path $root ("logs\native-runs\ram-k273-$Tag-$stamp")
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'run.log'
function Line($m){ $line = "$((Get-Date).ToString('HH:mm:ss.fff')) $m"; Add-Content -LiteralPath $log -Value $line; $line }
function FastDevices { ((& $f devices 2>&1 | Out-String)) }
function AdbDevices { ((& $a devices 2>&1 | Out-String)) }
function WaitFast([string]$s,[int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match "(?m)^$s\s+fastboot"){ return $true }; Start-Sleep -Milliseconds 800 }while((Get-Date) -lt $until); return $false }
function WaitReader([int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match '(?m)^piano-uefi\s+fastboot'){ return $true }; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
try {
  Line 'waiting for the running probe to reset itself back to ABL'
  if(!(WaitFast $Serial $WaitAbl)){ Line 'the probe never returned to ABL'; exit 3 }
  Line ('ABL seen at ' + (Get-Date).ToString('HH:mm:ss'))
  Start-Sleep -Seconds 4
  (& $f -s $Serial boot $reader 2>&1 | Out-Null)
  if(!(WaitReader 150)){ Line 'reader never enumerated'; exit 5 }
  Start-Sleep -Seconds 2
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
