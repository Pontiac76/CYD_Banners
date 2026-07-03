# flash-and-monitor-all-cyds.ps1
# Build firmware once, publish OTA firmware, and optionally upload LittleFS/firmware to detected COM ports.
# With -UploadData, LittleFS is built and serial-uploaded first in spawned terminals while firmware builds.

param(
    [ValidateSet("Tabs", "Windows")]
    [string]$Layout = "Tabs",
    [int]$WindowOffset = 128,
    [switch]$UploadData,
    [switch]$BuildOnly,
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
    Write-Host "  -UploadData" -ForegroundColor Yellow
    Write-Host "  -BuildOnly        build and publish OTA firmware; skip serial firmware flash" -ForegroundColor Yellow
    Write-Host "  -ComPort COM6     aliases: -Port, -Ports, -CommPort" -ForegroundColor Yellow
    exit 1
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Split-Path -Parent $scriptDir
$pio = "C:\Users\Stephen\.platformio\penv\Scripts\platformio.exe"
$envName = "cyd"
$baud = 115200
$launchDir = Join-Path $rootDir ".pio\multi-cyd"
$python = "C:\Users\Stephen\.platformio\penv\Scripts\python.exe"
$buildDir = Join-Path $rootDir ".pio\build\$envName"
$bootloaderImage = Join-Path $buildDir "bootloader.bin"
$partitionsImage = Join-Path $buildDir "partitions.bin"
$firmwareImage = Join-Path $buildDir "firmware.bin"
$fsImage = Join-Path $buildDir "littlefs.bin"
$partitionCsv = Join-Path $rootDir "ota_littlefs.csv"
$firmwarePublishDir = Join-Path $rootDir "server\firmware"
$firmwareVersionHeader = Join-Path $rootDir "src\firmware_version.h"
$firmwareReadyMarker = Join-Path $launchDir "firmware-ready.marker"

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

function Write-FirmwareVersionHeader([string]$version) {
    $content = @"
#pragma once

#define CYD_FIRMWARE_VERSION `"$version`"
"@
    Set-Content -Path $firmwareVersionHeader -Value $content -Encoding ASCII
}

function Publish-FirmwareOtaMetadata([string]$version) {
    New-Item -ItemType Directory -Force -Path $firmwarePublishDir | Out-Null
    $publishedBin = Join-Path $firmwarePublishDir "firmware.bin"
    Copy-Item -Force -Path $firmwareImage -Destination $publishedBin
    $md5 = (Get-FileHash -Algorithm MD5 -Path $publishedBin).Hash.ToLowerInvariant()
    $size = (Get-Item $publishedBin).Length
    $metadata = [ordered]@{
        enabled = "true"
        project = "Banners"
        version = $version
        md5 = $md5
        size = $size
        url = "firmware/firmware.bin"
        published_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    }
    $metadata | ConvertTo-Json | Set-Content -Path (Join-Path $firmwarePublishDir "latest.json") -Encoding UTF8
    Write-Host "OTA firmware published:" -ForegroundColor Green
    Write-Host "  Version: $version"
    Write-Host "  MD5:     $md5"
    Write-Host "  Size:    $size"
}

function Get-LittleFsOffset {
    if (!(Test-Path $partitionCsv)) { throw "Partition CSV not found: $partitionCsv" }
    foreach ($line in Get-Content $partitionCsv) {
        $trimmed = $line.Trim()
        if (!$trimmed -or $trimmed.StartsWith('#')) { continue }
        $parts = $trimmed.Split(',') | ForEach-Object { $_.Trim() }
        if ($parts.Count -ge 5 -and ($parts[0] -eq 'spiffs' -or $parts[2] -eq 'spiffs' -or $parts[2] -eq 'littlefs')) { return $parts[3] }
    }
    throw "Could not find LittleFS/SPIFFS partition offset in $partitionCsv"
}

function Start-TerminalCommands([array]$commands) {
    if ($commands.Count -eq 0) { return }
    if ($Layout -eq "Windows") {
        $index = 0
        foreach ($tabCommand in $commands) {
            $pos = ($index * $WindowOffset).ToString() + "," + ($index * $WindowOffset).ToString()
            Start-Process wt.exe -ArgumentList "-w new --pos $pos $tabCommand"
            $index++
        }
    } else {
        Start-Process wt.exe -ArgumentList ("-w new " + ($commands -join " ; "))
    }
}

function Start-DeviceWorkers([array]$ports, [bool]$uploadData, [bool]$firmwareUpload, [bool]$monitor, [bool]$waitForFirmware) {
    if ($ports.Count -eq 0) { return }
    New-Item -ItemType Directory -Force -Path $launchDir | Out-Null
    $tabCommands = @()
    $uploadDataEnabled = if ($uploadData) { "1" } else { "0" }
    $firmwareUploadEnabled = if ($firmwareUpload) { "1" } else { "0" }
    $monitorEnabled = if ($monitor) { "1" } else { "0" }
    $waitForFirmwareEnabled = if ($waitForFirmware) { "1" } else { "0" }

    foreach ($p in $ports) {
        $port = Normalize-ComPort $p.DeviceID
        $title = "CYD $port"
        $cmdPath = Join-Path $launchDir "flash-monitor-$port.cmd"
        $cmd = @"
@echo off
cd /d "$rootDir"
if "$uploadDataEnabled" == "1" (
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
  echo.
)
if "$waitForFirmwareEnabled" == "1" (
  echo Waiting for firmware build/publish to complete...
  :wait_firmware
  if not exist "$firmwareReadyMarker" (
    timeout /t 2 /nobreak >nul
    goto wait_firmware
  )
)
if "$firmwareUploadEnabled" == "1" (
  echo Uploading prebuilt CYD firmware to $port...
  echo Bootloader: $bootloaderImage
  echo Partitions: $partitionsImage
  echo Firmware:   $firmwareImage
  "$python" -m esptool --chip esp32 --port $port --baud 1500000 write_flash 0x1000 "$bootloaderImage" 0x8000 "$partitionsImage" 0x10000 "$firmwareImage"
  if errorlevel 1 (
    echo.
    echo Firmware upload failed on $port.
    pause
    exit /b 1
  )
  echo.
)
if "$monitorEnabled" == "1" (
  echo Starting serial monitor on $port at $baud baud...
  echo Press Ctrl+] to exit PlatformIO monitor.
  "$pio" device monitor --port $port --baud $baud
) else (
  echo BuildOnly selected or monitor disabled; firmware monitor skipped for $port.
)
pause
"@
        Set-Content -Path $cmdPath -Value $cmd -Encoding ASCII
        $tabCommands += "new-tab --title `"$title`" powershell.exe -NoProfile -NoExit -ExecutionPolicy Bypass -Command `"& '$cmdPath'`""
    }
    Start-TerminalCommands $tabCommands
}

if (!(Test-Path $pio)) { Write-Host "PlatformIO not found: $pio" -ForegroundColor Red; exit 1 }
if (!(Test-Path $python)) { Write-Host "PlatformIO Python not found: $python" -ForegroundColor Red; exit 1 }

$detectedPorts = @(Get-SerialPorts)
$comPortProvided = $PSBoundParameters.ContainsKey("ComPort") -or $PSBoundParameters.ContainsKey("Port") -or $PSBoundParameters.ContainsKey("Ports")
if ($comPortProvided) {
    $wanted = @($ComPort | Where-Object { $_ -and $_.Trim().Length -gt 0 } | ForEach-Object { Normalize-ComPort $_ })
    if ($wanted.Count -eq 0) { Write-Host "-ComPort was provided but no valid port value was supplied; aborting before build/upload." -ForegroundColor Red; exit 1 }
    $detectedIds = @($detectedPorts | ForEach-Object { Normalize-ComPort $_.DeviceID })
    $missing = @($wanted | Where-Object { $detectedIds -notcontains $_ })
    if ($missing.Count -gt 0) {
        Write-Host "Requested COM port(s) not available; aborting before build/upload:" -ForegroundColor Red
        foreach ($m in $missing) { Write-Host "  $m" -ForegroundColor Red }
        Write-Host ""
        Write-Host "Currently detected COM ports:" -ForegroundColor Yellow
        if ($detectedPorts.Count -eq 0) { Write-Host "  <none>" -ForegroundColor Yellow } else { foreach ($p in $detectedPorts) { Write-Host "  $($p.DeviceID) - $($p.Name)" -ForegroundColor Yellow } }
        exit 1
    }
    $ports = @($wanted | ForEach-Object { $id = $_; $detectedPorts | Where-Object { (Normalize-ComPort $_.DeviceID) -eq $id } | Select-Object -First 1 })
} else {
    Write-Host "Hint: target one or more specific ports with -ComPort, e.g.:" -ForegroundColor Yellow
    Write-Host "  .\scripts\flash-and-monitor-all-cyds.ps1 -ComPort COM6" -ForegroundColor Yellow
    Write-Host "  .\scripts\flash-and-monitor-all-cyds.ps1 -ComPort COM3,COM6" -ForegroundColor Yellow
    if ($BuildOnly -and !$UploadData) { Write-Host "No -ComPort specified; BuildOnly will not use detected COM ports unless -UploadData is also set." -ForegroundColor Yellow }
    elseif ($BuildOnly -and $UploadData) { Write-Host "No -ComPort specified; BuildOnly + UploadData will upload LittleFS to all detected COM ports." -ForegroundColor Yellow }
    else { Write-Host "No -ComPort specified; flashing all detected COM ports." -ForegroundColor Yellow }
    Write-Host ""
    $ports = $detectedPorts
}

$noComPortsSelected = ($ports.Count -eq 0)
if ($noComPortsSelected) {
    Write-Host "No COM ports selected/detected; build will publish OTA firmware and skip serial upload." -ForegroundColor Yellow
} else {
    if ($BuildOnly -and $UploadData) { Write-Host "BuildOnly selected; serial firmware flash will be skipped, but LittleFS data will be uploaded." -ForegroundColor Yellow }
    elseif ($BuildOnly) { Write-Host "BuildOnly selected; serial firmware flash and monitor will be skipped." -ForegroundColor Yellow }
    Write-Host "Selected COM ports:" -ForegroundColor Green
    foreach ($p in $ports) {
        $detected = $detectedPorts | Where-Object { $_.DeviceID -eq $p.DeviceID } | Select-Object -First 1
        if ($detected) { Write-Host "  $($detected.DeviceID) - $($detected.Name)" }
        else { Write-Host "  $($p.DeviceID) - not currently reported by Windows serial-port discovery" -ForegroundColor Yellow }
    }
    Write-Host ""
}

New-Item -ItemType Directory -Force -Path $launchDir | Out-Null
if (Test-Path $firmwareReadyMarker) { Remove-Item -Force $firmwareReadyMarker }

$fsOffset = ""
if ($UploadData) {
    Write-Host "Building LittleFS image once from ./data..." -ForegroundColor Green
    Push-Location $rootDir
    try {
        & $pio run --environment $envName --target buildfs
        if ($LASTEXITCODE -ne 0) { throw "PlatformIO buildfs failed with exit code $LASTEXITCODE" }
    } finally { Pop-Location }
    if (!(Test-Path $fsImage)) { Write-Host "LittleFS image not found after buildfs: $fsImage" -ForegroundColor Red; exit 1 }
    $fsOffset = Get-LittleFsOffset
    Write-Host "LittleFS image: $fsImage" -ForegroundColor Green
    Write-Host "LittleFS offset: $fsOffset" -ForegroundColor Green

    if (!$noComPortsSelected) {
        Write-Host "Launching LittleFS upload $Layout before firmware build..." -ForegroundColor Green
        Start-DeviceWorkers $ports $true (!$BuildOnly) (!$BuildOnly) (!$BuildOnly)
    }
}

$firmwareBuildVersion = (Get-Date).ToUniversalTime().ToString("yyyyMMddHHmmss")
Write-FirmwareVersionHeader $firmwareBuildVersion
Write-Host "Firmware build version: $firmwareBuildVersion" -ForegroundColor Green

Write-Host "Building firmware once..." -ForegroundColor Green
Push-Location $rootDir
try {
    & $pio run --environment $envName
    if ($LASTEXITCODE -ne 0) { throw "PlatformIO build failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }

foreach ($requiredImage in @($bootloaderImage, $partitionsImage, $firmwareImage)) {
    if (!(Test-Path $requiredImage)) { Write-Host "Firmware image not found after build: $requiredImage" -ForegroundColor Red; exit 1 }
}
Write-Host "Firmware images:" -ForegroundColor Green
Write-Host "  Bootloader: $bootloaderImage @ 0x1000"
Write-Host "  Partitions: $partitionsImage @ 0x8000"
Write-Host "  Firmware:   $firmwareImage @ 0x10000"
Publish-FirmwareOtaMetadata $firmwareBuildVersion
Set-Content -Path $firmwareReadyMarker -Value "ready" -Encoding ASCII

if ($UploadData) {
    if ($BuildOnly) { Write-Host "BuildOnly complete; OTA firmware published and LittleFS worker(s), if any, were launched." -ForegroundColor Green }
    else { Write-Host "Firmware ready marker written; upload worker(s) may now flash firmware and monitor." -ForegroundColor Green }
    exit 0
}

if ($noComPortsSelected) {
    Write-Host "No COM ports available; OTA metadata has been published, skipping serial upload/monitor." -ForegroundColor Yellow
    exit 0
}

if ($BuildOnly) {
    Write-Host "BuildOnly complete; OTA metadata has been published, skipping serial upload/monitor." -ForegroundColor Green
    exit 0
}

Write-Host "Launching upload+monitor $Layout..." -ForegroundColor Green
Start-DeviceWorkers $ports $false $true $true $false
Write-Host "Started Windows Terminal with $($ports.Count) device worker(s)." -ForegroundColor Green
