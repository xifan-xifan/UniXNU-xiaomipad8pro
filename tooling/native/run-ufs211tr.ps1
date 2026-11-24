# Runs out/no-demura-control/boot-piano-ufs211cfg.img (keep-all KC; synchronous media registration) and captures
# the console through reader.img, then leaves the tablet on Android. fastboot boot only.
$ErrorActionPreference='Stop'
$d='7ccdd855'; $pt='C:\Users\ZhangQi\AppData\Local\Android\platform-tools'; $a="$pt\adb.exe"; $f="$pt\fastboot.exe"
Set-Location (Join-Path (Split-Path $PSScriptRoot -Parent) '..\out\no-demura-control')
Start-Transcript -Path ufs211tr-live.log -Force | Out-Null
function Wait-Fast($serial,$seconds){$until=(Get-Date).AddSeconds($seconds); do {if((& $f devices) -match "^$serial\s"){return $true}; Start-Sleep -Milliseconds 400} while((Get-Date) -lt $until); return $false}
try {
function ShaH($p){ $full=Join-Path (Get-Location).Path $p; $fs=[IO.File]::OpenRead($full); try { $sha=[Security.Cryptography.SHA256]::Create(); (($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('x2') }) -join '') } finally { $fs.Dispose() } }
if((ShaH boot-piano-ufs211cfg.img) -ne '9952514cea44146ef3cff13b33d6fb556a7cd1b27631755bd149094520bbf84b' -or (ShaH reader.img) -ne '708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468'){throw 'Image identity'}
if(!((& $f devices) -match "^$d\s")){ & $a -s $d reboot bootloader | Out-Null }
if(!(Wait-Fast $d 60)){throw 'Fastboot did not start'}
$ErrorActionPreference='Continue'
foreach($key in 'product','current-slot','is-userspace','slot-successful:a','slot-unbootable:a','slot-retry-count:a','snapshot-update-status'){$v=(& $f -s $d getvar $key 2>&1 | Out-String); $expect=@{'product'='piano';'current-slot'='a';'is-userspace'='no';'slot-successful:a'='yes';'slot-unbootable:a'='no';'slot-retry-count:a'='6';'snapshot-update-status'='none'}[$key]; if($v -notmatch ('(?m)^\s*(?:fastboot\.exe : )?'+[regex]::Escape($key)+':\s*'+$expect+'\s*$')){& $f -s $d reboot; throw "ABL preflight $key"}}
$t0=Get-Date
& $f -s $d boot boot-piano-ufs211cfg.img
if($LASTEXITCODE -ne 0){if(Wait-Fast $d 5){& $f -s $d reboot}; throw 'Test boot rejected'}
$returned=Wait-Fast $d 320
"fastboot after {0:N1} s" -f ((Get-Date)-$t0).TotalSeconds
if($returned){
 & $f -s $d boot reader.img
 "reader boot rc=$LASTEXITCODE"
 if($LASTEXITCODE -eq 0 -and (Wait-Fast 'piano-uefi' 25)){
  & $f -s piano-uefi oem dump 9cb00000 700000
  & $f -s piano-uefi get_staged ufs211-console.bin
  & $f -s piano-uefi oem dump 8c5945000 21100
  & $f -s piano-uefi get_staged ufs211-panic.bin
 } else { 'reader did not appear as piano-uefi' }
}
if(Wait-Fast $d 2){& $f -s $d reboot}
$until=(Get-Date).AddSeconds(180)
do { $adbDevices=& $a devices; if($adbDevices -match "^$d\s+device\s*$"){if((& $a -s $d shell getprop sys.boot_completed).Trim() -eq '1'){& $a -s $d shell getprop ro.build.version.incremental; 'boot_completed=1'; exit}}; Start-Sleep -Seconds 2 } while((Get-Date) -lt $until)
throw 'Android return not observed'
} finally { Stop-Transcript | Out-Null }










































