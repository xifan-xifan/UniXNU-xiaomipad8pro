# Runs out/no-demura-control/boot-piano-paclr53-ufslog.img and measures the seconds until the
# fastboot serial returns, then stages the pstore console and panic zone. fastboot boot only.
# See docs/results-2026-11-23-paclr53-ufslog.md.
$ErrorActionPreference='Stop'
$d='7ccdd855'; $pt='C:\Users\ZhangQi\AppData\Local\Android\platform-tools'; $a="$pt\adb.exe"; $f="$pt\fastboot.exe"
Set-Location (Join-Path (Split-Path $PSScriptRoot -Parent) '..\out\no-demura-control')
Start-Transcript -Path paclr53-ufslog-live.log -Force | Out-Null
function Wait-Fast($serial,$seconds){$until=(Get-Date).AddSeconds($seconds); do {if((& $f devices) -match "^$serial\s"){return $true}; Start-Sleep -Milliseconds 400} while((Get-Date) -lt $until); return $false}
try {
if((Get-FileHash boot-piano-paclr53-ufslog.img).Hash -ne '4E7F28C96E0F460B806749972C422DBA7D503956EEA2851DB7BAF5FD775D1763' -or (Get-FileHash reader.img).Hash -ne '708D0E79FFA8ADAE5227EAE5DBDFF27121DA4FA7E9530DB31F71DD5153570468'){throw 'Image identity'}
if(!((& $f devices) -match "^$d\s")){ & $a -s $d reboot bootloader | Out-Null }
if(!(Wait-Fast $d 60)){throw 'Fastboot did not start'}
$ErrorActionPreference='Continue'
foreach($key in 'product','current-slot','is-userspace','slot-successful:a','slot-unbootable:a','slot-retry-count:a','snapshot-update-status'){$v=(& $f -s $d getvar $key 2>&1 | Out-String); $expect=@{'product'='piano';'current-slot'='a';'is-userspace'='no';'slot-successful:a'='yes';'slot-unbootable:a'='no';'slot-retry-count:a'='6';'snapshot-update-status'='none'}[$key]; if($v -notmatch ('(?m)^\s*(?:fastboot\.exe : )?'+[regex]::Escape($key)+':\s*'+$expect+'\s*$')){& $f -s $d reboot; throw "ABL preflight $key"}}
$t0=Get-Date
& $f -s $d boot boot-piano-paclr53-ufslog.img
if($LASTEXITCODE -ne 0){if(Wait-Fast $d 5){& $f -s $d reboot}; throw 'Test boot rejected'}
$returned=Wait-Fast $d 90
"fastboot after {0:N1} s" -f ((Get-Date)-$t0).TotalSeconds
if($returned){
 & $f -s $d boot reader.img
 "reader boot rc=$LASTEXITCODE"
 if($LASTEXITCODE -eq 0 -and (Wait-Fast 'piano-uefi' 25)){
  & $f -s piano-uefi oem dump 9cb00000 700000
  & $f -s piano-uefi get_staged paclr53-ufslog-console.bin
  & $f -s piano-uefi oem dump 8c5945000 20000
  & $f -s piano-uefi get_staged paclr53-ufslog-panic.bin
 } else { 'reader did not appear as piano-uefi' }
}
if(Wait-Fast $d 2){& $f -s $d reboot}
$until=(Get-Date).AddSeconds(180)
do { $adbDevices=& $a devices; if($adbDevices -match "^$d\s+device\s*$"){if((& $a -s $d shell getprop sys.boot_completed).Trim() -eq '1'){& $a -s $d shell getprop ro.build.version.incremental; 'boot_completed=1'; exit}}; Start-Sleep -Seconds 2 } while((Get-Date) -lt $until)
throw 'Android return not observed'
} finally { Stop-Transcript | Out-Null }


