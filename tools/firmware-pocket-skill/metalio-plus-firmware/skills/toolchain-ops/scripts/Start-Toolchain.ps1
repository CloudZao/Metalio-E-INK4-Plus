param(
    [ValidateSet('Auto','Local','Official','Mirror')][string]$Channel = 'Auto',
    [Parameter(Mandatory=$true)][string]$Cache,
    [Parameter(Mandatory=$true)][string]$Destination,
    [string]$Source, [string]$DeviceRoot, [string]$ContextFile,
    [ValidateSet('plus-base','official','debug','full')][string]$Profile = 'plus-base',
    [string]$MirrorBase, [string]$MirrorMap, [string]$ReportPath,
    [string]$BasePython, [string]$Work, [switch]$Offline
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Material-Common.ps1')
$requestedChannel = $Channel
$destinationPath = [IO.Path]::GetFullPath($Destination)
Assert-Separate $destinationPath $script:SkillRoot
Assert-Separate $destinationPath ([IO.Path]::GetFullPath($Cache))
$ancestor = Get-ResolvedMaterialPath $destinationPath
while ($ancestor) {
    if ((Split-Path $ancestor -Leaf) -eq '02_测试预置_原始材料') { throw 'Runtime must be outside original materials.' }
    $next = Split-Path $ancestor -Parent
    if ($next -eq $ancestor) { break }
    $ancestor = $next
}
# This is an explicit build-environment command, never Skill installation.
# Check the existing environment BEFORE resolving or downloading raw inputs.
if (Test-Path -LiteralPath $destinationPath) {
    if (-not (Test-Path -LiteralPath $destinationPath -PathType Container)) { throw 'Environment destination is a file; select a fresh directory.' }
    $entries = @(Get-ChildItem -LiteralPath $destinationPath -Force)
    if ($entries.Count) {
        $receiptPath = Join-Path $destinationPath 'environment.json'
        if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) { throw 'Existing environment is incomplete or unknown. Preserve it; choose a new directory or repair it explicitly.' }
        $previous = Get-Content -LiteralPath $receiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if (-not $previous.material_profile) { throw 'Existing environment lacks a verifiable material profile; inspect it or prepare a separate environment.' }
        $wanted = Get-AcquisitionProfile $Profile
        $available = Get-AcquisitionProfile $previous.material_profile
        $missing = @($wanted.item_ids | Where-Object { $_ -notin $available.item_ids })
        if ($missing.Count) { throw 'Existing environment profile does not cover this request. Preserve it and use a new environment for the additional profile.' }
        $engine = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $priorPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $checkOutput = @(& $engine -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Test-Environment.ps1') -Destination $destinationPath 2>&1)
            $checkCode = $LASTEXITCODE
        } finally { $ErrorActionPreference = $priorPreference }
        foreach ($line in $checkOutput) { Write-Host ([string]$line) }
        if ($checkCode -ne 0 -or -not (@($checkOutput | Where-Object { ([string]$_) -like 'ENVIRONMENT_REUSE_CHECK_PASSED*' }).Count)) {
            throw 'Existing environment verification failed. No download or installation was started; preserve it for diagnosis.'
        }
        if ($ReportPath) {
            $output = [IO.Path]::GetFullPath($ReportPath)
            if (Test-ContainsPath $script:SkillRoot $output) { throw 'Report must be in WORK, not Skill.' }
            $ancestor = Split-Path (Get-ResolvedMaterialPath $output) -Parent
            while ($ancestor) {
                if ((Split-Path $ancestor -Leaf) -eq '02_测试预置_原始材料') { throw 'Report must be outside original materials.' }
                $next = Split-Path $ancestor -Parent
                if ($next -eq $ancestor) { break }
                $ancestor = $next
            }
            if ($Source -and (Test-ContainsPath $Source $output)) { throw 'Report must be outside original material input.' }
            if ($DeviceRoot) {
                $originals = Join-Path ([IO.Path]::GetFullPath($DeviceRoot)) '02_测试预置_原始材料'
                if (Test-ContainsPath $originals $output) { throw 'Report must be outside original material input.' }
            }
            $writer = New-Object IO.StreamWriter(([IO.File]::Open($output,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)),(New-Object Text.UTF8Encoding($false)))
            try { $writer.WriteLine(([ordered]@{schema=1; status='environment_reused'; destination=$destinationPath; profile=$previous.material_profile; requested_profile=$Profile; registry_sha256=$script:RegistrySHA; downloaded_files=0; installation_performed=$false} | ConvertTo-Json -Depth 5)) }
            finally { $writer.Dispose() }
        }
        Write-Host ('Verified existing environment; no materials fetched. Activate in each build terminal: . "' + $destinationPath + '\Activate.ps1"')
        return
    }
}
$selection = & (Join-Path $PSScriptRoot 'Resolve-Materials.ps1') -Channel $Channel -Source $Source -DeviceRoot $DeviceRoot -MirrorBase $MirrorBase -MirrorMap $MirrorMap -ContextFile $ContextFile
# An explicitly supplied context is an input record; never rewrite it.
$contextPath = Join-Path $destinationPath 'material-context.json'
if (Test-ContainsPath $script:SkillRoot $contextPath) { throw 'Material context must be in WORK or runtime, never in Skill.' }
if ($selection.source -and (Test-ContainsPath $selection.source $contextPath)) { throw 'Material context must be outside original materials.' }
$preflight = & (Join-Path $PSScriptRoot 'Test-SetupPaths.ps1') -Destination $Destination -Cache $Cache -Work $Work -Source $selection.source
Write-Host ('Environment preflight: path length=' + $preflight.destination_characters + '; writable parent checks passed.')
$started = [DateTime]::UtcNow
# Preserve original Auto semantics: Auto-discovered Local must not become strict Local.
$getArgs = @{Channel=$selection.requested_channel;Cache=$Cache;Profile=$Profile;Stage='profile';MirrorBase=$selection.mirror_base;MirrorMap=$selection.mirror_map;Offline=$Offline}
if ($selection.source) { $getArgs.Source = $selection.source }
if ($selection.device_root) { $getArgs.DeviceRoot = $selection.device_root }
if ($ReportPath) { $getArgs.ReportPath = $ReportPath }
$raw = & (Join-Path $PSScriptRoot 'Get-Materials.ps1') @getArgs
if (-not $raw -or @($raw).Count -ne 1) { throw 'Expected exactly one verified raw root.' }
$acquired = [DateTime]::UtcNow
& (Join-Path $PSScriptRoot 'Prepare-Toolchain.ps1') -Mirror $raw -Destination $Destination -BasePython $BasePython -Profile $Profile
$finished = [DateTime]::UtcNow
$receipt = [ordered]@{
    schema=1; material_profile=$Profile; requested_channel=$selection.requested_channel; invocation_channel=$requestedChannel; channel=$selection.channel; selection_reason=$selection.reason
    strict_local=$selection.strict_local; offline=[bool]$Offline; raw_root=$raw; destination=$destinationPath
    registry_sha256=$script:RegistrySHA; started_utc=$started.ToString('o'); completed_utc=$finished.ToString('o')
    acquisition_seconds=($acquired-$started).TotalSeconds; environment_seconds=($finished-$acquired).TotalSeconds
    firmware_built=$false; hardware_verified=$false
}
$receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destinationPath 'setup-timing.json') -Encoding UTF8
$writer = New-Object IO.StreamWriter(([IO.File]::Open($contextPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)),(New-Object Text.UTF8Encoding($false)))
try { $writer.WriteLine(($selection | ConvertTo-Json -Depth 5)) } finally { $writer.Dispose() }
$preflight | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destinationPath 'path-preflight.json') -Encoding UTF8
Write-Host ('Material binding: ' + $contextPath)
Write-Host ('Activate in each build terminal: . "' + $destinationPath + '\Activate.ps1"')
