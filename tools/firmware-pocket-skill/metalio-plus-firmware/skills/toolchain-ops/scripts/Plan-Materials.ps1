param(
    [ValidateSet('install','profile','python-bootstrap','git-bootstrap','sdk','tools','python-core','extra')]
    [string]$Stage = 'install',
    [ValidateSet('plus-base','official','debug','full')][string]$Profile = 'plus-base',
    [string[]]$Id = @(),
    [string]$Reason,
    [string]$OutputFile
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Material-Common.ps1')
if ($Id.Count -and -not $PSBoundParameters.ContainsKey('Stage')) { $Stage = 'extra' }
if ($Stage -eq 'install' -and $Id.Count) { throw 'Skill installation selects zero resources. Use a separate acquisition-stage plan.' }
if ($Id.Count -and [string]::IsNullOrWhiteSpace($Reason)) { throw 'Explicit IDs require a Reason tied to this task.' }
if ($Stage -eq 'extra' -and -not $Id.Count) { throw 'Stage extra requires one or more exact resource IDs.' }
$registry = Read-FrozenRegistry (Join-Path $script:ModuleRoot 'references\registry.json')
if ($registry.target -ne 'esp32s31' -or $registry.platform -ne 'windows-x64') { throw 'Expected Plus ESP32-S31 / Windows x64 registry.' }
$catalog = Get-Content -LiteralPath (Join-Path $script:ModuleRoot 'assets\acquisition-stages.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($catalog.registry_sha256 -ne $script:RegistrySHA -or $catalog.target -ne 'esp32s31') { throw 'Stage catalog identity does not match the Plus registry.' }
$selectedProfile = @(Get-MaterialItems $registry $Profile)
$idsWanted = @{}
$why = ''
if ($Stage -eq 'profile') {
    $why = 'Complete existing installation profile: ' + $Profile
    foreach ($item in $selectedProfile) { $idsWanted[$item.id] = @($why) }
} elseif ($Stage -notin @('install','extra')) {
    $stageProperty = $catalog.stages.PSObject.Properties[$Stage]
    if (-not $stageProperty) { throw 'Unknown acquisition stage.' }
    $definition = $stageProperty.Value
    $why = $definition.reason
    $wanted = @($definition.item_ids)
    if ($Stage -eq 'tools') {
        $wanted = @($selectedProfile | Where-Object { $_.category -eq 'tools' -and $_.id -notin @('TOOL-001','TOOL-002') } | ForEach-Object { $_.id })
    }
    foreach ($identity in $wanted) { $idsWanted[$identity] = @($why) }
}
foreach ($identity in $Id) {
    if (-not $idsWanted.ContainsKey($identity)) { $idsWanted[$identity] = @() }
    $idsWanted[$identity] += ('task: ' + $Reason.Trim())
}
$known = @{}
foreach ($item in $registry.items) {
    if ($known.ContainsKey($item.id)) { throw 'Duplicate registry identity.' }
    $known[$item.id] = $item
}
foreach ($identity in $idsWanted.Keys) {
    if (-not $known.ContainsKey($identity)) { throw ('Unknown registered resource ID: ' + $identity) }
}
$rows = @()
$bytes = 0L
foreach ($item in $registry.items) {
    if ($idsWanted.ContainsKey($item.id)) {
        $rows += [pscustomobject][ordered]@{id=$item.id; filename=$item.filename; version=$item.version; bytes=$item.bytes; reasons=@($idsWanted[$item.id])}
        $bytes += $item.bytes
    }
}
$result = [pscustomobject][ordered]@{
    schema=1; target='esp32s31'; platform='windows-x64'; skill_version='0.4.0'
    registry_sha256=$script:RegistrySHA; stage=$Stage; profile=$Profile
    selected_ids=@($rows | ForEach-Object { $_.id }); selected=@($rows)
    selected_count=$rows.Count; selected_bytes_upper_bound=$bytes; network_bytes_known=$false
    installation_performed=$false
    note='This plan selects only the current stage. Existing/local resources reduce actual downloads; selected originals are not an installed environment.'
}
if ($OutputFile) {
    $output = [IO.Path]::GetFullPath($OutputFile)
    if (Test-ContainsPath $script:SkillRoot $output) { throw 'Write the plan to WORK, not the Skill.' }
    $ancestor = Split-Path (Get-ResolvedMaterialPath $output) -Parent
    while ($ancestor) {
        if ((Split-Path $ancestor -Leaf) -eq '02_测试预置_原始材料') { throw 'Write the plan to WORK, not original materials.' }
        $next = Split-Path $ancestor -Parent
        if ($next -eq $ancestor) { break }
        $ancestor = $next
    }
    $parent = Split-Path $output -Parent
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw 'Plan output parent must already exist in WORK.' }
    $stream = [IO.File]::Open($output,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
    $writer = New-Object IO.StreamWriter($stream,(New-Object Text.UTF8Encoding($false)))
    try { $writer.WriteLine(($result | ConvertTo-Json -Depth 8)) } finally { $writer.Dispose() }
}
$result
