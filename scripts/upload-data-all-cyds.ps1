# upload-data-all-cyds.ps1
# Build LittleFS image from ./data once, then open one Windows Terminal tab/window per detected COM port.
# Each tab/window writes the same prebuilt LittleFS image directly with esptool.
# Assumes only CYDs/target serial devices are connected.

param(
    [ValidateSet("Tabs", "Windows")]
    [string]$Layout = "Tabs",
    [int]$WindowOffset = 128
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Split-Path -Parent $scriptDir
$pio = "C:\Users\Stephen\.platformio\penv\Scripts\platformio.exe"
$python = "C:\Users\Stephen\.platformio\penv\Scripts\python.exe"
$envName = "cyd"
$launchDir = Join-Path $rootDir ".pio\multi-cyd"
$fsImage = Join-Path $rootDir ".pio\build\$envName\littlefs.bin"
$partitionCsv = Join-Path $rootDir "no_ota_littlefs.csv"

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

function Get-LittleFsOffset {
    if (!(Test-Path $partitionCsv)) {
        throw "Partition CSV not found: $partitionCsv"
    }
    foreach ($line in Get-Content $partitionCsv) {
        $trimmed = $line.Trim()
        if (!$trimmed -or $trimmed.StartsWith('#')) { continue }
        $parts = $trimmed.Split(',') | ForEach-Object { $_.Trim() }
        if ($parts.Count -ge 5 -and ($parts[0] -eq 'spiffs' -or $parts[2] -eq 'spiffs' -or $parts[2] -eq 'littlefs')) {
            return $parts[3]
        }
    }
    throw "Could not find LittleFS/SPIFFS partition offset in $partitionCsv"
}

if (!(Test-Path $pio)) {
    Write-Host "PlatformIO not found: $pio" -ForegroundColor Red
    exit 1
}
if (!(Test-Path $python)) {
    Write-Host "PlatformIO Python not found: $python" -ForegroundColor Red
    exit 1
}
if (!(Test-Path (Join-Path $rootDir "data"))) {
    Write-Host "Project data directory not found: $(Join-Path $rootDir 'data')" -ForegroundColor Red
    exit 1
}

$ports = @(Get-SerialPorts)
if ($ports.Count -eq 0) {
    Write-Host "No COM ports detected." -ForegroundColor Red
    exit 1
}

Write-Host "Detected COM ports:" -ForegroundColor Green
foreach ($p in $ports) {
    Write-Host "  $($p.DeviceID) - $($p.Name)"
}
Write-Host ""

Write-Host "Building LittleFS image once from ./data..." -ForegroundColor Green
Push-Location $rootDir
try {
    & $pio run --environment $envName --target buildfs
    if ($LASTEXITCODE -ne 0) { throw "PlatformIO buildfs failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

if (!(Test-Path $fsImage)) {
    Write-Host "LittleFS image not found after buildfs: $fsImage" -ForegroundColor Red
    exit 1
}

$fsOffset = Get-LittleFsOffset
Write-Host "LittleFS image: $fsImage" -ForegroundColor Green
Write-Host "LittleFS offset: $fsOffset" -ForegroundColor Green

New-Item -ItemType Directory -Force -Path $launchDir | Out-Null

Write-Host "Launching LittleFS upload $Layout..." -ForegroundColor Green
$tabCommands = @()
$index = 0
foreach ($p in $ports) {
    $port = $p.DeviceID
    $title = "CYD DATA $port"
    $cmdPath = Join-Path $launchDir "upload-data-$port.cmd"
    $cmd = @"
@echo off
cd /d "$rootDir"
echo Uploading prebuilt CYD LittleFS image to $port...
echo Image: $fsImage
echo Offset: $fsOffset
"$python" -m esptool --chip esp32 --port $port --baud 1500000 write_flash $fsOffset "$fsImage"
if errorlevel 1 (
  echo.
  echo LittleFS upload failed on $port.
  pause
  exit /b 1
)
echo.
echo LittleFS upload complete on $port.
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
}

if ($Layout -eq "Tabs") {
    $wtArgs = "-w new " + ($tabCommands -join " ; ")
    Start-Process wt.exe -ArgumentList $wtArgs
    Write-Host "Started Windows Terminal with $($ports.Count) tab(s)." -ForegroundColor Green
} else {
    Write-Host "Started $($ports.Count) Windows Terminal window(s)." -ForegroundColor Green
}
