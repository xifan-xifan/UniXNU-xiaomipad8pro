# Small-window readback of the ufs256 crash site. fastboot boot only, no writes.
# Stage 1: ufs256 (VBAR fix, no probe) -> natural death, returns to fastboot.
# Stage 2: fastboot reboot bootloader + settle (the successful captures never booted the
#          reader from the fastboot session that had just seen the test image return).
# Stage 3: reader.img, then ONE small dump: the in-image piano_stage_slots table
#          (PA 0x8c5912880 = _piano_stage_slots 0xfffffff00c912880 - 0xfffffff007004000 + 0x8c0004000, 0x2000) which is where piano_dbg_kv() keeps probe91's
#          ESR/FAR/PC/x16/x17. Falls back to a 0x1000 dump and then the 7 MiB console.
$ErrorActionPreference='Continue'
$d='7ccdd855'; $pt='E:\project\UniXNU-xiaomipad8pro\out\pt33\platform-tools'; $a="$pt\adb.exe"; $f="$pt\fastboot.exe"
Set-Location (Join-Path $PSScriptRoot '..\..\out\no-demura-control')
Start-Transcript -Path ufs256-dump2-live.log -Force | Out-Null
function Fast($serial,$seconds){$until=(Get-Date).AddSeconds($seconds); do{ $r=(& $f devices 2>&1) -join ' '; if($r -match "(^|\s)$([regex]::Escape($serial))(\s|$)"){return $true}; Start-Sleep -Milliseconds 400 }while((Get-Date) -lt $until); return $false}
function Dump($label,$addr,$size){
  for($i=1;$i -le 3;$i++){
    $out=(& $f -s piano-uefi oem dump $addr $size 2>&1 | Out-String)
    $out=$out.Trim()
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
  $h=(Get-FileHash boot-piano-ufs256cfg.img).Hash
  if($h -ne 'D68EB6E2409B799012E5DDA68F1CAC7355DB79E8F44200CC9CC050AA403D472B'){throw "image identity $h"}
  if((Get-FileHash reader.img).Hash -ne '708D0E79FFA8ADAE5227EAE5DBDFF27121DA4FA7E9530DB31F71DD5153570468'){throw 'reader identity'}
  if(!((& $f devices) -match "^$d\s")){ "no fastboot device; abort"; exit 2 }
  $t0=Get-Date
  & $f -s $d boot boot-piano-ufs256cfg.img
  "test boot rc=$LASTEXITCODE"
  $returned=Fast $d 120
  "PROBE fastboot-after {0:N1} s returned={1}" -f ((Get-Date)-$t0).TotalSeconds,$returned
  if(!$returned){ 'test image never returned'; exit 3 }
  & $f -s $d reboot bootloader | Out-Null
  Start-Sleep -Seconds 6
  if(!(Fast $d 60)){ 'no fastboot after reboot bootloader'; exit 4 }
  "devices before reader:"; (& $f devices)
  & $f -s $d boot reader.img 2>&1 | Out-String | ForEach-Object { "reader boot: " + $_.Trim() }
  if(!(Fast 'piano-uefi' 30)){ 'piano-uefi did not appear'; exit 5 }
  "reader up after {0:N1}s" -f ((Get-Date)-$t0).TotalSeconds
  Start-Sleep -Milliseconds 1500
  $ok = Dump 'ufs256-slots' '8c5912880' '2000'
  if(!$ok){ $ok = Dump 'ufs256-slots1k' '8c5912880' '1000' }
  if(!$ok){ $ok = Dump 'ufs256-panic' '8c5945000' '20000' }
  if(!$ok){ $ok = Dump 'ufs256-console' '9cb00000' '700000' }
  "readback_ok=$ok"
  if(Fast 'piano-uefi' 2){ & $f -s piano-uefi reboot 2>&1 | Out-String | ForEach-Object { "reader reboot: " + $_.Trim() } }
} finally {
  Stop-Transcript | Out-Null
}

