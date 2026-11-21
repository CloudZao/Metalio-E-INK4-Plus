param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [Parameter(Mandatory=$true)][string]$Cache,
    [string]$Work, [string]$Source
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Material-Common.ps1')
$destinationPath = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
$cachePath = [IO.Path]::GetFullPath($Cache).TrimEnd('\')
if ($destinationPath.Length -gt 40 -or $destinationPath -match '[^\x21-\x7e]') {
    throw ('Environment path must be ASCII without spaces and <=40 characters; actual={0}, length={1}. Choose a writable shorter path before installation.' -f $destinationPath,$destinationPath.Length)
}
Assert-Separate $destinationPath $cachePath
Assert-Separate $destinationPath $script:SkillRoot
Assert-Separate $cachePath $script:SkillRoot
if ($Work) {
    $Work = [IO.Path]::GetFullPath($Work).TrimEnd('\')
    if ($Work -match '[^\x21-\x7e]') { throw 'Project WORK must be a short ASCII path without spaces.' }
    Assert-Separate $Work $destinationPath; Assert-Separate $Work $cachePath; Assert-Separate $Work $script:SkillRoot
}
if ($Source) {
    $sourcePath = [IO.Path]::GetFullPath($Source)
    Assert-Separate $sourcePath $destinationPath; Assert-Separate $sourcePath $cachePath
    if ($Work) { Assert-Separate $sourcePath $Work }
}
# Probe the existing parent only, then remove only this call's uniquely named file.
# No permanent directories are created by this preflight.
$probes = @()
foreach ($target in @($destinationPath,$cachePath,$Work) | Where-Object { $_ }) {
    $parent = $target
    while (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        if (Test-Path -LiteralPath $parent) { throw ('Path is a file: ' + $parent) }
        $next = Split-Path $parent -Parent
        if (-not $next -or $next -eq $parent) { throw ('Cannot locate an existing parent: ' + $target) }
        $parent = $next
    }
    $probe = Join-Path $parent ('.plus-write-probe-' + [Guid]::NewGuid().ToString('N'))
    try {
        $stream = [IO.File]::Open($probe,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
        $stream.Dispose()
        $probes += [ordered]@{target=$target; tested_parent=$parent; writable=$true}
    } catch { throw ('Cannot write at intended location: ' + $target + '. Use an allowed path or report the actual permission block. ' + $_.Exception.Message) }
    finally { if (Test-Path -LiteralPath $probe) { Remove-Item -LiteralPath $probe -Force } }
}
$temp = [IO.Path]::GetTempPath()
[pscustomobject][ordered]@{status='PATH_PREFLIGHT_PASSED'; destination=$destinationPath; destination_characters=$destinationPath.Length; probes=$probes; process_temp=$temp; note='Parent write check only; installer-specific rights and disk capacity still require actual results.'}
