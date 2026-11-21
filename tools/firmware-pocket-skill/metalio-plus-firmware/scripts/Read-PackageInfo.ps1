# Compact, read-only package metadata. Never prints the full manifest or registry.
[CmdletBinding(DefaultParameterSetName='Summary')]
param(
    [Parameter(ParameterSetName='Path')]
    [ValidateNotNullOrEmpty()][string]$MaterialPath,
    [Parameter(ParameterSetName='Find')]
    [ValidateNotNullOrEmpty()][string]$Find,
    [Parameter(ParameterSetName='Find')]
    [ValidateRange(1,10)][int]$Limit = 3
)
$ErrorActionPreference = 'Stop'
try {
    if ($PSCmdlet.ParameterSetName -eq 'Find' -and [string]::IsNullOrWhiteSpace($Find)) {
        throw 'Find is required when using Limit.'
    }
    $root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
    $manifest = Get-Content -LiteralPath (Join-Path $root 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $registryName = [string]$manifest.materials.registry
    if (-not $registryName -or [IO.Path]::IsPathRooted($registryName)) { throw 'Invalid package registry path.' }
    $registryPath = [IO.Path]::GetFullPath((Join-Path $root $registryName))
    $prefix = $root.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $registryPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Registry must remain inside this Skill package.'
    }
    # A lexical child path can still escape through a junction or symlink.
    $cursor = $registryPath
    while ($cursor -ne $root) {
        $node = Get-Item -LiteralPath $cursor -Force
        if (($node.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Registry path cannot traverse a reparse point.'
        }
        $cursor = Split-Path $cursor -Parent
    }
    $expected = [string]$manifest.materials.registry_sha256
    if ($expected -notmatch '^[a-fA-F0-9]{64}$') { throw 'Missing valid registry SHA-256 in manifest.' }
    $hasher = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($registryPath)
    try { $actual = ([BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
    finally { $stream.Dispose(); $hasher.Dispose() }
    if ($actual -ne $expected.ToLowerInvariant()) { throw 'Frozen registry SHA-256 mismatch.' }
    $registry = Get-Content -LiteralPath $registryPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $items = @($registry.items)
    if ($items.Count -ne $manifest.materials.raw_file_count -or $items.Count -ne $registry.material_count) {
        throw 'Registry item count does not match the declared material count.'
    }
    $result = [ordered]@{
        name = $manifest.name
        version = $manifest.version
        revision = $manifest.revision
        registry_sha256 = $actual
        registry_identity_verified = $true
        scope = 'metadata only; not installed-toolchain, artifact, or hardware validation'
    }
    if ($PSCmdlet.ParameterSetName -eq 'Summary') {
        $result['entrypoint'] = $manifest.entrypoint
        $result['target'] = $manifest.target
        $result['materials'] = [ordered]@{
            version = $manifest.materials.version
            registry_version = $registry.version
            item_count = $items.Count
            sdk_version = $manifest.materials.sdk_version
            sdk_commit = $manifest.materials.sdk_commit
        }
        $result['modules'] = $manifest.modules
    } else {
        if ($PSCmdlet.ParameterSetName -eq 'Path') {
            $query = $MaterialPath.Replace('\','/')
            $matches = @($items | Where-Object { [string]::Equals($_.path, $query, [StringComparison]::Ordinal) })
            if ($matches.Count -ne 1) { throw 'MaterialPath must identify exactly one registered path; use -Find to locate it.' }
            $Limit = 1
        } else {
            $query = $Find.Trim()
            if (-not $query) { throw 'Find cannot be whitespace.' }
            $matches = @($items | Where-Object {
                $text = (@($_.id, $_.category, $_.package_name, $_.filename, $_.version, $_.path, $_.upstream_url) -join ' ')
                $text.IndexOf($query, [StringComparison]::OrdinalIgnoreCase) -ge 0
            })
        }
        $result['query'] = $query
        $result['total'] = $matches.Count
        $result['returned'] = [Math]::Min($matches.Count, $Limit)
        $result['truncated'] = $matches.Count -gt $Limit
        $result['items'] = @($matches | Select-Object -First $Limit -Property id, package_name, version, path, zip_entry, bytes, sha256, upstream_url, retrieved_url)
    }
    $result | ConvertTo-Json -Depth 7
} catch {
    [Console]::Error.WriteLine('Package info failed: ' + $_.Exception.Message)
    exit 1
}
