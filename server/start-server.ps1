# start-server.ps1
# Starts the CYD Banners Flask server.

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Split-Path -Parent $scriptDir
$pythonExe = "python"
$appPath = (Join-Path $scriptDir "app.py")
$appPathBackslash = $appPath.Replace('/', '\')
$appPathSlash = $appPath.Replace('\', '/')

$existing = Get-CimInstance Win32_Process | Where-Object {
    $_.Name -match '^python(\.exe)?$' -and
    $_.CommandLine -and (
        $_.CommandLine.Contains($appPathBackslash) -or
        $_.CommandLine.Contains($appPathSlash) -or
        $_.CommandLine.Contains('server/app.py') -or
        $_.CommandLine.Contains('server\app.py')
    )
}

if ($existing) {
    Write-Host "CYD Banners server is already running:" -ForegroundColor Yellow
    foreach ($proc in $existing) {
        Write-Host "  PID $($proc.ProcessId): $($proc.CommandLine)" -ForegroundColor Yellow
    }
    exit 1
}

Write-Host "Starting CYD Banners server..." -ForegroundColor Green
Write-Host "Working directory: $rootDir"
Write-Host "App: $appPath"
Write-Host ""

Start-Process $pythonExe -ArgumentList @($appPath) -WorkingDirectory $rootDir -WindowStyle Minimized
Write-Host "Server started in minimized window." -ForegroundColor Green
