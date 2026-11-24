param([string]$Addr='8c0916880',[string]$Size='1800',[string]$Out='ufs264-slots2.bin')
$ErrorActionPreference='Continue'
$d='7ccdd855'; $pt='E:\project\UniXNU-xiaomipad8pro\out\pt33\platform-tools'
$f="$pt\fastboot.exe"; $a="$pt\adb.exe"
Set-Location (Join-Path $PSScriptRoot '..\..\out\no-demura-control')
function ShaH($p){ (Get-FileHash $p).Hash.ToLower() }
if((ShaH 'boot-piano-ufs264cfg.img') -ne 'd74fad3a4b8425bbc3fe929c776a985eaa2a69aadba30770cc7baf63b1e83a4a'){ throw 'ufs264 image identity' }
if((ShaH 'reader.img') -ne '708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468'){ throw 'reader identity' }
function Fast($serial,$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ $r=(& $f devices 2>&1) -join ' '; if($r -match "(^|\s)$([regex]::Escape($serial))(\s|$)"){return $true}; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
function AdbUp($seconds){ $until=(Get-Date).AddSeconds($seconds); do{ $r=(& $a devices 2>&1) -join ' '; if($r -match "(^|\s)$([regex]::Escape($d))(\s+device)"){return $true}; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
if(-not (Fast $d 3)){
  if(!(AdbUp 5)){ 'no adb and no fastboot'; exit 2 }
  & $a -s $d reboot bootloader 2>&1 | Out-Null
  if(!(Fast $d 90)){ 'no fastboot'; exit 2 }
}
$t0=Get-Date
& $f -s $d boot boot-piano-ufs264cfg.img 2>&1 | ForEach-Object { "boot: $_" }
if($LASTEXITCODE -ne 0){ 'xnu boot refused'; exit 4 }
if(!(Fast $d 150)){ 'xnu never returned to fastboot'; exit 5 }
"xnu returned after {0:N1}s" -f ((Get-Date)-$t0).TotalSeconds
& $f -s $d reboot bootloader 2>&1 | Out-Null
Start-Sleep -Seconds 7
if(!(Fast $d 90)){ 'no fastboot after reboot'; exit 6 }
& $f -s $d boot reader.img 2>&1 | Out-Null
if(!(Fast 'piano-uefi' 120)){ 'no reader'; exit 7 }
Start-Sleep -Seconds 2
& $f -s piano-uefi oem dump $Addr $Size 2>&1 | ForEach-Object { "dump: $_" }
$rc=$LASTEXITCODE
if($rc -ne 0){ exit $rc }
& $f -s piano-uefi get_staged $Out 2>&1 | Out-Null
if(Test-Path $Out){ $b=[IO.File]::ReadAllBytes($Out); "bytes=$($b.Length) nonzero=$((@($b | Where-Object {$_ -ne 0})).Count)" } else { 'no staged file' }
exit 0

