# Is the RAM readback channel alive? One oem dump, as the first command of a reader session.
# fastboot boot only; nothing is written. Docs: docs/results-2026-11-25-dump-channel.md.
param([string]$Serial='7ccdd855', [string]$Addr='8c0004000', [string]$Size='100', [int]$WaitBoot=120, [int]$WaitReader=150)
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'; $a = Join-Path $pt 'adb.exe'
$reader = Join-Path $root 'out\no-demura-control\reader.img'
$recover = Join-Path $PSScriptRoot 'ensure-hyperos.ps1'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dir = Join-Path $root ("logs\native-runs\dump-channel-$stamp")
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'run.log'
function Line($m){ $line = "$((Get-Date).ToString('HH:mm:ss.fff')) $m"; Add-Content -LiteralPath $log -Value $line; $line }
function ShaH($p){ if(!(Test-Path $p)){ return '' }; (Get-FileHash -LiteralPath $p).Hash.ToLower() }
function FastDevices { ((& $f devices 2>&1 | Out-String)) }
function AdbDevices { ((& $a devices 2>&1 | Out-String)) }
function WaitFast([string]$s,[int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match "(?m)^$s\s+fastboot"){ return $true }; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
function WaitReader([int]$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ if((FastDevices) -match '(?m)^piano-uefi\s+fastboot'){ return $true }; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
try {
  if((ShaH $reader) -ne '708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468'){ Line 'reader identity mismatch'; exit 1 }
  $s = ''
  foreach($x in [regex]::Matches((FastDevices), '(?m)^([0-9a-fA-F]{8})\s+fastboot')){ if($Serial -eq '' -or $x.Groups[1].Value -eq $Serial){ $s = $x.Groups[1].Value } }
  if($s -eq ''){
    foreach($x in [regex]::Matches((AdbDevices), '(?m)^([0-9a-fA-F]{8})\s+device')){ if($Serial -eq '' -or $x.Groups[1].Value -eq $Serial){ $s = $x.Groups[1].Value } }
    if($s -eq ''){ Line 'no adb and no fastboot device'; exit 2 }
    Line "serial=$s on adb; rebooting into ABL"
    (& $a -s $s reboot bootloader 2>&1 | Out-Null)
    if(!(WaitFast $s 120)){ Line 'device did not reach fastboot'; exit 2 }
  }
  Line "serial=$s in ABL; booting the reader (no getvar first: it poisons the session)"
  (& $f -s $s boot $reader 2>&1 | Out-File (Join-Path $dir 'boot.txt'))
  if(!(WaitReader $WaitReader)){ Line 'reader never enumerated'; exit 3 }
  $t0 = Get-Date
  $o = (& $f -s piano-uefi oem dump $Addr $Size 2>&1 | Out-String)
  Line ("dump rc={0} after {1:N1}s : {2}" -f $LASTEXITCODE, ((Get-Date)-$t0).TotalSeconds, ($o -replace '\s+',' ').Trim())
  if($LASTEXITCODE -eq 0){
    $staged = Join-Path $dir ("dump-{0}.bin" -f $Addr)
    (& $f -s piano-uefi get_staged $staged 2>&1 | Out-File (Join-Path $dir 'get_staged.txt'))
    $fi = Get-Item -LiteralPath $staged -ErrorAction SilentlyContinue
    if($fi){ Line ("staged {0} bytes, sha256 {1}" -f $fi.Length, (Get-FileHash -LiteralPath $staged).Hash.ToLower()) } else { Line 'nothing staged' }
    Line 'channel=alive'
  } else {
    Line 'channel=dead (a faulted dump takes the reader session with it)'
  }
} finally {
  Line 'returning the tablet to HyperOS'
  & $recover -Serial $Serial -WaitBoot 300 -Log $log
  Line "ensure-hyperos exit=$LASTEXITCODE"
}