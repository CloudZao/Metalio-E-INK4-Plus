param([Parameter(Mandatory=$true)][string]$Alias)
$ErrorActionPreference = 'Stop'
if (-not $env:IDF_TOOLS_PATH) { throw 'Activate the fixed environment before assigning its tool alias.' }
$target = [IO.Path]::GetFullPath($env:IDF_TOOLS_PATH).TrimEnd('\')
$aliasPath = [IO.Path]::GetFullPath($Alias).TrimEnd('\')
if ($aliasPath -match '[^\x21-\x7e]' -or $aliasPath.Length -ge $target.Length) { throw 'Tool alias must be a shorter ASCII path without spaces.' }
. (Join-Path $PSScriptRoot 'Material-Common.ps1')
Assert-Separate $aliasPath $target
Assert-Separate $aliasPath $script:SkillRoot
if (-not (Test-Path -LiteralPath $target -PathType Container)) { throw 'Activated tool directory is missing.' }
if (Test-Path -LiteralPath $aliasPath) {
    $item = Get-Item -LiteralPath $aliasPath -Force
    if ($item.LinkType -ne 'Junction' -or @($item.Target).Count -ne 1 -or
        [IO.Path]::GetFullPath([string]$item.Target).TrimEnd('\') -ne $target) {
        throw 'Alias exists and is not the expected junction. Do not replace it.'
    }
} else {
    $parent = Split-Path $aliasPath -Parent
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw 'Choose an existing writable parent for the short tool alias.' }
    New-Item -ItemType Junction -Path $aliasPath -Target $target | Out-Null
}
$env:PATH = (($env:PATH -split ';' | ForEach-Object {
    if ($_.Equals($target,[StringComparison]::OrdinalIgnoreCase)) { $aliasPath }
    elseif ($_.StartsWith($target+'\',[StringComparison]::OrdinalIgnoreCase)) { $aliasPath + $_.Substring($target.Length) }
    else { $_ }
}) -join ';')
$env:IDF_TOOLS_PATH = $aliasPath
Write-Host ('Tools alias active in this process: ' + $aliasPath + ' -> ' + $target)
