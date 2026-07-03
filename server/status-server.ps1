# status-server.ps1
# Checks if the CYD Banners Flask server is running.

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$appPath = (Join-Path $scriptDir "app.py")
$appPathBackslash = $appPath.Replace('/', '\')
$appPathSlash = $appPath.Replace('\', '/')

Write-Host "Checking CYD Banners server status..." -ForegroundColor Green
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

if ($processes) {
    foreach ($proc in $processes) {
        $p = Get-Process -Id $proc.ProcessId -ErrorAction SilentlyContinue
        $memoryMb = if ($p) { [math]::Round($p.WorkingSet / 1MB, 1) } else { 0 }
        Write-Host "Running: PID=$($proc.ProcessId) Memory=${memoryMb}MB" -ForegroundColor Green
        Write-Host "  $($proc.CommandLine)"
    }
} else {
    Write-Host "Server is NOT running." -ForegroundColor Yellow
}
