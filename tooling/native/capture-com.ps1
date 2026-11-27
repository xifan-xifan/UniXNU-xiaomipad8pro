# Captures the tablet's PianoDwc3 CDC-ACM ports (piano-usb=2) on Windows, without external packages.
# Ports appear/disappear with the gadget; every byte is written raw and the open/close events are logged inline.
param([string]$OutFile='ufs227-com.log',[int]$Seconds=200,[string[]]$ExcludePorts=@())
$ErrorActionPreference='Continue'
if([IO.Path]::IsPathRooted($OutFile)){ $log=$OutFile } else { $log=Join-Path (Get-Location).Path $OutFile }   # Join-Path in 5.1 glues an absolute child onto the cwd instead of using it, which used to throw here and leave the boot uncaptured
$sw=New-Object IO.StreamWriter($log,$false,[Text.Encoding]::UTF8)
function Log($s){ $sw.WriteLine(('### {0:HH:mm:ss.fff} {1}' -f (Get-Date),$s)); $sw.Flush() }
$ports=@{}
$deadline=(Get-Date).AddSeconds($Seconds)
Log "capture start, ${Seconds}s"
while((Get-Date) -lt $deadline){
  foreach($name in [IO.Ports.SerialPort]::GetPortNames()){
    if($ExcludePorts -contains $name){ continue }
    if(-not $ports.ContainsKey($name)){
      try{
        $sp=New-Object IO.Ports.SerialPort -ArgumentList @($name,115200,'None',8,'One')
        $sp.ReadBufferSize=65536; $sp.ReadTimeout=100; $sp.DtrEnable=$true; $sp.RtsEnable=$true
        $sp.Open()
        $ports[$name]=$sp
        Log "OPEN $name"
      }catch{ Log ("open failed $name : " + $_.Exception.Message) }
    }
  }
  foreach($k in @($ports.Keys)){
    $sp=$ports[$k]
    if(-not $sp.IsOpen){ Log "CLOSED $k"; $ports.Remove($k); continue }
    try{
      while($sp.BytesToRead -gt 0){
        $n=$sp.BytesToRead
        $buf=New-Object byte[] $n
        $r=$sp.Read($buf,0,$n)
        if($r -le 0){break}
        $sw.Write([Text.Encoding]::ASCII.GetString($buf,0,$r)); $sw.Flush()
      }
    }catch{ Log ("read error $k : " + $_.Exception.Message) }
  }
  Start-Sleep -Milliseconds 40
}
foreach($sp in $ports.Values){ try{$sp.Close()}catch{} }
Log "capture end"
$sw.Close()
