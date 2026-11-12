# Retry smoke test for Agent98.
#
# Starts tests\mock_openai.js and runs the CLI twice: once against a server
# that answers HTTP 503 on the first request, once against a server that
# drops the connection before replying.  Both conversations must succeed on
# the automatic retry, and the log must show the backoff.
#
# Expects a98.exe in -Dir (default build\testrun_retry); the script writes
# agent98.ini next to it.

param(
    [string]$Dir = "build\testrun_retry",
    [int]$Port = 8790,
    [int]$TimeoutSec = 60
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $Dir 'a98.exe'
if (-not (Test-Path $exe)) {
    throw "missing $exe"
}
$exe = (Resolve-Path $exe).Path
$Dir = (Resolve-Path $Dir).Path
$mock = Join-Path $root 'tests\mock_openai.js'
$log = Join-Path $Dir 'agent98.log'
$ini = Join-Path $Dir 'agent98.ini'

$iniText = @"
[api]
base=http://127.0.0.1:$Port/v1
key=test
model=mock-model
stream=1
max_tokens=256
temperature=0.7
[agent]
workspace=.\workspace
allow_shell=0
allow_outside=0
max_steps=32
connect_timeout=10
request_timeout=30
[tls]
verify=0
[log]
level=3
"@
Set-Content -Path $ini -Value $iniText -Encoding ascii

Remove-Item $log -Force -ErrorAction SilentlyContinue

$node = Start-Process -FilePath 'node' -ArgumentList @("`"$mock`"", "$Port") -PassThru -WindowStyle Hidden
try {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $ready = $false
    while ((Get-Date) -lt $deadline -and -not $ready) {
        try {
            $c = New-Object Net.Sockets.TcpClient
            $c.Connect('127.0.0.1', $Port)
            $c.Close()
            $ready = $true
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    if (-not $ready) {
        throw "mock server did not start on port $Port"
    }

    Push-Location $Dir
    try {
        $out503 = ("retry 503`n/quit`n" | & $exe 2>&1 | Out-String)
        if ($out503 -notmatch 'The retry succeeded\.') {
            throw "503 retry did not produce an answer:`n$out503"
        }
        if ($out503 -match 'HTTP 503') {
            throw "503 error leaked to the user instead of being retried:`n$out503"
        }
        Write-Host 'PASS: HTTP 503 was retried'

        $outDrop = ("retry drop`n/quit`n" | & $exe 2>&1 | Out-String)
        if ($outDrop -notmatch 'The retry succeeded\.') {
            throw "dropped-connection retry did not produce an answer:`n$outDrop"
        }
        Write-Host 'PASS: dropped connection was retried'
    } finally {
        Pop-Location
    }

    $logText = Get-Content $log -Raw
    if ($logText -notmatch 'retrying in 1 s') {
        throw "retry was not logged in $log"
    }
    Write-Host 'PASS: retry attempt logged with backoff'

    Write-Host 'retry smoke test passed'
}
finally {
    if ($node -and -not $node.HasExited) {
        Stop-Process -Id $node.Id -Force
    }
}
