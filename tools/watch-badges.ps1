<#
.SYNOPSIS
  Watch every attached badge's serial output at once and summarise rx/relay/resyncs/fps.

.DESCRIPTION
  Opens all attached badge COM ports (or -Ports) WITHOUT resetting them, reads
  for -Seconds, and parses the badge's one-per-second status lines:
    [badge] RECEIVER 31 fps | bass .. | rx 123 relay 4
    [badge] tx ok 4 fail 0 | resyncs 2 | boot attempts 0
  Prints a per-badge table: samples, fps min/avg/max, rx and relay deltas over
  the window, rx rate per second, resyncs, and the longest gap (seconds)
  between consecutive rx increases (a stall / conductor-restart indicator).
  Raw serial per port is saved to flash-results\serial-<port>-<stamp>.log and a
  JSON summary to flash-results\watch-<stamp>.json.

.EXAMPLE
  .\tools\watch-badges.ps1 -Seconds 120
  .\tools\watch-badges.ps1 -Ports COM4,COM6 -Seconds 60 -Label conductor-restart
#>
[CmdletBinding()]
param(
  [string[]]$Ports,
  [int]$Seconds = 60,
  [string]$Label = 'watch'
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ResultsDir = Join-Path $ProjectRoot 'flash-results'
New-Item -ItemType Directory -Force $ResultsDir | Out-Null
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')

if (-not $Ports) {
  $Ports = @(Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -match 'VID_(1A86|10C4|303A)' -and $_.FriendlyName -match '\((COM\d+)\)' } |
    ForEach-Object { $Matches[1] } | Sort-Object { [int]($_ -replace 'COM', '') })
}
if ($Ports.Count -eq 0) { Write-Host 'no badges attached'; exit 2 }

$state = @{}
foreach ($p in $Ports) {
  $sp = New-Object System.IO.Ports.SerialPort $p, 115200, 'None', 8, 'One'
  $sp.ReadTimeout = 50; $sp.DtrEnable = $false; $sp.RtsEnable = $false
  $sp.Open(); $sp.DiscardInBuffer()
  $state[$p] = [ordered]@{
    port = $p; sp = $sp; buf = ''; log = New-Object System.Text.StringBuilder
    samples = 0; fpsMin = 9999; fpsMax = 0; fpsSum = 0
    rxFirst = -1; rxLast = -1; relayFirst = -1; relayLast = -1; resyncs = -1; bootAttempts = -1
    role = ''; lastRxIncreaseAt = $null; maxGapSec = 0.0; rxIncreases = 0; lastLine = ''
    firstAt = $null; lastAt = $null
  }
}
Write-Host ("Watching {0} for {1}s (label '{2}', started {3})" -f ($Ports -join ', '), $Seconds, $Label, (Get-Date).ToString('HH:mm:ss')) -ForegroundColor Cyan

$start = Get-Date
$deadline = $start.AddSeconds($Seconds)
$nextTick = $start.AddSeconds(10)
try {
  while ((Get-Date) -lt $deadline) {
    foreach ($p in $Ports) {
      $s = $state[$p]
      try { $chunk = $s.sp.ReadExisting() } catch { $chunk = '' }
      if (-not $chunk) { continue }
      [void]$s.log.Append($chunk)
      $s.buf += $chunk
      while (($i = $s.buf.IndexOf("`n")) -ge 0) {
        $line = $s.buf.Substring(0, $i).TrimEnd("`r"); $s.buf = $s.buf.Substring($i + 1)
        $now = Get-Date
        if ($line -match '\[badge\] (CONDUCTOR|RECEIVER) (\d+) fps .*\| rx (\d+) relay (\d+)') {
          $s.role = $Matches[1]; $fps = [int]$Matches[2]; $rx = [int]$Matches[3]; $relay = [int]$Matches[4]
          $s.samples++; $s.fpsSum += $fps
          if ($fps -lt $s.fpsMin) { $s.fpsMin = $fps }; if ($fps -gt $s.fpsMax) { $s.fpsMax = $fps }
          if ($s.rxFirst -lt 0) { $s.rxFirst = $rx; $s.relayFirst = $relay; $s.firstAt = $now; $s.lastRxIncreaseAt = $now }
          if ($rx -gt $s.rxLast -and $s.rxLast -ge 0) {
            $gap = ($now - $s.lastRxIncreaseAt).TotalSeconds
            if ($gap -gt $s.maxGapSec) { $s.maxGapSec = [math]::Round($gap, 1) }
            $s.lastRxIncreaseAt = $now; $s.rxIncreases++
          }
          $s.rxLast = $rx; $s.relayLast = $relay; $s.lastAt = $now; $s.lastLine = $line
        } elseif ($line -match '\| resyncs (\d+) \| boot attempts (\d+)') {
          $s.resyncs = [int]$Matches[1]; $s.bootAttempts = [int]$Matches[2]
        } elseif ($line -match '\[badge\] boot') {
          Write-Host ("  {0} REBOOTED at {1}: {2}" -f $p, $now.ToString('HH:mm:ss'), $line) -ForegroundColor Yellow
        }
      }
    }
    if ((Get-Date) -ge $nextTick) {
      $nextTick = $nextTick.AddSeconds(10)
      $parts = foreach ($p in $Ports) { $s = $state[$p]; "{0} rx {1} fps {2}" -f $p, $s.rxLast, ($(if ($s.samples) { [int]($s.fpsSum / $s.samples) } else { 0 })) }
      Write-Host ("  {0}  {1}" -f (Get-Date).ToString('HH:mm:ss'), ($parts -join ' | ')) -ForegroundColor DarkGray
    }
    Start-Sleep -Milliseconds 100
  }
} finally {
  foreach ($p in $Ports) { try { $state[$p].sp.Close() } catch {} }
}

$rows = foreach ($p in $Ports) {
  $s = $state[$p]
  $win = if ($s.firstAt -and $s.lastAt) { ($s.lastAt - $s.firstAt).TotalSeconds } else { 0 }
  # A stall that lasted until the end of the window also counts as a gap.
  if ($s.lastRxIncreaseAt) { $tail = ((Get-Date) - $s.lastRxIncreaseAt).TotalSeconds; if ($tail -gt $s.maxGapSec) { $s.maxGapSec = [math]::Round($tail, 1) } }
  $rxDelta = if ($s.rxFirst -ge 0) { $s.rxLast - $s.rxFirst } else { 0 }
  [pscustomobject]@{
    port = $p; role = $s.role; samples = $s.samples
    fps_min = $(if ($s.samples) { $s.fpsMin } else { 0 }); fps_avg = $(if ($s.samples) { [math]::Round($s.fpsSum / $s.samples, 1) } else { 0 }); fps_max = $s.fpsMax
    rx_start = $s.rxFirst; rx_end = $s.rxLast; rx_delta = $rxDelta
    rx_per_s = $(if ($win -gt 0) { [math]::Round($rxDelta / $win, 1) } else { 0 })
    relay_delta = $(if ($s.relayFirst -ge 0) { $s.relayLast - $s.relayFirst } else { 0 })
    resyncs = $s.resyncs; boot_attempts = $s.bootAttempts; max_rx_gap_s = $s.maxGapSec
    window_s = [math]::Round($win, 1)
  }
  [IO.File]::WriteAllText((Join-Path $ResultsDir "serial-$p-$stamp.log"), $s.log.ToString())
}
$rows | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
$summary = [pscustomobject]@{ label = $Label; started = $start.ToString('s'); seconds = $Seconds; badges = $rows }
$summary | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $ResultsDir "watch-$Label-$stamp.json") -Encoding UTF8
Write-Host "raw serial and JSON summary in $ResultsDir (stamp $stamp)"
