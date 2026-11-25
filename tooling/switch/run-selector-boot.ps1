# Runs a combined boot_a (the stock Android kernel + firmware/selector + the UEFI payload) with `fastboot boot`, which writes
# nothing to any disk, and records where the boot actually landed. HyperOS coming up means the selector ran and handed the
# boot to the stock kernel; a piano-uefi fastboot device or a return to ABL means it handed the boot to the UEFI payload.
# Every run, whatever it landed on, ends back in HyperOS through ensure-hyperos.ps1.
#
#   run-selector-boot.ps1 -Image out\switch\boot-a-probe-android-20261126-signed.img -Expect android
#   run-selector-boot.ps1 -Image out\switch\boot-a-combined-20261126-signed.img      -Expect uefi
#
# ABL's `fastboot boot` on Windows does not always exit after it has handed the image over (the tablet boots anyway), so the
# transfer is capped at FastbootExitTimeout and the tablet, not the host process, is what the run then waits for.
param(
  [Parameter(Mandatory=$true)][string]$Image,
  [ValidateSet('android','uefi')][string]$Expect='android',
  [string]$ExpectSha256='',
  [string]$Serial='7ccdd855',
  [int]$WaitAndroid=300,
  [int]$FastbootExitTimeout=90,
  [string]$Log=''
)
$ErrorActionPreference='Continue'
$root = Join-Path $PSScriptRoot '..\..'
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'
$a = Join-Path $pt 'adb.exe'
$Image = (Resolve-Path -LiteralPath $Image).Path
if($Log -eq ''){ $Log = Join-Path $root ("logs\selector-{0}-{1}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'),([IO.Path]::GetFileNameWithoutExtension($Image))) }
New-Item -ItemType Directory -Force -Path (Split-Path $Log) | Out-Null
function Line($m){ Add-Content -LiteralPath $Log -Value ("{0} {1}" -f (Get-Date -Format 'HH:mm:ss'),$m) }
function FastText { ((& $f devices 2>&1) | Out-String) }
function AdbText { ((& $a devices 2>&1) | Out-String) }
function WaitFast([string]$name,[int]$seconds){
  $until=(Get-Date).AddSeconds($seconds)
  do { if((FastText) -match "(?m)^$name\s"){ return $true }; Start-Sleep -Milliseconds 400 } while((Get-Date) -lt $until)
  return $false
}
function WaitAdb([string]$s,[int]$seconds){
  $until=(Get-Date).AddSeconds($seconds)
  do { if((AdbText) -match "(?m)^$s\s+device"){ return $true }; Start-Sleep -Milliseconds 800 } while((Get-Date) -lt $until)
  return $false
}
function Getvar([string]$key){
  $raw=((& $f -s $Serial getvar $key 2>&1) | Out-String)
  $m=[regex]::Match($raw,[regex]::Escape($key)+':\s*(.+)')
  if($m.Success){ return $m.Groups[1].Value.Trim() }
  return ''
}

Line "=== selector run: expect=$Expect image=$Image ==="
$sha=(Get-FileHash -Algorithm SHA256 -LiteralPath $Image).Hash.ToLower()
Line "image sha256=$sha"
if($ExpectSha256 -ne '' -and $sha -ne $ExpectSha256.ToLower()){ Line 'ABORT: image identity'; exit 1 }

if((AdbText) -match "(?m)^$Serial\s+device"){ & $a -s $Serial reboot bootloader | Out-Null }
if(!(WaitFast $Serial 120)){ Line 'ABORT: no fastboot device, the tablet needs a power cycle'; exit 2 }
$want=[ordered]@{'product'='piano';'current-slot'='a';'is-userspace'='no';'slot-successful:a'='yes';'slot-unbootable:a'='no';'slot-retry-count:a'='6'}
$bad=''
foreach($key in $want.Keys){
  $v=Getvar $key
  Line ("ABL {0}={1}" -f $key,$v)
  if($v -ne $want[$key]){ $bad += ("{0}={1} " -f $key,$v) }
}
if($bad -ne ''){
  Line ("ABL preflight differs: {0}-- clearing the slot state before anything else" -f $bad)
  & $f -s $Serial --set-active=a 2>&1 | ForEach-Object { Line ("set-active: {0}" -f $_) }
  if(!(WaitFast $Serial 60)){ Line 'ABORT: tablet not on fastboot after set-active'; exit 2 }
}

$t0=Get-Date
$of=Join-Path $env:TEMP ("fb-boot-{0}.out" -f (Get-Date -Format 'yyyyMMddHHmmss'))
$p=Start-Process -FilePath $f -ArgumentList @('-s',$Serial,'boot',$Image) -NoNewWindow -PassThru -RedirectStandardOutput "$of.out" -RedirectStandardError "$of.err"
if($p.WaitForExit($FastbootExitTimeout*1000)){ $note='exited' } else { $null=$p.Kill(); $note='still attached after the transfer, killed' }
foreach($sfx in 'out','err'){
  $file="$of.$sfx"
  if(Test-Path -LiteralPath $file){ Get-Content -LiteralPath $file | Where-Object { $_ -ne '' } | ForEach-Object { Line ("fastboot: {0}" -f $_) }; Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }
}
Line ("fastboot boot {0} after {1:N1} s" -f $note,((Get-Date)-$t0).TotalSeconds)

if($Expect -eq 'android'){
  if(WaitAdb $Serial $WaitAndroid){
    $bc=((& $a -s $Serial shell getprop sys.boot_completed 2>&1) | Out-String).Trim()
    $inc=((& $a -s $Serial shell getprop ro.build.version.incremental 2>&1) | Out-String).Trim()
    $slot=((& $a -s $Serial shell getprop ro.boot.slot_suffix 2>&1) | Out-String).Trim()
    $up=((& $a -s $Serial shell uptime 2>&1) | Out-String).Trim()
    Line ("landed=android boot_completed={0} incremental={1} slot={2} uptime={3}" -f $bc,$inc,$slot,$up)
  } else {
    Line 'landed=not-android: no adb device inside the window'
  }
} else {
  $t1=Get-Date
  $uefi=WaitFast 'piano-uefi' 150
  if($uefi){
    Line ("uefi-fastboot appeared after {0:N1} s" -f ((Get-Date)-$t1).TotalSeconds)
    (& $f -s piano-uefi getvar product 2>&1) | ForEach-Object { Line ("uefi getvar: {0}" -f $_) }
  } elseif(WaitFast $Serial 150){
    Line ("returned to ABL fastboot after {0:N1} s with no UEFI fastboot device" -f ((Get-Date)-$t1).TotalSeconds)
    Line ("ABL slot-retry-count:a={0}" -f (Getvar 'slot-retry-count:a'))
  } else {
    Line 'no fastboot device of any kind inside the window'
  }
}

& (Join-Path $PSScriptRoot '..\native\ensure-hyperos.ps1') -Serial $Serial -Log $Log | Out-Null
Line ("ensure-hyperos rc={0}" -f $LASTEXITCODE)
Get-Content -LiteralPath $Log