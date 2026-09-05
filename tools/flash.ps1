<#
.SYNOPSIS
  Batch-flash Chorus badges (Waveshare ESP32-S3-LCD-1.28) from Windows.

.DESCRIPTION
  Builds the badge firmware once, then waits for boards to be plugged in. Each
  new board (identified by its USB serial number, so re-plugging the same board
  does not re-flash it) gets:
    1. chip info read with esptool (MAC, chip, flash size; must be 16MB)
    2. optionally a full 16MB stock-flash dump into backups\
         default    : no dump (the stock demo was already dumped once, see NOTES.md)
         -Backup    : dump the first unit only if backups\ is still empty
         -BackupAll : dump every unit (about 3-4 minutes each at 921600 baud)
    3. the firmware flashed with PlatformIO (--upload-port overrides the
       Linux /dev/ttyACM0 hardcoded in platformio.ini)
    4. a reset, then 15s of serial capture checking for the badge's boot
       markers: "[badge] boot", "[badge] panel up", "[badge] role:" and
       steady "[badge] N fps" lines. The boot-loop counter in NVS clears
       after 3s of rendering, so this extra reset does not count against it.
    5. a row appended to flash-log.csv
  Then it tells you to unplug and waits for the next board. Ctrl+C to stop.

  Serial bridges accepted: WCH CH343/CH340 (the 1.28 board), Silicon Labs
  CP210x, and Espressif native USB.

.EXAMPLE
  .\tools\flash.ps1                 # loop: plug badges in one after another
  .\tools\flash.ps1 -Once           # flash whatever is plugged in now, then exit
  .\tools\flash.ps1 -Backup         # also dump the first unit's stock flash
  .\tools\flash.ps1 -Port COM3      # only touch this port
  .\tools\flash.ps1 -Env conductor_fake -SkipVerify   # other envs, no badge markers
#>
[CmdletBinding()]
param(
  [string]$Env = 'waveshare_esp32s3_lcd128',
  [switch]$Once,
  [switch]$Backup,
  [switch]$BackupAll,
  [switch]$SkipBuild,
  [switch]$SkipVerify,
  [int]$VerifySeconds = 15,
  [string]$Port
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot
$BackupDir = Join-Path $ProjectRoot 'backups'
$LogPath   = Join-Path $ProjectRoot 'flash-log.csv'
New-Item -ItemType Directory -Force $BackupDir | Out-Null

$VendorPattern = 'VID_(1A86|10C4|303A)'

# ---------------------------------------------------------------- tools

function Find-Tool([string[]]$Candidates, [string]$What) {
  foreach ($c in $Candidates) {
    if ($c -and (Test-Path $c)) { return (Resolve-Path $c).Path }
    $cmd = Get-Command $c -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
  }
  throw "Could not find $What. Tried: $($Candidates -join ', ')"
}

$UvTools = Join-Path $env:APPDATA 'uv\tools\platformio\Scripts'
$Pio     = Find-Tool @((Join-Path $env:USERPROFILE '.local\bin\pio.exe'), (Join-Path $UvTools 'pio.exe'), 'pio') 'PlatformIO (pio)'
$Esptool = Find-Tool @((Join-Path $UvTools 'esptool.exe'), 'esptool', 'esptool.py') 'esptool'

function Write-Step([string]$Text) { Write-Host "`n==> $Text" -ForegroundColor Cyan }
function Write-Ok([string]$Text)   { Write-Host "    $Text" -ForegroundColor Green }
function Write-Warn2([string]$Text){ Write-Host "    $Text" -ForegroundColor Yellow }
function Write-Bad([string]$Text)  { Write-Host "    $Text" -ForegroundColor Red }

# Run a native tool, echo its output indented, return the captured text. Throws on non-zero exit.
function Invoke-Native([string]$Exe, [string[]]$Arguments) {
  $lines = New-Object System.Collections.Generic.List[string]
  & $Exe @Arguments 2>&1 | ForEach-Object {
    $line = "$_"
    $lines.Add($line)
    Write-Host "    $line" -ForegroundColor DarkGray
  }
  if ($LASTEXITCODE -ne 0) { throw "$([IO.Path]::GetFileName($Exe)) exited with code $LASTEXITCODE ($($Arguments -join ' '))" }
  return ($lines -join "`n")
}

# ---------------------------------------------------------------- boards

function Get-Boards {
  Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.InstanceId -match "$VendorPattern.*\\([^\\]+)$" -and $_.FriendlyName -match '\((COM\d+)\)') {
      [pscustomobject]@{
        Port   = $Matches[1]
        Serial = ($_.InstanceId -split '\\')[-1]
        Name   = $_.FriendlyName
      }
    }
  }
}

function Get-ChipInfo([string]$ComPort) {
  $out = Invoke-Native $Esptool @('--chip', 'esp32s3', '--port', $ComPort, '--baud', '921600', 'flash-id')
  $info = [ordered]@{ Mac = ''; Chip = ''; Features = ''; FlashSize = '' }
  if ($out -match '(?im)^\s*MAC:\s*([0-9A-Fa-f:]{17})')            { $info.Mac = $Matches[1].ToUpper() }
  if ($out -match '(?im)^\s*Chip (?:type|is):?\s*(.+?)\s*$')       { $info.Chip = $Matches[1] }
  if ($out -match '(?im)^\s*Features:\s*(.+?)\s*$')                { $info.Features = $Matches[1] }
  if ($out -match '(?im)^\s*(?:Detected f|F)lash size:\s*(\S+)')   { $info.FlashSize = $Matches[1] }
  return [pscustomobject]$info
}

function Invoke-SerialCheck([string]$ComPort, [int]$Seconds) {
  # Reset the board the way esptool does (EN low via RTS, IO0 released via DTR),
  # then watch the badge's boot markers and fps lines.
  $r = [ordered]@{ Boot = $false; Panel = $false; Role = ''; Mac = ''; FpsSamples = 0; MaxFps = 0; Radio = 'unknown'; Fatal = ''; Log = '' }
  $sp = New-Object System.IO.Ports.SerialPort $ComPort, 115200, 'None', 8, 'One'
  $sp.ReadTimeout = 200
  $sp.DtrEnable = $false
  $sp.RtsEnable = $false
  $sp.Open()
  try {
    $sp.RtsEnable = $true
    Start-Sleep -Milliseconds 100
    $sp.RtsEnable = $false
    $sp.DiscardInBuffer()

    $log = ''
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
      try { $log += $sp.ReadExisting() } catch {}
      # Stop early once we have a role and a couple of healthy fps samples.
      $fpsMatches = [regex]::Matches($log, '\[badge\] (?:CONDUCTOR|RECEIVER) (\d+) fps')
      if ($fpsMatches.Count -ge 3 -and $log -match '\[badge\] role:') { break }
      Start-Sleep -Milliseconds 150
    }
    $r.Log = $log
    $r.Boot  = $log -match '\[badge\] boot'
    $r.Panel = $log -match '\[badge\] panel up'
    if ($log -match '\[badge\] role: ([^\r\n]+)')                     { $r.Role = $Matches[1].Trim() }
    if ($log -match 'ESP-NOW up, channel \d+, tx -?\d+dBm, mac ([0-9A-Fa-f:]{17})') { $r.Mac = $Matches[1].ToUpper(); $r.Radio = 'up' }
    elseif ($log -match 'ESP-NOW init FAILED|add broadcast peer FAILED|no radio|skipping radio|radio off') { $r.Radio = 'down' }
    if ($log -match '\[badge\] FATAL: ([^\r\n]+)')                    { $r.Fatal = $Matches[1].Trim() }
    $fpsMatches = [regex]::Matches($log, '\[badge\] (?:CONDUCTOR|RECEIVER) (\d+) fps')
    $r.FpsSamples = $fpsMatches.Count
    foreach ($m in $fpsMatches) { $v = [int]$m.Groups[1].Value; if ($v -gt $r.MaxFps) { $r.MaxFps = $v } }
  } finally {
    $sp.Close()
  }
  return [pscustomobject]$r
}

function Write-LogRow([hashtable]$Row) {
  $obj = [pscustomobject]$Row
  if (Test-Path $LogPath) { $obj | Export-Csv -Path $LogPath -Append -Force -NoTypeInformation }
  else                    { $obj | Export-Csv -Path $LogPath -NoTypeInformation }
}

function Invoke-Board($Board) {
  $row = [ordered]@{
    timestamp  = (Get-Date).ToString('s')
    usb_serial = $Board.Serial
    port       = $Board.Port
    mac        = ''
    chip       = ''
    flash      = ''
    features   = ''
    backup     = ''
    env        = $Env
    role       = ''
    radio      = ''
    fps        = ''
    result     = ''
    note       = ''
  }

  Write-Host ("`n" + ('=' * 72)) -ForegroundColor Cyan
  Write-Host "BOARD  $($Board.Name)   usb-serial=$($Board.Serial)" -ForegroundColor Cyan
  Write-Host ('=' * 72) -ForegroundColor Cyan

  if (Test-Path $LogPath) {
    $prev = Import-Csv $LogPath | Where-Object { $_.usb_serial -eq $Board.Serial } | Select-Object -Last 1
    if ($prev) { Write-Warn2 "Seen before: $($prev.timestamp) result=$($prev.result) mac=$($prev.mac)" }
  }

  try {
    Write-Step "Chip info ($($Board.Port))"
    $chip = Get-ChipInfo $Board.Port
    $row.mac = $chip.Mac; $row.chip = $chip.Chip; $row.flash = $chip.FlashSize; $row.features = $chip.Features
    Write-Ok "MAC $($chip.Mac)  |  $($chip.Chip)  |  flash $($chip.FlashSize)  |  $($chip.Features)"
    if ($chip.FlashSize -ne '16MB') {
      throw "Flash size is '$($chip.FlashSize)', expected 16MB for the ESP32-S3-LCD-1.28. Not flashing."
    }

    $existingBackups = @(Get-ChildItem $BackupDir -Filter '*.bin' -ErrorAction SilentlyContinue)
    $doBackup = $BackupAll -or ($Backup -and $existingBackups.Count -eq 0)
    if ($doBackup) {
      $macTag = ($chip.Mac -replace ':', '')
      $backupFile = Join-Path $BackupDir "stock_$($Board.Serial)_$macTag`_16MB.bin"
      if (Test-Path $backupFile) {
        Write-Warn2 "Backup already exists, keeping it: $backupFile"
      } else {
        Write-Step "Backing up full 16MB flash (3-4 min) -> $backupFile"
        Invoke-Native $Esptool @('--chip', 'esp32s3', '--port', $Board.Port, '--baud', '921600', 'read-flash', '0', '0x1000000', $backupFile) | Out-Null
        $size = (Get-Item $backupFile).Length
        if ($size -ne 16MB) { throw "Backup is $size bytes, expected 16777216" }
        Write-Ok "Backup complete ($size bytes)"
      }
      $row.backup = $backupFile
    } else {
      $row.backup = 'skipped'
    }

    Write-Step "Flashing env '$Env' to $($Board.Port)"
    Invoke-Native $Pio @('run', '-e', $Env, '-t', 'upload', '--upload-port', $Board.Port) | Out-Null
    Write-Ok "Upload done"

    if (-not $SkipVerify) {
      Write-Step "Verifying over serial ($VerifySeconds s max)"
      Start-Sleep -Milliseconds 500
      $chk = Invoke-SerialCheck $Board.Port $VerifySeconds
      $row.role = $chk.Role; $row.radio = $chk.Radio; $row.fps = $chk.MaxFps
      if ($chk.Mac) { $row.mac = $chk.Mac }
      if ($chk.Fatal) {
        Write-Bad "FATAL from badge: $($chk.Fatal)"
        $row.result = 'FATAL'
      } elseif ($chk.Role -and $chk.FpsSamples -ge 2 -and $chk.MaxFps -gt 0) {
        Write-Ok "Booted: role=$($chk.Role)  radio=$($chk.Radio)  panel=$($chk.Panel)  fps=$($chk.MaxFps) ($($chk.FpsSamples) samples)"
        if ($chk.Radio -eq 'down') { Write-Warn2 "Radio did not come up on this boot (badge renders locally). Check power / cable."; $row.result = 'OK_NO_RADIO' }
        elseif ($chk.Role -notmatch 'RECEIVER') { Write-Warn2 "Role is '$($chk.Role)' -- was BOOT held during reset?"; $row.result = 'OK_' + ($chk.Role -replace '[^A-Z]', '') }
        else { $row.result = 'OK' }
      } elseif ($chk.Boot -or $chk.Panel) {
        Write-Warn2 "Booted (boot=$($chk.Boot) panel=$($chk.Panel) role='$($chk.Role)') but no steady fps lines within $VerifySeconds s"
        $row.result = 'BOOTED_NO_FPS'
      } else {
        Write-Bad "No badge output within $VerifySeconds s after reset. Last serial lines:"
        ($chk.Log -split "`n" | Select-Object -Last 15) | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkGray }
        $row.result = 'FLASHED_NO_OUTPUT'
      }
    } else {
      $row.result = 'FLASHED'
    }
  } catch {
    $row.result = 'FAILED'
    $row.note = "$($_.Exception.Message)" -replace '[\r\n]+', ' '
    Write-Bad "FAILED: $($_.Exception.Message)"
  }

  Write-LogRow $row
  $color = if ($row.result -eq 'OK') { 'Green' } elseif ($row.result -match 'FAILED|FATAL|NO_OUTPUT') { 'Red' } else { 'Yellow' }
  Write-Host "`nRESULT  $($row.result)   mac $($row.mac)   $($row.role)   ($($Board.Port), usb-serial $($Board.Serial))" -ForegroundColor $color
  return $row
}

# ---------------------------------------------------------------- main

Write-Host "PlatformIO : $Pio"
Write-Host "esptool    : $Esptool"
Write-Host "env        : $Env"
Write-Host "log        : $LogPath"
Write-Host "backups    : $(if ($BackupAll) { 'every unit' } elseif ($Backup) { 'first unit only' } else { 'off (-Backup / -BackupAll to enable)' })"

if (-not $SkipBuild) {
  Write-Step "Building env '$Env' once up front"
  Invoke-Native $Pio @('run', '-e', $Env) | Out-Null
  Write-Ok "Build OK"
}

$done = @{}
$results = New-Object System.Collections.Generic.List[object]
Write-Host "`nWaiting for boards (Ctrl+C to stop)..." -ForegroundColor Cyan
try {
  while ($true) {
    $boards = @(Get-Boards | Where-Object { -not $done.ContainsKey($_.Serial) })
    if ($Port) { $boards = @($boards | Where-Object { $_.Port -eq $Port }) }
    if ($boards.Count -eq 0) {
      if ($Once) { Write-Warn2 "No new board attached."; break }
      Start-Sleep -Seconds 1
      continue
    }
    foreach ($b in $boards) {
      Start-Sleep -Milliseconds 1500   # let enumeration settle after hot-plug
      $results.Add((Invoke-Board $b))
      $done[$b.Serial] = $true
    }
    if ($Once) { break }
    Write-Host "`n>>> Unplug that board and plug in the next one. (Ctrl+C to stop)`n" -ForegroundColor Magenta
  }
} finally {
  if ($results.Count -gt 0) {
    Write-Host "`nSession summary:" -ForegroundColor Cyan
    $results | Select-Object result, mac, role, radio, fps, port, usb_serial | Format-Table -AutoSize | Out-String | Write-Host
  }
}
