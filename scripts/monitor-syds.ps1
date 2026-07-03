# monitor-syds.ps1
# Open PlatformIO serial monitors for detected COM ports.
# Does not build, upload firmware, or upload data.
# Assumes only CYDs/target serial devices are connected unless -ComPort is specified.

param(
    [ValidateSet("Tabs", "Windows")]
    [string]$Layout = "Tabs",
    [int]$WindowOffset = 128,
    [int]$Baud = 115200,
    [Alias("Port", "Ports", "CommPort")]
    [string[]]$ComPort,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$UnknownArgs
)

$ErrorActionPreference = "Stop"

if ($UnknownArgs -and $UnknownArgs.Count -gt 0) {
    Write-Host "I don't know what this parameter/value is, dingus:" -ForegroundColor Red
    foreach ($arg in $UnknownArgs) { Write-Host "  $arg" -ForegroundColor Red }
    Write-Host ""
    Write-Host "Known parameters:" -ForegroundColor Yellow
    Write-Host "  -Layout Tabs|Windows" -ForegroundColor Yellow
    Write-Host "  -WindowOffset 128" -ForegroundColor Yellow
    Write-Host "  -Baud 115200" -ForegroundColor Yellow
    Write-Host "  -ComPort COM6     aliases: -Port, -Ports, -CommPort" -ForegroundColor Yellow
    exit 1
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Split-Path -Parent $scriptDir
$pio = "C:\Users\Stephen\.platformio\penv\Scripts\platformio.exe"
$launchDir = Join-Path $rootDir ".pio\multi-cyd"

function Normalize-ComPort([string]$port) {
    $p = $port.Trim().ToUpper()
    if ($p -match '^\d+$') { return "COM$p" }
    return $p
}

function Get-SerialPorts {
    $ports = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
        Where-Object { $_.DeviceID -match '^COM\d+$' } |
        Sort-Object { [int]($_.DeviceID -replace '^COM','') }

    if (!$ports) {
        $ports = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '\((COM\d+)\)' } |
            ForEach-Object {
                $com = [regex]::Match($_.Name, '\((COM\d+)\)').Groups[1].Value
                [pscustomobject]@{ DeviceID = $com; Name = $_.Name }
            } |
            Sort-Object { [int]($_.DeviceID -replace '^COM','') }
    }
    return $ports
}

function Get-MonitorProcesses([string]$port) {
    $escapedPort = [regex]::Escape($port)
    return @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $cmd = $_.CommandLine
            if (!$cmd) { return $false }
            return (
                ($cmd -match '(?i)platformio(\.exe)?"?\s+device\s+monitor' -and $cmd -match "(?i)(--port|--serial-port)\s+$escapedPort(\s|$)") -or
                ($cmd -match '(?i)device\s+monitor' -and $cmd -match "(?i)(--port|--serial-port)\s+$escapedPort(\s|$)") -or
                ($cmd -match "(?i)monitor-$escapedPort\.cmd")
            )
        })
}

if (!(Test-Path $pio)) {
    Write-Host "PlatformIO not found: $pio" -ForegroundColor Red
    exit 1
}

$detectedPorts = @(Get-SerialPorts)
if ($ComPort -and $ComPort.Count -gt 0) {
    $wanted = @($ComPort | ForEach-Object { Normalize-ComPort $_ })
    $ports = @($wanted | ForEach-Object { [pscustomobject]@{ DeviceID = $_; Name = "specified" } })
} else {
    Write-Host "Hint: monitor one or more specific ports with -ComPort, e.g.:" -ForegroundColor Yellow
    Write-Host "  .\scripts\monitor-syds.ps1 -ComPort COM6" -ForegroundColor Yellow
    Write-Host "  .\scripts\monitor-syds.ps1 -ComPort COM3,COM6" -ForegroundColor Yellow
    Write-Host "No -ComPort specified; monitoring all detected COM ports." -ForegroundColor Yellow
    Write-Host ""
    $ports = $detectedPorts
}

if ($ports.Count -eq 0) {
    Write-Host "No COM ports selected/detected." -ForegroundColor Red
    exit 1
}

Write-Host "Selected COM ports:" -ForegroundColor Green
foreach ($p in $ports) {
    $detected = $detectedPorts | Where-Object { $_.DeviceID -eq $p.DeviceID } | Select-Object -First 1
    if ($detected) {
        Write-Host "  $($detected.DeviceID) - $($detected.Name)"
    } else {
        Write-Host "  $($p.DeviceID) - not currently reported by Windows serial-port discovery" -ForegroundColor Yellow
    }
}
Write-Host ""

New-Item -ItemType Directory -Force -Path $launchDir | Out-Null

Write-Host "Launching serial monitors $Layout..." -ForegroundColor Green
$tabCommands = @()
$index = 0
$launchCount = 0
foreach ($p in $ports) {
    $port = Normalize-ComPort $p.DeviceID
    $runningMonitors = @(Get-MonitorProcesses $port)
    if ($runningMonitors.Count -gt 0) {
        $pidText = ($runningMonitors | ForEach-Object { "$($_.ProcessId) [$($_.Name)]" }) -join ", "
        Write-Host "Skipping $port; monitor already appears to be running. PID(s): $pidText" -ForegroundColor Yellow
        continue
    }

    $title = "CYD MON $port"
    $cmdPath = Join-Path $launchDir "monitor-$port.cmd"
    $cmd = @"
@echo off
cd /d "$rootDir"
echo Starting serial monitor on $port at $Baud baud...
echo Press Ctrl+] to exit PlatformIO monitor.
"$pio" device monitor --port $port --baud $Baud
"@
    Set-Content -Path $cmdPath -Value $cmd -Encoding ASCII
    $tabCommand = "new-tab --title `"$title`" cmd.exe /k `"$cmdPath`""
    if ($Layout -eq "Windows") {
        $pos = ($index * $WindowOffset).ToString() + "," + ($index * $WindowOffset).ToString()
        $wtArgs = "-w new --pos $pos $tabCommand"
        Start-Process wt.exe -ArgumentList $wtArgs
    } else {
        $tabCommands += $tabCommand
    }
    $index++
    $launchCount++
}

if ($launchCount -eq 0) {
    Write-Host "No new monitors launched." -ForegroundColor Yellow
    exit 0
}

if ($Layout -eq "Tabs") {
    $wtArgs = "-w new " + ($tabCommands -join " ; ")
    Start-Process wt.exe -ArgumentList $wtArgs
    Write-Host "Started Windows Terminal with $launchCount new monitor tab(s)." -ForegroundColor Green
} else {
    Write-Host "Started $launchCount new Windows Terminal monitor window(s)." -ForegroundColor Green
}
