# ufs267 unattended: boot the probe image with piano-usb=3, capture the console,
# peek the stage-slot table, reset to fastboot. No partition write; fastboot boot only.
param([int]$BootWait=300, [int]$WaitUfs=180, [switch]$NoReset)
$ErrorActionPreference='Continue'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$pt = Join-Path $root 'out\pt33\platform-tools'
$f = Join-Path $pt 'fastboot.exe'
$img = Join-Path $root 'out\k27-usbc\boot-piano-ufs267-usbc.img'
$dir = Join-Path $root 'out\k27-usbc'
$want = '0be526f8e4ac7fbebb7b818c59ffd214e803a5b653191637c6d31767f41a5de3'
if((Get-FileHash -LiteralPath $img).Hash.ToLower() -ne $want){ throw 'image identity' }
$t0 = Get-Date
& $f boot $img 2>&1 | ForEach-Object { "{0:N1}s boot: $_" -f ((Get-Date)-$t0).TotalSeconds }
if($LASTEXITCODE -ne 0){ 'fastboot boot refused'; exit 3 }
Add-Type -AssemblyName System.IO.Ports
function Ports { [System.IO.Ports.SerialPort]::GetPortNames() }
function WaitPort([int]$seconds){
  $known = @(Ports)
  $until=(Get-Date).AddSeconds($seconds)
  while((Get-Date) -lt $until){
    $new = @(Ports) | Where-Object { $known -notcontains $_ }
    if($new){ return $new[0] }
    Start-Sleep -Milliseconds 500
  }
  return ''
}
$portName = WaitPort $BootWait
if($portName -eq ''){ 'no new COM port before the window closed'; exit 4 }
"port=$portName after {0:N1}s" -f ((Get-Date)-$t0).TotalSeconds
$sp = New-Object System.IO.Ports.SerialPort $portName,115200,8,None,1
$sp.ReadTimeout = 2000; $sp.NewLine="`r`n"; $sp.DtrEnable=$true; $sp.Open()
$log = Join-Path $dir 'ufs267-console.log'
$sw = [IO.StreamWriter]::new($log, $false, [Text.Encoding]::UTF8)
function Drain([int]$ms){
  $acc=''
  $until=(Get-Date).AddMilliseconds($ms)
  while((Get-Date) -lt $until){
    try { $n = $sp.BytesToRead } catch { $n = 0 }
    if($n -gt 0){ $buf=New-Object byte[] $n; $null=$sp.Read($buf,0,$n); $s=[Text.Encoding]::UTF8.GetString($buf); $acc+=$s; $until=(Get-Date).AddMilliseconds(1200) } else { if($acc -ne ''){ break } ; Start-Sleep -Milliseconds 200 }
  }
  if($acc -ne ''){ $sw.Write($acc); $sw.Flush() }
  return $acc
}
$deadline=(Get-Date).AddSeconds($WaitUfs)
$hit=$false
while((Get-Date) -lt $deadline){
  $s = Drain 5000
  if($s -match 'recordMediaParameters|media register|acceptNewMedia|panic|Assertion'){ $hit=$true; break }
}
"marker hit=$hit after {0:N1}s" -f ((Get-Date)-$t0).TotalSeconds
function Cmd([string]$c){ $sp.Write($c + "`r`n"); Start-Sleep -Milliseconds 300; return (Drain 8000) }
$help = Cmd 'help'
$sw.Write("`n===HELP====`n"); $sw.Write($help)
$peek = ''
$addr = 0x8c5912880
for($w=0; $w -lt 0x600; $w += 0x100){
  $r = Cmd ('peek {0:x} {1:x}' -f ($addr + $w*4), $w + 0x100)
  $peek += $r
  $sw.Write("`n===PEEK {0:x}====`n" -f ($addr + $w*4)); $sw.Write($r); $sw.Flush()
  "peek {0:x}: {1} chars" -f ($addr + $w*4), $r.Length
}
Set-Content -LiteralPath (Join-Path $dir 'ufs267-peek.txt') -Value $peek -Encoding ascii
if(-not $NoReset){ $null = Cmd 'reset fastboot'; Start-Sleep -Seconds 3 }
$sw.Close(); $sp.Close()
$until=(Get-Date).AddSeconds(90)
while((Get-Date) -lt $until){ if(((& $f devices 2>&1)|Out-String) -match '(?m)^[0-9a-f]{8}\s+fastboot'){ 'back in fastboot'; exit 0 }; Start-Sleep -Milliseconds 500 }
'still not in fastboot after the reset request'
exit 5
