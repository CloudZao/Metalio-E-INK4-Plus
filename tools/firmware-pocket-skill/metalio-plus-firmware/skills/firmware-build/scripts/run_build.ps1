param(
    [Parameter(Mandatory=$true)][string]$EnvScript,
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$LogDirectory,
    [ValidateSet('Build','Reconfigure','SetTarget')][string]$Action = 'Build',
    [switch]$DisableCcache, [switch]$CMakeDirect, [string]$ShortToolsRoot,
    [ValidateRange(10,14400)][int]$TimeoutSeconds = 3600,
    [ValidateRange(1,60)][int]$PollSeconds = 10
)
$ErrorActionPreference = 'Stop'
$moduleRoot = Split-Path $PSScriptRoot -Parent
$skillRoot = Split-Path (Split-Path $moduleRoot -Parent) -Parent
. (Join-Path $skillRoot 'skills\toolchain-ops\scripts\Material-Common.ps1')
$Project = [IO.Path]::GetFullPath($Project).TrimEnd('\')
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\')
$LogDirectory = [IO.Path]::GetFullPath($LogDirectory).TrimEnd('\')
foreach ($path in @($Project,$BuildDirectory,$LogDirectory)) {
    if (Test-ContainsPath $skillRoot $path) { throw 'Project, build and logs must be outside Skill.' }
}
if ($Project -match '[^\x21-\x7e]' -or $BuildDirectory -match '[^\x21-\x7e]') { throw 'Project and build paths must be short ASCII paths without spaces.' }
if (-not (Test-Path -LiteralPath (Join-Path $Project 'CMakeLists.txt'))) { throw 'Project has no CMakeLists.txt.' }
. ([IO.Path]::GetFullPath($EnvScript))
# Native stderr is diagnostic data, not a PowerShell terminating-error policy.
$ErrorActionPreference = 'Continue'
if ($env:IDF_TARGET -ne 'esp32s31') { throw 'Activated environment must target esp32s31.' }
$env:PYTHONUNBUFFERED = '1'; $env:PYTHONUTF8 = '1'; $env:PYTHONIOENCODING = 'utf-8'
if ($DisableCcache) { $env:IDF_CCACHE_ENABLE = '0'; $env:CCACHE_DISABLE = '1' }
if ($ShortToolsRoot) { & (Join-Path $skillRoot 'skills\toolchain-ops\scripts\Use-ShortToolsPath.ps1') -Alias $ShortToolsRoot }
$config = Join-Path $Project 'sdkconfig'
if ($Action -ne 'SetTarget' -and (Test-Path -LiteralPath $config)) {
    $targetLine = Get-Content -LiteralPath $config | Where-Object { $_ -match '^CONFIG_IDF_TARGET=' }
    if ($targetLine -and $targetLine -ne 'CONFIG_IDF_TARGET="esp32s31"') { throw 'Existing sdkconfig targets another chip; use a new build directory and intentional SetTarget.' }
}
$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idf = Join-Path $env:IDF_PATH 'tools\idf.py'
if (-not (Test-Path -LiteralPath $python) -or -not (Test-Path -LiteralPath $idf)) { throw 'Activated fixed Python/SDK is incomplete.' }
New-Item -ItemType Directory -Path $LogDirectory -Force -ErrorAction Stop | Out-Null
$tag = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$stdout = Join-Path $LogDirectory ($tag+'.stdout.log')
$stderr = Join-Path $LogDirectory ($tag+'.stderr.log')
$report = Join-Path $LogDirectory ($tag+'.build.json')
$arguments = @('-u',$idf,'--preview','-C',$Project,'-B',$BuildDirectory)
switch ($Action) {
    'SetTarget' { $arguments += @('set-target','esp32s31') }
    'Reconfigure' { $arguments += 'reconfigure' }
    default { $arguments += 'build' }
}
$processExecutable = $python
if ($CMakeDirect) {
    if ($Action -eq 'SetTarget') { throw 'CMakeDirect uses the fixed S31 target in a new build directory; SetTarget is not supported.' }
    $processExecutable = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'direct_cmake.ps1'),'-Project',$Project,'-BuildDirectory',$BuildDirectory,'-Python',$python,'-Action',$Action)
    $env:IDF_CCACHE_ENABLE = '0'; $env:CCACHE_DISABLE = '1'
}
function Quote-NativeArgument([string]$Value) {
    return '"' + [regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1') + '"'
}
$commandArgs = ($arguments | ForEach-Object { Quote-NativeArgument $_ }) -join ' '
$started = [DateTime]::UtcNow
$receipt = [ordered]@{schema=1; status='RUNNING'; action=$Action; started_utc=$started.ToString('o'); project=$Project; build=$BuildDirectory; executable=$python; arguments=$arguments; stdout=$stdout; stderr=$stderr; disable_ccache_requested=[bool]$DisableCcache; idf_ccache_enable=$env:IDF_CCACHE_ENABLE; ccache_disable=$env:CCACHE_DISABLE; tools_path=$env:IDF_TOOLS_PATH; pid=$null; exit_code=$null; hardware_verified=$false}
$evidenceTool = Join-Path $PSScriptRoot 'capture_build_evidence.py'
$beforeEvidence = Join-Path $LogDirectory ($tag+'.inputs-before.json')
$afterEvidence = Join-Path $LogDirectory ($tag+'.inputs-after.json')
& $python -B $evidenceTool --project $Project --build $BuildDirectory --output $beforeEvidence --stage before
if ($LASTEXITCODE -ne 0) { throw 'Cannot record build inputs.' }
$receipt.inputs_before = $beforeEvidence
$receipt.executable = $processExecutable
$receipt.cmake_direct = [bool]$CMakeDirect
$process = Start-Process -FilePath $processExecutable -ArgumentList $commandArgs -WorkingDirectory $Project -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -ErrorAction Stop
# Retain the process handle before a fast child exits (Windows PowerShell 5.1).
$null = $process.Handle
$receipt.pid = $process.Id
$receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $report -Encoding UTF8
Write-Host ('BUILD_STARTED pid=' + $process.Id + ' report=' + $report)
$previousStamp = ''; $lastChange = [DateTime]::UtcNow; $timedOut = $false; $stopUnconfirmed = $false
try {
    while (-not $process.WaitForExit($PollSeconds*1000)) {
        $process.Refresh()
        $outBytes = (Get-Item -LiteralPath $stdout -ErrorAction SilentlyContinue).Length
        $errBytes = (Get-Item -LiteralPath $stderr -ErrorAction SilentlyContinue).Length
        $ninja = Get-Item -LiteralPath (Join-Path $BuildDirectory '.ninja_log') -ErrorAction SilentlyContinue
        $stamp = '{0}:{1}:{2}:{3}' -f $outBytes,$errBytes,$ninja.Length,$ninja.LastWriteTimeUtc
        if ($stamp -ne $previousStamp) { $lastChange = [DateTime]::UtcNow; $previousStamp = $stamp }
        $quiet = [int]([DateTime]::UtcNow-$lastChange).TotalSeconds
        Write-Host ('BUILD_RUNNING pid={0} stdout={1} stderr={2} ninja_bytes={3} unchanged_seconds={4}' -f $process.Id,$outBytes,$errBytes,$ninja.Length,$quiet)
        if ($quiet -ge 120) { Write-Host 'No new output evidence: inspect this process tree and compiler activity; this alone does not prove a hang.' }
        if (([DateTime]::UtcNow-$started).TotalSeconds -ge $TimeoutSeconds) {
            $timedOut = $true
            # Terminate only the process tree created by this invocation, never every ninja/python.
            $stopLog = Join-Path $LogDirectory ($tag+'.stop.log')
            & "$env:SystemRoot\System32\taskkill.exe" /PID $process.Id /T /F *> $stopLog
            $receipt.stop_exit_code = $LASTEXITCODE
            $receipt.stop_log = $stopLog
            if (-not $process.WaitForExit(3000)) { $stopUnconfirmed = $true }
            break
        }
    }
    if ($stopUnconfirmed) {
        $receipt.status = 'TIMED_OUT_STOP_UNCONFIRMED'
        $receipt.completed_utc = [DateTime]::UtcNow.ToString('o')
        $receipt.elapsed_seconds = ([DateTime]::UtcNow-$started).TotalSeconds
        $receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $report -Encoding UTF8
        Write-Host ('TIMEOUT: stop was not confirmed for owned pid=' + $process.Id + '. Do not start another build against the same directory; inspect permissions/process state. report=' + $report)
        exit 124
    }
    $process.WaitForExit()
    $receipt.exit_code = $process.ExitCode
    if ($null -eq $receipt.exit_code) { throw 'Child exit code unavailable; do not report success.' }
    $receipt.status = if ($timedOut) { 'TIMED_OUT' } elseif ($process.ExitCode -eq 0) { 'COMMAND_PASSED' } else { 'COMMAND_FAILED' }
    & $python -B $evidenceTool --project $Project --build $BuildDirectory --output $afterEvidence --stage after --before $beforeEvidence
    $evidenceExit = $LASTEXITCODE
    $receipt.inputs_after = $afterEvidence
    $receipt.evidence_exit_code = $evidenceExit
    if ($evidenceExit -ne 0) { $receipt.status = 'INPUT_EVIDENCE_FAILED_OR_CHANGED' }
    $receipt.completed_utc = [DateTime]::UtcNow.ToString('o')
    $receipt.elapsed_seconds = ([DateTime]::UtcNow-$started).TotalSeconds
    $receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $report -Encoding UTF8
    Write-Host ('BUILD_FINISHED status=' + $receipt.status + ' exit=' + $receipt.exit_code + ' report=' + $report)
    if ($timedOut) { exit 124 }
    if ($evidenceExit -ne 0) { exit $evidenceExit }
    exit $process.ExitCode
} finally { $process.Dispose() }
