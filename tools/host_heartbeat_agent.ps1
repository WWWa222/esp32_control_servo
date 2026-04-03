param(
    [Parameter(Mandatory = $true)]
    [string]$Esp32BaseUrl,

    [string]$AuthToken = "",

    [string]$Source = "windows-agent",

    [int]$IntervalSeconds = 5,

    [int]$RequestTimeoutSeconds = 5,

    [switch]$Once
)

$ErrorActionPreference = "Stop"

function Get-SystemSnapshot {
    $os = Get-CimInstance Win32_OperatingSystem
    $bootValue = $os.LastBootUpTime
    if ($bootValue -is [DateTime]) {
        $bootTime = $bootValue
    }
    else {
        $bootTime = [Management.ManagementDateTimeConverter]::ToDateTime([string]$bootValue)
    }

    $now = Get-Date
    $uptimeSeconds = [int]($now - $bootTime).TotalSeconds
    $cpuPercent = [math]::Round((Get-Counter '\Processor(_Total)\% Processor Time').CounterSamples[0].CookedValue, 1)
    $memoryPercent = [math]::Round((($os.TotalVisibleMemorySize - $os.FreePhysicalMemory) / $os.TotalVisibleMemorySize) * 100, 1)

    return [ordered]@{
        schema       = "rpc.host.v1"
        timestamp    = $now.ToUniversalTime().ToString("o")
        platform     = "windows"
        host         = $env:COMPUTERNAME
        user         = $env:USERNAME
        cpu_pct      = $cpuPercent
        memory_pct   = $memoryPercent
        uptime_s     = $uptimeSeconds
        powershell   = $PSVersionTable.PSVersion.ToString()
    }
}

function Encode-FormData([hashtable]$Values) {
    $pairs = foreach ($entry in $Values.GetEnumerator()) {
        "{0}={1}" -f [Uri]::EscapeDataString([string]$entry.Key), [Uri]::EscapeDataString([string]$entry.Value)
    }
    return ($pairs -join "&")
}

function Send-HeartbeatPayload {
    $payloadObject = Get-SystemSnapshot
    $payloadJson = $payloadObject | ConvertTo-Json -Compress -Depth 4

    $body = @{
        source  = $Source
        payload = $payloadJson
    }

    $headers = @{}
    if ($AuthToken) {
        $headers["X-Auth-Token"] = $AuthToken
    }

    Invoke-RestMethod `
        -Method Post `
        -Uri "$Esp32BaseUrl/api/report" `
        -TimeoutSec $RequestTimeoutSeconds `
        -ContentType "application/x-www-form-urlencoded" `
        -Headers $headers `
        -Body (Encode-FormData $body) | Out-Null

    Write-Host ("[{0}] payload sent source={1} host={2} cpu={3}% mem={4}%" -f `
        (Get-Date -Format "HH:mm:ss"), `
        $Source, `
        $payloadObject.host, `
        $payloadObject.cpu_pct, `
        $payloadObject.memory_pct)
}

Write-Host "Starting structured report agent -> $Esp32BaseUrl"
Write-Host "Source: $Source"
Write-Host "Press Ctrl+C to stop."

do {
    try {
        Send-HeartbeatPayload
    }
    catch {
        Write-Warning $_
    }

    if (-not $Once) {
        Start-Sleep -Seconds $IntervalSeconds
    }
} while (-not $Once)
