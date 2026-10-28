# SPDX-License-Identifier: EUPL-1.2
# Copyright (c) 2026 E-Boats Systems (https://www.eboatsystems.com)

# test_enttec.ps1 - tests the console's ENTTEC DMX USB Pro bridge, without QLC+
#
# Usage (PowerShell, console in USB mode: the OLED shows "MIDI-USB"):
#   powershell -ExecutionPolicy Bypass -File .\test_enttec.ps1 -Port COM7
# The port is in Device Manager > Ports (COM & LPT) > "USB Serial Device (COMx)".
# Close any program that uses that port first.

param(
  [Parameter(Mandatory = $true)][string]$Port,
  [int]$Seconds = 5
)

function Hex([byte[]]$b) {
  if ($b.Length -eq 0) { return '(no reply)' }
  return (($b | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
}

function Send-Packet($sp, [byte]$label, [byte[]]$data) {
  $list = New-Object 'System.Collections.Generic.List[byte]'
  $list.Add(0x7E)
  $list.Add($label)
  $list.Add([byte]($data.Length -band 0xFF))
  $list.Add([byte]($data.Length -shr 8))
  foreach ($d in $data) { $list.Add($d) }
  $list.Add(0xE7)
  $arr = $list.ToArray()
  $sp.Write($arr, 0, $arr.Length)
}

function Read-Reply($sp) {
  Start-Sleep -Milliseconds 500
  $n = $sp.BytesToRead
  $buf = New-Object byte[] $n
  if ($n -gt 0) { [void]$sp.Read($buf, 0, $n) }
  return , $buf
}

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.DtrEnable = $true        # the bridge only replies if the PC has the port open (DTR line)
$sp.WriteTimeout = 2000
try { $sp.Open() } catch { Write-Host "Cannot open $Port : $_"; exit 1 }
Start-Sleep -Milliseconds 300

Write-Host "1) Serial number (label 10)"
Send-Packet $sp 10 ([byte[]]@())
Write-Host ("   reply:    " + (Hex (Read-Reply $sp)))
Write-Host "   expected: 7E 0A 04 00 xx xx xx xx E7"

Write-Host "2) Parameters (label 3)"
Send-Packet $sp 3 ([byte[]]@(0, 0))
Write-Host ("   reply:    " + (Hex (Read-Reply $sp)))
Write-Host "   expected: 7E 03 05 00 2C 01 0A 02 28 E7"

Write-Host "3) Sending DMX frames (label 6) for $Seconds s: channels 1-8 at full"
$frame = New-Object byte[] 513      # start code 0 + 512 slots
for ($i = 1; $i -le 8; $i++) { $frame[$i] = 255 }
$end = (Get-Date).AddSeconds($Seconds)
$count = 0
while ((Get-Date) -lt $end) {
  Send-Packet $sp 6 $frame
  $count++
  Start-Sleep -Milliseconds 30
}
Write-Host "   sent $count frames. Check WLED's Info page: Console-DMX > PC>DMX should be about $count."

$rx = Read-Reply $sp
if ($rx.Length -gt 0) {
  Write-Host ("4) " + $rx.Length + " bytes arrived from the DMX input; first bytes: " + (Hex ($rx | Select-Object -First 12)))
} else {
  Write-Host "4) No frames from the DMX input (normal if no DMX controller is connected to GPIO18)."
}

$sp.Close()
