param(
    [ValidateSet('Auto','Local','Official','Mirror')][string]$Channel = 'Auto',
    [string]$Source, [string]$DeviceRoot, [string]$MirrorBase, [string]$MirrorMap, [string]$ContextFile, [string]$SaveContext
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Material-Common.ps1')
$requested = $Channel
$strictLocal = $Channel -eq 'Local'
$reason = 'explicit_channel'
$context = $null
if ($ContextFile) {
    if (-not (Test-Path -LiteralPath $ContextFile -PathType Leaf)) { throw 'Specified context file does not exist. Supply a current path or omit it.' }
    $context = Get-Content -LiteralPath $ContextFile -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($context.schema -ne 1 -or $context.registry_sha256 -ne $script:RegistrySHA) { throw 'Material context is not for the frozen Plus r2 registry.' }
}
if ($Channel -eq 'Auto' -and -not ($Source -or $DeviceRoot) -and $context) {
    $Source = $context.source; $DeviceRoot = $context.device_root
    if (-not $MirrorBase) { $MirrorBase = $context.mirror_base }
    if (-not $MirrorMap -and $context.PSObject.Properties['mirror_map']) { $MirrorMap = $context.mirror_map }
    if ($context.PSObject.Properties['requested_channel']) { $requested = $context.requested_channel }
    elseif ($context.channel -in @('Official','Mirror')) { $requested = $context.channel }
    elseif ($context.reason -eq 'explicit_channel' -and $context.channel -eq 'Local') { $requested = 'Local' }
    if ($requested -notin @('Auto','Local','Official','Mirror')) { throw 'Invalid saved material channel.' }
    $Channel = $requested; $strictLocal = $requested -eq 'Local'; $reason = 'saved_context'
}
if ($Channel -eq 'Auto') {
    if ($Source) { $Channel = 'Local'; $reason = 'explicit_source_preferred' }
    else {
        if (-not $DeviceRoot) {
            $group = Split-Path $script:SkillRoot -Parent
            if ((Split-Path $group -Leaf) -like '01_*') {
                $candidate = Split-Path $group -Parent
                if (Test-Path -LiteralPath (Join-Path $candidate '02_测试预置_原始材料') -PathType Container) { $DeviceRoot = $candidate }
            }
        }
        if ($DeviceRoot) {
            $DeviceRoot = [IO.Path]::GetFullPath($DeviceRoot)
            if (-not (Test-Path -LiteralPath $DeviceRoot -PathType Container)) {
                if ($context -and $reason -eq 'saved_context' -and $requested -eq 'Auto') {
                    Write-Warning 'Saved device path is unavailable here; Auto will use current cache and online sources.'
                    $DeviceRoot = $null
                } else { throw 'DeviceRoot does not exist. Supply this machine current device directory.' }
            }
            if ($DeviceRoot) {
                $preload = Join-Path $DeviceRoot '02_测试预置_原始材料'
                if (Test-Path -LiteralPath $preload -PathType Container) { $Channel = 'Local'; $Source = $preload; $reason = 'device_preload_preferred' }
            }
        }
        if ($Channel -eq 'Auto') { $Channel = 'Official'; $reason = 'no_preload' }
    }
}
if ($Channel -eq 'Local') {
    if (-not $Source -and $DeviceRoot) { $Source = Join-Path ([IO.Path]::GetFullPath($DeviceRoot)) '02_测试预置_原始材料' }
    if (-not $Source -or -not (Test-Path -LiteralPath $Source)) {
        if ($strictLocal) { throw 'Explicit Local materials are missing; local-only requests cannot switch to network.' }
        Write-Warning 'Preferred local materials are absent; Auto will use verified cache and online sources.'
        $Source = $null; $Channel = 'Official'; $reason = 'preferred_local_missing'
        if ($context -and $requested -eq 'Auto' -and $DeviceRoot -and -not (Test-Path -LiteralPath $DeviceRoot -PathType Container)) { $DeviceRoot = $null }
    } else { $Source = [IO.Path]::GetFullPath($Source) }
} elseif ($Source) { throw 'Source conflicts with an explicit non-local channel.' }
if ($MirrorMap) {
    if (-not (Test-Path -LiteralPath $MirrorMap -PathType Leaf)) { throw 'Configured MirrorMap file is missing. Supply its current path.' }
    $MirrorMap = [IO.Path]::GetFullPath($MirrorMap)
}
if ($Channel -eq 'Mirror' -and -not ($MirrorBase -or $MirrorMap)) { throw 'Mirror requires a configured HTTPS MirrorBase or an existing MirrorMap file.' }
if ($MirrorBase) { Assert-HttpsRoot $MirrorBase }
$result = [pscustomobject][ordered]@{
    schema=1; registry_sha256=$script:RegistrySHA; requested_channel=$requested
    channel=$Channel; strict_local=$strictLocal; source=$Source
    device_root=$DeviceRoot; mirror_base=$MirrorBase; mirror_map=$MirrorMap; reason=$reason
}
if ($SaveContext) {
    $savePath = [IO.Path]::GetFullPath($SaveContext)
    if (Test-ContainsPath $script:SkillRoot $savePath) { throw 'SaveContext must be outside Skill.' }
    if ($Source -and (Test-ContainsPath $Source $savePath)) { throw 'SaveContext must be outside original materials.' }
    $ancestor = Split-Path (Get-ResolvedMaterialPath $savePath) -Parent
    while ($ancestor) {
        if ((Split-Path $ancestor -Leaf) -eq '02_测试预置_原始材料') { throw 'SaveContext must be outside original materials.' }
        $next = Split-Path $ancestor -Parent
        if ($next -eq $ancestor) { break }
        $ancestor = $next
    }
    if (-not (Test-Path -LiteralPath (Split-Path $savePath -Parent) -PathType Container)) { throw 'SaveContext parent must already exist in WORK.' }
    $stream = [IO.File]::Open($savePath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
    $writer = New-Object IO.StreamWriter($stream,(New-Object Text.UTF8Encoding($false)))
    try { $writer.WriteLine(($result | ConvertTo-Json -Depth 5)) } finally { $writer.Dispose() }
}
$result
