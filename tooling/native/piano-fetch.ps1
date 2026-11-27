# Pull one file off the tablet through the user pipe (piano-usb-shell=1 plus pianousb-shell), base64 in chunks, and verify it.
# The pipe is CDC-ACM, so there is no real baud limit; what limits one reply is how fast this drains it, hence the chunks.
# Each request brackets its payload between two markers on a line of their own, so the terminal echoing the command back can
# never be mistaken for data. tooling/native/piano-shell.ps1 is the single-command version of the same pipe.
#   tooling/native/piano-fetch.ps1 -Remote /private/tmp/x.png -Local out\x.png
#   tooling/native/piano-fetch.ps1 -Remote /private/var/log/pianousb-shell.log -Local out\pianousb-shell.log -Chunk 32768
param([Parameter(Mandatory=$true)][string]$Remote, [Parameter(Mandatory=$true)][string]$Local,
      [int]$Chunk=12288, [string]$Port='', [int]$WaitSeconds=90, [string]$Sha256='')
$ErrorActionPreference='Continue'
function ReadQuiet($sp,[int]$ms,[int]$quietMs){
  $sb=New-Object Text.StringBuilder; $last=Get-Date; $end=(Get-Date).AddMilliseconds($ms)
  while((Get-Date) -lt $end){
    if($sp.BytesToRead -gt 0){
      $n=$sp.BytesToRead; $b=New-Object byte[] $n; $r=$sp.Read($b,0,$n)
      $sb.Append([Text.Encoding]::UTF8.GetString($b,0,$r)) | Out-Null; $last=Get-Date
    } elseif(((Get-Date)-$last).TotalMilliseconds -ge $quietMs -and $sb.Length -gt 0){ break }
    Start-Sleep -Milliseconds 20
  }
  return $sb.ToString()
}
function OpenShell([string]$Port){
  foreach($name in [IO.Ports.SerialPort]::GetPortNames()){
    if($Port -ne '' -and $name -ne $Port){ continue }
    try{
      $sp=New-Object IO.Ports.SerialPort -ArgumentList @($name,115200,'None',8,'One')
      $sp.ReadBufferSize=1048576; $sp.ReadTimeout=200; $sp.WriteTimeout=5000; $sp.DtrEnable=$true; $sp.RtsEnable=$true
      $sp.Open()
    }catch{ "open failed $name : " + $_.Exception.Message; continue }
    $sp.DiscardInBuffer(); $sp.Write('echo PIANO_$((6*8))'); $sp.Write("`r")
    $probe=ReadQuiet $sp 2500 700
    if($probe -match 'PIANO_48'){ "shell: $name"; return $sp }
    "not the user pipe: $name (" + (($probe -replace '\s+',' ').Trim() -replace '^(.{0,60}).*','$1') + ')'
    $sp.Close()
  }
  return $null
}
function Ask($sp,[string]$command,[int]$seconds){
  $id=[Guid]::NewGuid().ToString('N').Substring(0,10)
  $sp.DiscardInBuffer()
  $sp.Write("echo B-$id`n$command`necho E-$id`r")
  $sb=New-Object Text.StringBuilder; $end=(Get-Date).AddSeconds($seconds)
  while((Get-Date) -lt $end){
    $c=ReadQuiet $sp 5000 900
    if($c){ $sb.Append($c) | Out-Null; if($sb.ToString() -match ("(?m)^E-" + $id + "\s*$")){ break } }
  }
  $text=([regex]::Replace($sb.ToString(), [char]27 + '\[[0-9;?]*[a-zA-Z]', '') -replace "`r",'')
  $m=[regex]::Match($text,"(?m)^B-$id\s*\n([\s\S]*?)\n?E-$id")
  if(-not $m.Success){ return $null }
  return $m.Groups[1].Value.Trim()
}
  $found = @(OpenShell $Port)
  $sp = @($found | Where-Object { $_ -is [System.IO.Ports.SerialPort] }) | Select-Object -First 1
  foreach ($line in ($found | Where-Object { $_ -isnot [System.IO.Ports.SerialPort] })) { Write-Host $line }
if(-not $sp){ 'no user pipe answered; boot with piano-usb-shell=1 and the pianousb-shell helper'; exit 3 }
$sp.Write("stty columns 500 2>/dev/null; export PS1='P> '`r"); ReadQuiet $sp 4000 900 | Out-Null
try{
  $sizeText = Ask $sp "stat -f%z '$Remote'" $WaitSeconds
  if($null -eq $sizeText){ "no size for $Remote (does it exist?)"; exit 4 }
  $size = [int]($sizeText -split "`n" | Where-Object { $_ -match '^\d+$' } | Select-Object -First 1)
  "${Remote}: $size bytes"
  $b64 = New-Object Text.StringBuilder
  $offset = 0
  while($offset -lt $size){
    $ask = "dd if='$Remote' bs=$Chunk skip=$([int]($offset/$Chunk)) count=1 2>/dev/null | base64"
    $part = Ask $sp $ask $WaitSeconds
    if($null -eq $part){ "chunk at ${offset}: no reply"; exit 5 }
    $clean = ($part -replace '[^A-Za-z0-9+/=]','')
    $b64.Append($clean) | Out-Null
    $offset += $Chunk
    "  $([Math]::Min($offset,$size))/$size"
  }
  $bytes = [Convert]::FromBase64String($b64.ToString())
  if($bytes.Length -ne $size){ "size after decode ${bytes.Length} ne the reported size"; exit 6 }
  $h=[Security.Cryptography.SHA256]::Create()
  $got=(-join ($h.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }))
  "sha256 $got"
  if($Sha256 -ne '' -and $got -ne $Sha256.Trim().ToLower()){ 'sha256 does not match what was asked for'; exit 7 }
  [IO.File]::WriteAllBytes($Local,$bytes)
  "saved $Local ($($bytes.Length) bytes)"
} finally { $sp.Close() }
