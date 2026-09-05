<#
.SYNOPSIS
  Enumerate every attached Chorus badge and flash them all, in parallel.

.DESCRIPTION
  1. Builds the badge env once (`pio run -e <env>`), unless -SkipBuild.
  2. Enumerates attached USB-serial boards (WCH CH343/CH340, CP210x, or
     Espressif native USB) and prints them.
  3. Clears flash-results\claims\ and runs tools\flash-one.ps1 for every board
     at once (one pwsh per board, throttled by -MaxParallel), each claiming
     its board by USB serial, flashing the prebuilt images, and verifying the
     boot over serial. Per-board logs land in flash-results\<port>.log.
  4. Aggregates flash-results\*.json into flash-log.csv and prints a summary.

  Needs PowerShell 7+ for the parallel path; on Windows PowerShell 5.1 it
  falls back to flashing boards one after another.

.EXAMPLE
  .\tools\flash-all.ps1                    # build, then flash everything attached
  .\tools\flash-all.ps1 -SkipBuild         # images already built
  .\tools\flash-all.ps1 -Ports COM4,COM6   # only these
  .\tools\flash-all.ps1 -Sequential        # one at a time (easier to watch)
#>
[CmdletBinding()]
param(
  [string]$Env = 'waveshare_esp32s3_lcd128',
  [string[]]$Ports,
  [switch]$SkipBuild,
  [switch]$SkipVerify,
  [switch]$Sequential,
  [int]$MaxParallel = 8,
  [int]$VerifySeconds = 15
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot
$FlashOne   = Join-Path $PSScriptRoot 'flash-one.ps1'
$BuildDir   = Join-Path $ProjectRoot ".pio\build\$Env"
$ResultsDir = Join-Path $ProjectRoot 'flash-results'
$ClaimsDir  = Join-Path $ResultsDir 'claims'
$LogPath    = Join-Path $ProjectRoot 'flash-log.csv'
$Pio = @((Join-Path $env:USERPROFILE '.local\bin\pio.exe'), (Join-Path $env:APPDATA 'uv\tools\platformio\Scripts\pio.exe'), 'pio') |
       ForEach-Object { if (Test-Path $_) { $_ } elseif (Get-Command $_ -ErrorAction SilentlyContinue) { (Get-Command $_).Source } } |
       Select-Object -First 1
if (-not $Pio) { throw 'pio not found' }

function Step([string]$t) { Write-Host "`n==> $t" -ForegroundColor Cyan }

# ---------------------------------------------------------------- build once
if (-not $SkipBuild) {
  Step "Building $Env"
  & $Pio run -e $Env 2>&1 | ForEach-Object { "$_" } | Select-String -Pattern 'error|RAM:|Flash:|SUCCESS|FAILED' | ForEach-Object { Write-Host "    $_" }
  if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
}
foreach ($f in 'bootloader.bin', 'partitions.bin', 'firmware.bin') {
  if (-not (Test-Path (Join-Path $BuildDir $f))) { throw "missing $BuildDir\$f -- build first" }
}
$fw = Get-Item (Join-Path $BuildDir 'firmware.bin')
Write-Host "    firmware.bin $($fw.Length) bytes, built $($fw.LastWriteTime.ToString('s'))"

# ---------------------------------------------------------------- enumerate
Step "Attached boards"
$boards = @(Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object {
  if ($_.InstanceId -match 'VID_(1A86|10C4|303A).*\\([^\\]+)$' -and $_.FriendlyName -match '\((COM\d+)\)') {
    [pscustomobject]@{ Port = $Matches[1]; Serial = ($_.InstanceId -split '\\')[-1]; Name = $_.FriendlyName }
  }
})
if ($Ports) { $boards = @($boards | Where-Object { $Ports -contains $_.Port }) }
$boards = @($boards | Sort-Object { [int]($_.Port -replace 'COM', '') })
if ($boards.Count -eq 0) { Write-Host "    none found"; exit 2 }
$boards | ForEach-Object { Write-Host ("    {0,-6} usb-serial {1,-14} {2}" -f $_.Port, $_.Serial, $_.Name) }

# ---------------------------------------------------------------- flash
New-Item -ItemType Directory -Force $ResultsDir | Out-Null
if (Test-Path $ClaimsDir) { Remove-Item -Recurse -Force $ClaimsDir }
New-Item -ItemType Directory -Force $ClaimsDir | Out-Null
foreach ($b in $boards) { Remove-Item (Join-Path $ResultsDir "$($b.Serial).json") -ErrorAction SilentlyContinue }

$common = @('-BuildDir', $BuildDir, '-ResultsDir', $ResultsDir, '-VerifySeconds', $VerifySeconds, '-Claimant', 'flash-all')
if ($SkipVerify) { $common += '-SkipVerify' }
$parallel = (-not $Sequential) -and ($PSVersionTable.PSVersion.Major -ge 7) -and $boards.Count -gt 1
Step "Flashing $($boards.Count) board(s) $(if ($parallel) { "in parallel (max $MaxParallel)" } else { 'sequentially' })"
$sw = [System.Diagnostics.Stopwatch]::StartNew()

if ($parallel) {
  $pwsh = (Get-Process -Id $PID).Path
  $boards | ForEach-Object -ThrottleLimit $MaxParallel -Parallel {
    $log = Join-Path $using:ResultsDir "$($_.Port).log"
    & $using:pwsh -NoProfile -NonInteractive -File $using:FlashOne -Port $_.Port @using:common *> $log
    $code = $LASTEXITCODE
    $last = (Get-Content $log -Tail 1)
    Write-Host ("    {0,-6} exit {1}  {2}" -f $_.Port, $code, $last)
  }
} else {
  foreach ($b in $boards) {
    Write-Host "`n--- $($b.Port) ---" -ForegroundColor Cyan
    & $FlashOne -Port $b.Port @common
  }
}
$sw.Stop()

# ---------------------------------------------------------------- aggregate
Step "Results ($([int]$sw.Elapsed.TotalSeconds)s total)"
$rows = @()
foreach ($b in $boards) {
  $f = Join-Path $ResultsDir "$($b.Serial).json"
  if (Test-Path $f) { $rows += (Get-Content $f -Raw | ConvertFrom-Json) }
  else { $rows += [pscustomobject]@{ timestamp = (Get-Date).ToString('s'); usb_serial = $b.Serial; port = $b.Port; mac = ''; role = ''; radio = ''; fps = 0; result = 'NO_RESULT'; note = "no result file; see flash-results\$($b.Port).log" } }
}
$rows | Select-Object port, result, mac, role, radio, fps, usb_serial, note | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
$csvRows = $rows | Select-Object timestamp, usb_serial, port, mac, chip, flash, role, radio, fps, result, note, @{n = 'firmware'; e = { $_.firmware } }, @{n = 'env'; e = { $Env } }
if (Test-Path $LogPath) { $csvRows | Export-Csv -Path $LogPath -Append -NoTypeInformation } else { $csvRows | Export-Csv -Path $LogPath -NoTypeInformation }
$ok  = @($rows | Where-Object { $_.result -like 'OK*' }).Count
$bad = $rows.Count - $ok
Write-Host ("{0} OK, {1} not OK, log appended to {2}" -f $ok, $bad, $LogPath) -ForegroundColor $(if ($bad) { 'Yellow' } else { 'Green' })
exit $(if ($bad) { 1 } else { 0 })
