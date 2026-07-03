# list-com-ports.ps1
# Lists COM ports Windows currently exposes.

$ports = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
    Where-Object { $_.DeviceID -match '^COM\d+$' } |
    Sort-Object { [int]($_.DeviceID -replace '^COM','') }

if (!$ports) {
    $ports = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\((COM\d+)\)' } |
        ForEach-Object {
            $com = [regex]::Match($_.Name, '\((COM\d+)\)').Groups[1].Value
            [pscustomobject]@{ DeviceID = $com; Name = $_.Name; Manufacturer = $_.Manufacturer }
        } |
        Sort-Object { [int]($_.DeviceID -replace '^COM','') }
}

if (!$ports) {
    Write-Host "No COM ports detected." -ForegroundColor Yellow
    exit 0
}

$ports | Format-Table DeviceID, Name, Manufacturer -AutoSize
