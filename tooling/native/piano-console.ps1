# Send one line to one of the tablet's CDC-ACM ports and print what comes back, without deciding what the port is.
# Port 0 is the kernel's console monitor (piano-usb=2: it answers 'help'); port 1 is the user pipe (/dev/pianousb).
# tooling/native/piano-shell.ps1 finds the user pipe by itself and refuses to send anything to the monitor; this names the port.
#   tooling/native/piano-console.ps1 -Port COM5 -Line help -WaitSeconds 10
param([Parameter(Mandatory=$true)][string]$Port,[Parameter(Mandatory=$true)][string]$Line,[int]$WaitSeconds=15,[string]$OutFile='')
$sp=New-Object IO.Ports.SerialPort -ArgumentList @($Port,115200,'None',8,'One')
$sp.ReadBufferSize=1048576; $sp.ReadTimeout=200; $sp.WriteTimeout=3000; $sp.DtrEnable=$true; $sp.RtsEnable=$true
$sp.Open()
try{
  $sp.DiscardInBuffer()
  $sp.Write($Line + "`r")
  $sb=New-Object Text.StringBuilder; $end=(Get-Date).AddSeconds($WaitSeconds); $last=Get-Date
  while((Get-Date) -lt $end){
    if($sp.BytesToRead -gt 0){
      $n=$sp.BytesToRead; $b=New-Object byte[] $n; $r=$sp.Read($b,0,$n)
      $sb.Append([Text.Encoding]::UTF8.GetString($b,0,$r)) | Out-Null; $last=Get-Date
    } elseif($sb.Length -gt 0 -and ((Get-Date)-$last).TotalMilliseconds -ge 1500){ break }
    Start-Sleep -Milliseconds 30
  }
  $text=$sb.ToString()
  if($OutFile -ne ''){ [IO.File]::AppendAllText($OutFile,$text) }
  $text
} finally { $sp.Close() }
