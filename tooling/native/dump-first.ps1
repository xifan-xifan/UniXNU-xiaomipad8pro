param([Parameter(Mandatory=$true)][string]$Addr,[string]$Size='100',[string]$Out='d.bin')
$ErrorActionPreference='Continue'
$d='7ccdd855'; $pt='E:\project\UniXNU-xiaomipad8pro\out\pt33\platform-tools'
$f="$pt\fastboot.exe"; $a="$pt\adb.exe"
Set-Location (Join-Path $PSScriptRoot '..\..\out\no-demura-control')
function Fast($serial,$seconds){ $until=(Get-Date).AddSeconds($seconds); do{ $r=(& $f devices 2>&1) -join ' '; if($r -match "(^|\s)$([regex]::Escape($serial))(\s|$)"){return $true}; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
function AdbUp($seconds){ $until=(Get-Date).AddSeconds($seconds); do{ $r=(& $a devices 2>&1) -join ' '; if($r -match "(^|\s)$([regex]::Escape($d))(\s+device)"){return $true}; Start-Sleep -Milliseconds 500 }while((Get-Date) -lt $until); return $false }
if(AdbUp 2){ & $a -s $d reboot bootloader 2>&1 | Out-Null }
if(!(Fast $d 90)){ 'no fastboot'; exit 2 }
& $f -s $d boot reader.img 2>&1 | Out-Null
if(!(Fast 'piano-uefi' 120)){ 'no reader'; exit 3 }
Start-Sleep -Seconds 2
& $f -s piano-uefi oem dump $Addr $Size 2>&1 | ForEach-Object { "dump: $_" }
$rc=$LASTEXITCODE
if($rc -ne 0){ exit $rc }
& $f -s piano-uefi get_staged $Out 2>&1 | Out-Null
if(Test-Path $Out){ $b=[IO.File]::ReadAllBytes($Out); "addr=$Addr bytes=$($b.Length) nonzero=$((@($b | Where-Object {$_ -ne 0})).Count) sha=$((Get-FileHash $Out).Hash)" } else { 'no staged file' }
exit $rc
