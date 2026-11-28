# Stamp the arrival time of every `PianoPower: ... rtc: <count> (<calendar>)` console line.
# The console monitor's live stream is the only place the PMIC RTC readout appears, and capture-com.ps1 time-stamps only its
# own open/close events, so an rtc line cannot be placed on the host clock from a capture. This reads one CDC-ACM port,
# survives the port disappearing across a reset, and writes `GOT <hh:mm:ss.fff> count= cal=` per readout: pair those arrivals
# with a host clock the network time service has corrected (`ssh <mac> date +%s` next to `UtcNow`) and the boot calendar's
# error falls out to ~0.1 s. tooling/native/rtc-offset.py derives the same number from a finished capture; this measures a
# running system, which is what deciding piano-rtc-offset needs.
#   tooling/native/rtc-stamp.ps1 -Ports COM5 -Seconds 330 -OutFile logs\rtc-stamp-live.txt
param([string[]]$Ports=@('COM5'),[int]$Seconds=330,[string]$OutFile='logs\rtc-stamp-live.txt')
$ErrorActionPreference='Continue'
$end=(Get-Date).AddSeconds($Seconds)
$base = if([IO.Path]::IsPathRooted($OutFile)){ $OutFile } else { Join-Path (Get-Location).Path $OutFile }
$sp=$null
while((Get-Date) -lt $end){
  if($sp -eq $null){
    foreach($p in $Ports){
      try{
        $o=New-Object IO.Ports.SerialPort -ArgumentList @($p,115200,'None',8,'One')
        $o.ReadBufferSize=1048576; $o.ReadTimeout=1000; $o.NewLine="`n"; $o.DtrEnable=$true; $o.RtsEnable=$true
        $o.Open(); $sp=$o; [IO.File]::AppendAllText($base,("OPEN {0} {1:HH:mm:ss.fff}`n" -f $p,(Get-Date))); break
      } catch { }
    }
    if($sp -eq $null){ Start-Sleep -Milliseconds 700; continue }
  }
  try{
    $t=Get-Date
    $s=$sp.ReadLine()
    if($s -match 'rtc: (\d+) \(([^)]+)\)'){
      [IO.File]::AppendAllText($base,("GOT {0:HH:mm:ss.fff} count={1} cal={2}`n" -f $t,$Matches[1],$Matches[2]))
    }
  } catch [TimeoutException] {
  } catch {
    [IO.File]::AppendAllText($base,("CLOSE {0:HH:mm:ss.fff} {1}`n" -f (Get-Date),$_.Exception.GetType().Name))
    try{ $sp.Close() } catch { }
    $sp=$null
  }
}
if($sp -ne $null){ try{ $sp.Close() } catch { } }
[IO.File]::AppendAllText($base,("END {0:HH:mm:ss.fff}`n" -f (Get-Date)))