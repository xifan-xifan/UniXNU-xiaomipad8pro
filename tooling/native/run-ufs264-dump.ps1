# Warm-session readback of the ufs264 crash site. fastboot boot only, no writes.
# Same timing as run-ufs256-dump2.ps1, which captured 8 KiB from this reader.
$ErrorActionPreference='Continue'
$d='7ccdd855'; $pt='E:\project\UniXNU-xiaomipad8pro\out\pt33\platform-tools'; $a="$pt\adb.exe"; $f="$pt\fastboot.exe"
Set-Location (Join-Path $PSScriptRoot '..\..\out\no-demura-control')
Start-Transcript -Path ufs264-dump-live.log -Force | Out-Null
function Fast($serial,$seconds){$until=(Get-Date).AddSeconds($seconds); do{ $r=(& $f devices 2>&1) -join ' '; if($r -match "(^|\s)$([regex]::Escape($serial))(\s|$)"){return $true}; Start-Sleep -Milliseconds 400 }while((Get-Date) -lt $until); return $false}
function Dump($label,$addr,$size){
  for($i=1;$i -le 3;$i++){
    $out=(& $f -s piano-uefi oem dump $addr $size 2>&1 | Out-String)
    "dump#$i $label $addr $size rc=$LASTEXITCODE : $($out.Trim())"
    if($LASTEXITCODE -eq 0){
      & $f -s piano-uefi get_staged "$label.bin" 2>&1 | Out-Null
      $fi=Get-Item "$label.bin" -ErrorAction SilentlyContinue
      if($fi -and $fi.Length -gt 0){ "staged $label.bin = $($fi.Length) bytes sha=$((Get-FileHash $label.bin).Hash)"; return $true }
    }
    if(!(Fast 'piano-uefi' 5)){ "reader gone after dump#$i"; return $false }
    Start-Sleep -Milliseconds 800
  }
  return $false
}
try {
  if((Get-FileHash boot-piano-ufs264cfg.img).Hash -ne 'D74FAD3A4B8425BBC3FE929C776A985EAA2A69AADBA30770CC7BAF63B1E83A4A'){throw 'image identity'}
  if((Get-FileHash reader.img).Hash -ne '708D0E79FFA8ADAE5227EAE5DBDFF27121DA4FA7E9530DB31F71DD5153570468'){throw 'reader identity'}
  if(-not ((& $f devices) -match "^$d\s")){ & $a -s $d reboot bootloader 2>&1 | Out-Null; Start-Sleep -Seconds 9 }
  if(!(Fast $d 60)){ 'no fastboot'; exit 2 }
  $t0=Get-Date
  & $f -s $d boot boot-piano-ufs264cfg.img
  "test boot rc=$LASTEXITCODE"
  $returned=Fast $d 150
  "PROBE fastboot-after {0:N1} s returned={1}" -f ((Get-Date)-$t0).TotalSeconds,$returned
  if(!$returned){ 'test image never returned'; exit 3 }
  & $f -s $d reboot bootloader | Out-Null
  Start-Sleep -Seconds 6
  if(!(Fast $d 60)){ 'no fastboot after reboot bootloader'; exit 4 }
  & $f -s $d boot reader.img 2>&1 | Out-Null
  if(!(Fast 'piano-uefi' 45)){ 'piano-uefi did not appear'; exit 5 }
  Start-Sleep -Milliseconds 1500
  $ok = Dump 'ufs264-slots' '8c5912880' '2000'
  if(!$ok){ $ok = Dump 'ufs264-slots1k' '8c5912880' '1000' }
  if(!$ok){ $ok = Dump 'ufs264-console' '9cb00000' '700000' }
  "readback_ok=$ok"
} finally { Stop-Transcript | Out-Null }
