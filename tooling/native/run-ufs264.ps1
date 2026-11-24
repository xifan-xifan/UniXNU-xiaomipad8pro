# ufs264: the VBAR fix plus the timer no longer switched off in the hand-written PPI 27
# branch, with a tick counter in piano_dbg_kv(0x50). fastboot boot only, no writes.
# After it returns, dump the in-image slot table (PA 0x8c5912880, 8 KiB) through the
# menu reader and decode the tick count and any synchronous-fault registers.
$ErrorActionPreference='Continue'
$d='7ccdd855'; $pt='E:\project\UniXNU-xiaomipad8pro\out\pt33\platform-tools'; $a="$pt\adb.exe"; $f="$pt\fastboot.exe"
Set-Location (Join-Path $PSScriptRoot '..\..\out\no-demura-control')
Start-Transcript -Path ufs264-live.log -Force | Out-Null
function Fast($serial,$seconds){$until=(Get-Date).AddSeconds($seconds); do{ $r=(& $f devices 2>&1) -join ' '; if($r -match "(^|\s)$([regex]::Escape($serial))(\s|$)"){return $true}; Start-Sleep -Milliseconds 400 }while((Get-Date) -lt $until); return $false}
function Dump($label,$addr,$size){
  for($i=1;$i -le 3;$i++){
    $out=((& $f -s piano-uefi oem dump $addr $size 2>&1 | Out-String).Trim())
    "dump#$i $label $addr $size rc=$LASTEXITCODE : $out"
    if($LASTEXITCODE -eq 0){
      & $f -s piano-uefi get_staged "$label.bin" 2>&1 | Out-String | ForEach-Object { "get_staged: " + $_.Trim() }
      $fi=Get-Item "$label.bin" -ErrorAction SilentlyContinue
      if($fi -and $fi.Length -gt 0){ "staged $label.bin = $($fi.Length) bytes sha=$((Get-FileHash $label.bin).Hash)"; return $true }
    }
    if(!(Fast 'piano-uefi' 5)){ "reader gone after dump#$i"; return $false }
    Start-Sleep -Milliseconds 800
  }
  return $false
}
try {
  $h=(Get-FileHash boot-piano-ufs264cfg.img).Hash
  if($h -ne 'D74FAD3A4B8425BBC3FE929C776A985EAA2A69AADBA30770CC7BAF63B1E83A4A'){throw "image identity $h"}
  if(!((& $f devices) -match "^$d\s")){ "no fastboot device; abort"; exit 2 }
  $t0=Get-Date
  & $f -s $d boot boot-piano-ufs264cfg.img
  "test boot rc=$LASTEXITCODE"
  $returned=Fast $d 150
  "PROBE fastboot-after {0:N1} s returned={1}" -f ((Get-Date)-$t0).TotalSeconds,$returned
  if(!$returned){ 'test image never returned'; exit 3 }
  & $f -s $d reboot bootloader | Out-Null
  Start-Sleep -Seconds 6
  if(!(Fast $d 60)){ 'no fastboot after reboot bootloader'; exit 4 }
  & $f -s $d boot reader.img 2>&1 | Out-String | ForEach-Object { "reader boot: " + $_.Trim() }
  if(!(Fast 'piano-uefi' 30)){ 'piano-uefi did not appear'; exit 5 }
  Start-Sleep -Milliseconds 1500
  $ok = Dump 'ufs264-slots' '8c5912880' '2000'
  if($ok -and (Test-Path 'ufs264-slots.bin')){ python ..\..\tooling\native\decode-slots.py ufs264-slots.bin }
  if(!$ok){ Dump 'ufs264-panic' '8c5945000' '20000' | Out-Null }
  "readback_ok=$ok"
  if(Fast 'piano-uefi' 2){ & $f -s piano-uefi reboot 2>&1 | Out-String | ForEach-Object { "reader reboot: " + $_.Trim() } }
} finally {
  Stop-Transcript | Out-Null
}

