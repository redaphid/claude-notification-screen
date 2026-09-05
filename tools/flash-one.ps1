<#
.SYNOPSIS
  Flash ONE Chorus badge on a given COM port from prebuilt images, claim it, verify it.

.DESCRIPTION
  Designed to be run by several workers at once (one per board), e.g. from
  tools/flash-all.ps1 or from parallel agents. It never builds; it flashes the
  four images PlatformIO produced (bootloader, partitions, boot_app0, firmware)
  with the same esptool invocation `pio run -t upload` uses on this board.

  Claiming: the board's USB serial number (from the CH343) is used to create
  flash-results\claims\<serial>.claim atomically. If the claim already exists
  the script exits with code 3 and touches nothing, so two workers cannot flash
  the same board. Pass -NoClaim to skip that.

  Result: flash-results\<serial>.json (also printed as a final RESULT line).
  Exit code 0 = OK (or OK_* warning states), 1 = failed, 3 = already claimed.

.EXAMPLE
  .\tools\flash-one.ps1 -Port COM4
  .\tools\flash-one.ps1 -Port COM5 -Claimant agent-b -BuildDir .pio\build\waveshare_esp32s3_lcd128
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Port,
  [string]$BuildDir,
  [string]$ResultsDir,
  [string]$Claimant = "$env:USERNAME@$env:COMPUTERNAME",
  [switch]$NoClaim,
  [switch]$SkipVerify,
  [int]$VerifySeconds = 15,
  [int]$Baud = 921600
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir)   { $BuildDir   = Join-Path $ProjectRoot '.pio\build\waveshare_esp32s3_lcd128' }
if (-not $ResultsDir) { $ResultsDir = Join-Path $ProjectRoot 'flash-results' }
$ClaimsDir = Join-Path $ResultsDir 'claims'
New-Item -ItemType Directory -Force $ResultsDir, $ClaimsDir | Out-Null

function Say([string]$t, [string]$c = 'Gray') { Write-Host "[$Port] $t" -ForegroundColor $c }

# ---------------------------------------------------------------- tools
$PioHome   = Join-Path $env:USERPROFILE '.platformio'
$Python    = Join-Path $env:APPDATA 'uv\tools\platformio\Scripts\python.exe'
$EsptoolPy = Join-Path $PioHome 'packages\tool-esptoolpy\esptool.py'
$BootApp0  = Join-Path $PioHome 'packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
foreach ($f in @($Python, $EsptoolPy, $BootApp0)) { if (-not (Test-Path $f)) { throw "Missing tool file: $f" } }

$Images = [ordered]@{
  '0x0'     = Join-Path $BuildDir 'bootloader.bin'
  '0x8000'  = Join-Path $BuildDir 'partitions.bin'
  '0xe000'  = $BootApp0
  '0x10000' = Join-Path $BuildDir 'firmware.bin'
}
foreach ($img in $Images.Values) { if (-not (Test-Path $img)) { throw "Missing image (build first with `pio run`): $img" } }

function Invoke-Esptool([string[]]$Arguments) {
  $lines = New-Object System.Collections.Generic.List[string]
  & $Python $EsptoolPy --chip esp32s3 --port $Port --baud $Baud @Arguments 2>&1 | ForEach-Object {
    $l = "$_"; $lines.Add($l)
    if ($l -notmatch '^Writing at') { Say "  $l" 'DarkGray' }
  }
  if ($LASTEXITCODE -ne 0) { throw "esptool exited $LASTEXITCODE ($($Arguments -join ' '))" }
  return ($lines -join "`n")
}

# ---------------------------------------------------------------- identify + claim
$pnp = Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
       Where-Object { $_.FriendlyName -match "\($Port\)" } | Select-Object -First 1
if (-not $pnp) { throw "$Port is not an attached serial device" }
$usbSerial = ($pnp.InstanceId -split '\\')[-1]
Say "$($pnp.FriendlyName)  usb-serial=$usbSerial" 'Cyan'

$claimFile = Join-Path $ClaimsDir "$usbSerial.claim"
if (-not $NoClaim) {
  try {
    $fs = [System.IO.File]::Open($claimFile, [System.IO.FileMode]::CreateNew)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes("claimant=$Claimant`nport=$Port`nat=$((Get-Date).ToString('s'))`n")
    $fs.Write($bytes, 0, $bytes.Length); $fs.Close()
    Say "claimed by $Claimant" 'Green'
  } catch [System.IO.IOException] {
    $owner = (Get-Content $claimFile -ErrorAction SilentlyContinue) -join ' '
    Say "already claimed ($owner); not touching it" 'Yellow'
    exit 3
  }
}

$row = [ordered]@{
  timestamp = (Get-Date).ToString('s'); usb_serial = $usbSerial; port = $Port; claimant = $Claimant
  mac = ''; chip = ''; flash = ''; features = ''; firmware = (Get-Item $Images['0x10000']).FullName
  firmware_bytes = (Get-Item $Images['0x10000']).Length; role = ''; radio = ''; fps = 0; result = ''; note = ''
}

try {
  # ---------------------------------------------------------------- chip info
  Say "reading chip info" 'Cyan'
  $out = Invoke-Esptool @('flash_id')
  if ($out -match '(?im)^\s*MAC:\s*([0-9A-Fa-f:]{17})')          { $row.mac = $Matches[1].ToUpper() }
  if ($out -match '(?im)^\s*Chip (?:type|is):?\s*(.+?)\s*$')     { $row.chip = $Matches[1] }
  if ($out -match '(?im)^\s*Features:\s*(.+?)\s*$')              { $row.features = $Matches[1] }
  if ($out -match '(?im)^\s*(?:Detected f|F)lash size:\s*(\S+)') { $row.flash = $Matches[1] }
  Say "MAC $($row.mac) | $($row.chip) | flash $($row.flash)" 'Green'
  if ($row.flash -ne '16MB') { throw "flash size '$($row.flash)' is not 16MB; refusing to flash" }

  # ---------------------------------------------------------------- write
  Say "writing $($row.firmware_bytes) byte firmware + bootloader/partitions/boot_app0" 'Cyan'
  $args = @('--before', 'default_reset', '--after', 'hard_reset', 'write_flash', '-z',
            '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '16MB')
  foreach ($k in $Images.Keys) { $args += @($k, $Images[$k]) }
  $out = Invoke-Esptool $args
  if ($out -notmatch 'Hash of data verified') { throw "esptool did not report 'Hash of data verified'" }
  Say "flash written and verified" 'Green'

  # ---------------------------------------------------------------- verify boot
  if (-not $SkipVerify) {
    Say "watching serial for $VerifySeconds s" 'Cyan'
    Start-Sleep -Milliseconds 500
    $sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
    $sp.ReadTimeout = 200; $sp.DtrEnable = $false; $sp.RtsEnable = $false
    $sp.Open()
    $log = ''
    try {
      $sp.RtsEnable = $true; Start-Sleep -Milliseconds 100; $sp.RtsEnable = $false
      $sp.DiscardInBuffer()
      $deadline = (Get-Date).AddSeconds($VerifySeconds)
      while ((Get-Date) -lt $deadline) {
        try { $log += $sp.ReadExisting() } catch {}
        if (([regex]::Matches($log, '\[badge\] (?:CONDUCTOR|RECEIVER) (\d+) fps')).Count -ge 3 -and $log -match '\[badge\] role:') { break }
        Start-Sleep -Milliseconds 150
      }
    } finally { $sp.Close() }
    if ($log -match '\[badge\] role: ([^\r\n]+)') { $row.role = $Matches[1].Trim() }
    if ($log -match 'ESP-NOW up, channel \d+, tx -?\d+dBm, mac ([0-9A-Fa-f:]{17})') { $row.radio = 'up'; $row.mac = $Matches[1].ToUpper() }
    elseif ($log -match 'ESP-NOW init FAILED|peer FAILED|no radio|skipping radio|radio off') { $row.radio = 'down' }
    $fpsM = [regex]::Matches($log, '\[badge\] (?:CONDUCTOR|RECEIVER) (\d+) fps')
    foreach ($m in $fpsM) { $v = [int]$m.Groups[1].Value; if ($v -gt $row.fps) { $row.fps = $v } }
    $panel = $log -match '\[badge\] panel up'
    if ($log -match '\[badge\] FATAL: ([^\r\n]+)')      { $row.result = 'FATAL'; $row.note = $Matches[1].Trim() }
    elseif ($row.role -and $fpsM.Count -ge 2 -and $row.fps -gt 0) {
      if ($row.radio -eq 'down')                { $row.result = 'OK_NO_RADIO' }
      elseif ($row.role -notmatch 'RECEIVER')   { $row.result = 'OK_' + ($row.role -replace '[^A-Z]', '') }
      else                                      { $row.result = 'OK' }
    }
    elseif ($panel -or $log -match '\[badge\] boot') { $row.result = 'BOOTED_NO_FPS' }
    else { $row.result = 'FLASHED_NO_OUTPUT'; $row.note = (($log -split "`n") | Select-Object -Last 5) -join ' | ' }
    Say "boot: role=$($row.role) radio=$($row.radio) panel=$panel fps=$($row.fps)" $(if ($row.result -eq 'OK') { 'Green' } else { 'Yellow' })
  } else {
    $row.result = 'FLASHED'
  }
} catch {
  $row.result = 'FAILED'
  $row.note = "$($_.Exception.Message)" -replace '[\r\n]+', ' '
  Say "FAILED: $($row.note)" 'Red'
}

$resultFile = Join-Path $ResultsDir "$usbSerial.json"
([pscustomobject]$row) | ConvertTo-Json | Set-Content -Path $resultFile -Encoding UTF8
Say "RESULT $($row.result) mac=$($row.mac) role=$($row.role) fps=$($row.fps) -> $resultFile" $(if ($row.result -like 'OK*') { 'Green' } else { 'Red' })
if ($row.result -like 'OK*' -or $row.result -eq 'FLASHED') { exit 0 } else { exit 1 }
