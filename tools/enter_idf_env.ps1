$idfToolsPath = "C:\Espressif\v6.0"
$idfPath = Join-Path $idfToolsPath "esp-idf"
$scoopShims = "C:\Users\Www\scoop\shims"

if (-not (Test-Path $idfPath)) {
    Write-Error "ESP-IDF not found at $idfPath"
    return
}

if (-not ($env:Path -split ';' | Where-Object { $_ -eq $scoopShims })) {
    $env:Path = "$scoopShims;$env:Path"
}

$env:IDF_TOOLS_PATH = $idfToolsPath
. (Join-Path $idfPath "export.ps1")

Write-Host ""
Write-Host "ESP-IDF environment ready."
Write-Host "IDF_TOOLS_PATH=$env:IDF_TOOLS_PATH"
Write-Host ""
