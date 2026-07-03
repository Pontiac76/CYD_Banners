# stop-server.ps1
# Stops CYD Banners Flask server processes only.

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$appPath = (Join-Path $scriptDir "app.py")
$appPathBackslash = $appPath.Replace('/', '\')
$appPathSlash = $appPath.Replace('\', '/')

Write-Host "Stopping CYD Banners server..." -ForegroundColor Green
Write-Host "Target app: $appPath"

$processes = Get-CimInstance Win32_Process | Where-Object {
    $_.Name -match '^python(\.exe)?$' -and
    $_.CommandLine -and (
        $_.CommandLine.Contains($appPathBackslash) -or
        $_.CommandLine.Contains($appPathSlash) -or
        $_.CommandLine.Contains('server/app.py') -or
        $_.CommandLine.Contains('server\app.py')
    )
}

if (!$processes) {
    Write-Host "No CYD Banners server process found." -ForegroundColor Yellow
    exit 0
}

foreach ($proc in $processes) {
    Write-Host "Stopping PID $($proc.ProcessId): $($proc.CommandLine)" -ForegroundColor Yellow
    Stop-Process -Id $proc.ProcessId -Force
}

Start-Sleep -Seconds 1
$remaining = Get-CimInstance Win32_Process | Where-Object {
    $_.Name -match '^python(\.exe)?$' -and
    $_.CommandLine -and (
        $_.CommandLine.Contains($appPathBackslash) -or
        $_.CommandLine.Contains($appPathSlash) -or
        $_.CommandLine.Contains('server/app.py') -or
        $_.CommandLine.Contains('server\app.py')
    )
}

if ($remaining) {
    Write-Host "Warning: one or more CYD Banners server processes remain:" -ForegroundColor Red
    foreach ($proc in $remaining) {
        Write-Host "  PID $($proc.ProcessId): $($proc.CommandLine)" -ForegroundColor Red
    }
    exit 1
}

Write-Host "Done." -ForegroundColor Green
