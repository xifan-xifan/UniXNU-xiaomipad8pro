# Send one command to the tablet's user pipe (port 1, /dev/pianousb) and print the reply.
# Needs the image booted with piano-usb=2 piano-usb-shell=1 and the helper installed (docs/drivers/usb-device-debug.md T4);
# without piano-usb-shell=1 the pipe exists but nothing answers, and this tool reports "no user pipe" rather than sending
# anything to the kernel console port, whose monitor would act on what it receives.
#   tooling/native/piano-shell.ps1 -Command 'ls -l /Users/piano'
#   tooling/native/piano-shell.ps1 -Port COM6 -Command 'sync' -WaitSeconds 30
param([string]$Command='stty -a >/dev/null; echo PIANO_SHELL_ALIVE', [int]$WaitSeconds=20, [string]$Port='', [string]$OutFile='')
$ErrorActionPreference='Continue'
$sentinel='PIANO_SHELL_EXIT'
function ReadQuiet($sp,[int]$ms,[int]$quietMs){
  $sb=New-Object Text.StringBuilder; $last=Get-Date; $end=(Get-Date).AddMilliseconds($ms)
  while((Get-Date) -lt $end){
    if($sp.BytesToRead -gt 0){
      $n=$sp.BytesToRead; $b=New-Object byte[] $n; $r=$sp.Read($b,0,$n)
      $sb.Append([Text.Encoding]::UTF8.GetString($b,0,$r)) | Out-Null; $last=Get-Date
    } elseif(((Get-Date)-$last).TotalMilliseconds -ge $quietMs -and $sb.Length -gt 0){ break }
    Start-Sleep -Milliseconds 40
  }
  return $sb.ToString()
}
$cands=@([IO.Ports.SerialPort]::GetPortNames())
if(-not $cands -or $cands.Count -eq 0){ 'no serial ports attached (is the tablet booting with piano-usb=2?)'; exit 2 }
$shell=$null
foreach($name in $cands){
  if($Port -ne '' -and $name -ne $Port){ continue }
  try{
    $sp=New-Object IO.Ports.SerialPort -ArgumentList @($name,115200,'None',8,'One')
    $sp.ReadBufferSize=262144; $sp.ReadTimeout=200; $sp.WriteTimeout=2000; $sp.DtrEnable=$true; $sp.RtsEnable=$true
    $sp.Open()
  }catch{ "open failed $name : " + $_.Exception.Message; continue }
  try{
    $sp.DiscardInBuffer()
    $sp.Write("`r")
    $probe=ReadQuiet $sp 2500 700
    if($probe -match 'root#|root %|pianousb|#\s*$'){
      "shell: $name (probe: " + (($probe -replace '\s+',' ').Trim() -replace '^(.{0,60}).*','$1') + ')'
      $shell=$sp
      break
    }
    "not the user pipe: $name (" + (($probe -replace '\s+',' ').Trim() -replace '^(.{0,60}).*','$1') + ')'
    $sp.Close()
  }catch{ $sp.Dispose() }
}
if(-not $shell){ 'no user pipe answered; boot with piano-usb-shell=1 and the pianousb-shell helper'; exit 3 }
try{
  $shell.DiscardInBuffer()
  $shell.Write($Command + "; echo " + $sentinel + '`$?' + "`r")
  $sb=New-Object Text.StringBuilder; $end=(Get-Date).AddSeconds($WaitSeconds)
  while((Get-Date) -lt $end){
    $chunk=ReadQuiet $shell 4000 1200
    if($chunk){ $sb.Append($chunk) | Out-Null; if($sb.ToString() -match [regex]::Escape($sentinel)){ break } }
  }
  $text=$sb.ToString()
  if($OutFile -ne ''){ [IO.File]::AppendAllText($OutFile, $text) }
  $text
  if($text -notmatch [regex]::Escape($sentinel)){ '(no ' + $sentinel + ' within ' + $WaitSeconds + ' s)' }
}finally{ $shell.Close() }
