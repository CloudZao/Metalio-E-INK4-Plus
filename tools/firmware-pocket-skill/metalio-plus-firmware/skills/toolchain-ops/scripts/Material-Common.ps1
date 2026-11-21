# PowerShell 5.1+; no Python, WSL or global installation required.
$script:RegistrySHA = 'dc38a81481b934ccfc7ccc246f1a6408396d39cfeabe365a9d123354b736e354'
$script:RawZipSHA = '4675ada29453e3078d29fd768d2bea4450d79297598e0878300b5941c76e5819'
$script:RawZipBytes = 1680776568L
$script:ModuleRoot = Split-Path $PSScriptRoot -Parent
$script:SkillRoot = Split-Path (Split-Path $script:ModuleRoot -Parent) -Parent

function Get-ResolvedMaterialPath([string]$Path) {
    # Resolve through SUBST/junction aliases before any installer/download writes.
    if (-not ('PlusMaterialPathIdentity' -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class PlusMaterialPathIdentity {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint mode, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern uint GetFinalPathNameByHandleW(SafeFileHandle handle, StringBuilder path, uint count, uint flags);
    public static string Resolve(string path) {
        string name = path.StartsWith(@"\\?\") ? path : (path.StartsWith(@"\\") ? @"\\?\UNC\" + path.Substring(2) : @"\\?\" + path);
        using (var handle = CreateFileW(name, 0, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero)) {
            if (handle.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            var output = new StringBuilder(32768);
            uint length = GetFinalPathNameByHandleW(handle, output, (uint)output.Capacity, 0);
            if (length == 0 || length >= output.Capacity) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            string result = output.ToString();
            if (result.StartsWith(@"\\?\UNC\")) return @"\\" + result.Substring(8);
            return result.StartsWith(@"\\?\") ? result.Substring(4) : result;
        }
    }
}
"@
    }
    $full = [IO.Path]::GetFullPath($Path)
    $probe = $full
    $tail = New-Object 'System.Collections.Generic.List[string]'
    while (-not (Test-Path -LiteralPath $probe)) {
        $leaf = [IO.Path]::GetFileName($probe.TrimEnd('\','/'))
        $parent = [IO.Path]::GetDirectoryName($probe.TrimEnd('\','/'))
        if (-not $parent -or $parent -eq $probe) { throw ('No existing ancestor for path: ' + $full) }
        $tail.Insert(0,$leaf); $probe = $parent
    }
    $resolved = [PlusMaterialPathIdentity]::Resolve($probe)
    foreach ($part in $tail) { $resolved = [IO.Path]::Combine($resolved,$part) }
    return [IO.Path]::GetFullPath($resolved)
}
function Test-ContainsPath([string]$Parent, [string]$Child) {
    $p = (Get-ResolvedMaterialPath $Parent).TrimEnd('\','/')
    $c = (Get-ResolvedMaterialPath $Child).TrimEnd('\','/')
    return $c.Equals($p,[StringComparison]::OrdinalIgnoreCase) -or $c.StartsWith($p + '\',[StringComparison]::OrdinalIgnoreCase)
}
function Assert-Separate([string]$First, [string]$Second) {
    if ((Test-ContainsPath $First $Second) -or (Test-ContainsPath $Second $First)) {
        throw 'Input, installation and Skill locations must be separate directories.'
    }
}
function Get-SafePath([string]$Root, [string]$Relative) {
    if (-not $Relative -or $Relative -match '(^/|\\|:|(^|/)\.\.(/|$))') { throw 'Unsafe relative material path.' }
    $result = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (-not (Test-ContainsPath $Root $result) -or $result -eq [IO.Path]::GetFullPath($Root)) { throw 'Material escapes root.' }
    $probe = [IO.Path]::GetFullPath($Root)
    foreach ($part in @($Relative -split '/')) {
        $probe = [IO.Path]::Combine($probe,$part)
        if (Test-Path -LiteralPath $probe) {
            $entry = Get-Item -LiteralPath $probe -Force
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Material path contains a reparse descendant.' }
        }
    }
    return $result
}
function Get-MaterialSHA256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
function Assert-FileIdentity([string]$Path, [long]$Size, [string]$SHA) {
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Expected a regular original file.' }
    if ($file.Length -ne $Size -or (Get-MaterialSHA256 $Path) -ne $SHA) {
        throw ('Material size or SHA256 mismatch: ' + $file.Name)
    }
}
function Read-FrozenRegistry([string]$Path) {
    if ((Get-MaterialSHA256 $Path) -ne $script:RegistrySHA) { throw 'Registry differs from frozen r2.' }
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}
function Get-AcquisitionProfile([string]$Profile = 'plus-base') {
    $catalog = Get-Content -LiteralPath (Join-Path $script:ModuleRoot 'assets\acquisition-profiles.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($catalog.registry_sha256 -ne $script:RegistrySHA) { throw 'Acquisition profiles and registry disagree.' }
    $value = $catalog.profiles.PSObject.Properties[$Profile]
    if (-not $value) { throw ('Unknown material profile: ' + $Profile) }
    return $value.Value
}
function Get-MaterialItems($Registry, [string]$Profile = 'plus-base') {
    $selected = Get-AcquisitionProfile $Profile
    $items = @($Registry.items | Where-Object { $_.id -in $selected.item_ids })
    if ($items.Count -ne $selected.item_ids.Count -or $items.Count -ne $selected.file_count -or
        ($items | Measure-Object bytes -Sum).Sum -ne $selected.bytes) { throw 'Material profile selection is inconsistent.' }
    return $items
}
function Assert-RawMaterials([string]$Root, $Registry, [string]$Profile = 'plus-base') {
    $items = @(Get-MaterialItems $Registry $Profile)
    $count = 0
    foreach ($item in $items) {
        Assert-FileIdentity (Get-SafePath $Root $item.path) $item.bytes $item.sha256
        $count++
        if ($count % 25 -eq 0) { Write-Host ('Verified {0}/{1} selected raw files' -f $count,$items.Count) }
    }
}
function Get-MaterialUrls($Item, [string]$Channel, [string]$MirrorBase) {
    $official = @($Item.upstream_url)
    # Historical recorded endpoints, not invented mirror domains.
    if ($Item.retrieved_url -and $Item.retrieved_url -notin $official) { $official += $Item.retrieved_url }
    if ($MirrorBase) {
        Assert-HttpsRoot $MirrorBase
        $encoded = (($Item.path -split '/' | ForEach-Object { [Uri]::EscapeDataString($_) }) -join '/')
        $mirror = $MirrorBase.TrimEnd('/') + '/' + $encoded
        if ($Channel -eq 'Mirror') { return @($mirror) + $official }
        return $official + @($mirror)
    }
    return $official
}
function Assert-HttpsRoot([string]$Value) {
    $uri = [Uri]$Value
    if (-not $uri.IsAbsoluteUri -or $uri.Scheme -ne 'https' -or $uri.UserInfo -or $uri.Query -or $uri.Fragment) {
        throw 'MirrorBase must be an HTTPS root without credentials, query or fragment.'
    }
}
function Receive-FrozenFile([string]$Url,[string]$Target,[long]$Size,[string]$SHA,[int]$TimeoutSeconds,[int]$DeadlineSeconds,[switch]$AllowLoopbackForTests) {
    $uri = [Uri]$Url
    if ($uri.Scheme -ne 'https' -and -not ($AllowLoopbackForTests -and $uri.Scheme -eq 'http' -and $uri.IsLoopback)) { throw 'Download requires HTTPS.' }
    if ($uri.UserInfo) { throw 'Download URLs must not embed credentials.' }
    if (Test-Path -LiteralPath $Target) { Assert-FileIdentity $Target $Size $SHA; return }
    $part = $Target + '.part'; $identityPath = $part + '.json'
    $identity = [ordered]@{schema=1; bytes=$Size; sha256=$SHA}
    if (Test-Path -LiteralPath $part) {
        $file = Get-Item -LiteralPath $part
        if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unsafe partial file.' }
        if (-not (Test-Path -LiteralPath $identityPath -PathType Leaf)) { throw 'Partial lacks identity sidecar; inspect or choose a fresh cache.' }
        $prior = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($prior.sha256 -ne $SHA -or $prior.bytes -ne $Size) { throw 'Partial belongs to another material identity.' }
        if ($file.Length -gt $Size) { throw 'Oversize partial retained for inspection.' }
        if ($file.Length -eq $Size) {
            try { Assert-FileIdentity $part $Size $SHA }
            catch {
                $retained = $part + '.rejected-' + [Guid]::NewGuid().ToString('N')
                [IO.File]::Move($part,$retained)
                throw ('Complete partial hash mismatch; retained: ' + $retained)
            }
            [IO.File]::Move($part,$Target); return
        }
    } else { $identity | ConvertTo-Json | Set-Content -LiteralPath $identityPath -Encoding UTF8 }
    $offset = 0L
    if (Test-Path -LiteralPath $part) { $offset = (Get-Item -LiteralPath $part).Length }
    $response = $null; $inputStream = $null; $outputStream = $null
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $request = [Net.HttpWebRequest]::Create($Url)
        $request.Timeout = $TimeoutSeconds * 1000; $request.ReadWriteTimeout = $TimeoutSeconds * 1000
        $request.UserAgent = 'Metalio-Plus-Skill/0.4'
        $request.Headers['Accept-Encoding'] = 'identity'
        if ($offset -gt 0) { $request.AddRange($offset) }
        $watch = [Diagnostics.Stopwatch]::StartNew()
        try { $response = $request.GetResponse() }
        catch { $_.Exception.Data['acquisition_context']='network'; throw }
        if ($response.ResponseUri.Scheme -ne 'https' -and -not ($AllowLoopbackForTests -and $response.ResponseUri.IsLoopback -and $response.ResponseUri.Scheme -eq 'http')) { throw 'Insecure download redirect refused.' }
        if ($response.ContentEncoding -and $response.ContentEncoding -ne 'identity') { throw 'Encoded response cannot preserve registered byte ranges.' }
        $status = [int]$response.StatusCode
        if ($status -eq 206) {
            $range = $response.Headers['Content-Range']
            if ($range -notmatch '^bytes ([0-9]+)-([0-9]+)/([0-9]+)$') { throw 'Invalid Content-Range.' }
            $start = [long]$Matches[1]; $end = [long]$Matches[2]; $total = [long]$Matches[3]
            if ($start -ne $offset -or $total -ne $Size -or $end -lt $start -or $end -ge $Size -or
                ($response.ContentLength -ge 0 -and $response.ContentLength -ne ($end-$start+1))) { throw 'Content-Range does not match frozen material and resume offset.' }
        } elseif ($status -eq 200) {
            if ($response.ContentLength -ge 0 -and $response.ContentLength -ne $Size) { throw 'Response length differs from frozen material.' }
            if ($offset -gt 0) {
                $retained = $part + '.restart-' + [Guid]::NewGuid().ToString('N')
                [IO.File]::Move($part,$retained)
                Write-Host ('Server ignored Range; restarting from zero; retained prior partial: ' + $retained)
                $offset = 0L
            }
        } else { throw ('Unexpected HTTP status: ' + $status) }
        Write-Host ('Downloading {0}: {1}/{2} bytes; host={3}' -f [IO.Path]::GetFileName($Target),$offset,$Size,$response.ResponseUri.Host)
        $inputStream = $response.GetResponseStream()
        $outputStream = [IO.File]::Open($part,[IO.FileMode]::Append,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        $buffer = New-Object byte[] (65536); $total = $offset; $lastProgress = 0.0
        while ($true) {
            if ($watch.Elapsed.TotalSeconds -ge $DeadlineSeconds) { $request.Abort(); throw 'Attempt time budget exceeded; identity partial retained.' }
            try { $read = $inputStream.Read($buffer,0,$buffer.Length) }
            catch { $_.Exception.Data['acquisition_context']='network'; throw }
            if ($read -eq 0) { break }
            if ($total + $read -gt $Size) { throw 'Download exceeded registered size.' }
            $outputStream.Write($buffer,0,$read); $total += $read
            if ($watch.Elapsed.TotalSeconds - $lastProgress -ge 5) {
                $outputStream.Flush()
                Write-Host ('Download progress: {0}/{1} bytes ({2:N1}%)' -f $total,$Size,(100.0*$total/$Size))
                $lastProgress = $watch.Elapsed.TotalSeconds
            }
            if ($watch.Elapsed.TotalSeconds -gt $DeadlineSeconds) { throw 'Attempt time budget exceeded; verified identity partial retained for resume.' }
        }
        $outputStream.Dispose(); $outputStream = $null
        if ($total -ne $Size) { throw ('Incomplete response; resume retained at byte ' + $total) }
        try { Assert-FileIdentity $part $Size $SHA }
        catch {
            $retained = $part + '.rejected-' + [Guid]::NewGuid().ToString('N')
            [IO.File]::Move($part,$retained)
            throw ('Downloaded identity validation failed: ' + $_.Exception.Message + '; retained: ' + $retained)
        }
        [IO.File]::Move($part,$Target)
        Write-Host ('Verified SHA256: ' + [IO.Path]::GetFileName($Target))
    } finally {
        if ($outputStream) { $outputStream.Dispose() }
        if ($inputStream) { $inputStream.Dispose() }
        if ($response) { $response.Dispose() }
        # Keep incomplete data and its frozen identity for retries and future calls.
    }
}

function Expand-RegisteredMaterials([string]$Archive,[string]$CacheRoot,$Registry,[string]$Profile = 'plus-base') {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $entryMap = @{}
        foreach ($entry in $zip.Entries) {
            if ($entryMap.ContainsKey($entry.FullName)) { throw 'Duplicate ZIP entry.' }
            $entryMap[$entry.FullName] = $entry
        }
        foreach ($item in @(Get-MaterialItems $Registry $Profile)) {
            $entry = $entryMap[$item.zip_entry]
            if (-not $entry -or $entry.Length -ne $item.bytes) { throw ('ZIP member missing or wrong size: ' + $item.filename) }
            $target = Get-SafePath $CacheRoot $item.path
            if (Test-Path -LiteralPath $target) { Assert-FileIdentity $target $item.bytes $item.sha256; continue }
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            $incoming = $entry.Open()
            try {
                $outgoing = [IO.File]::Open($target,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
                try { $incoming.CopyTo($outgoing) } finally { $outgoing.Dispose() }
            } finally { $incoming.Dispose() }
            Assert-FileIdentity $target $item.bytes $item.sha256
        }
    } finally { $zip.Dispose() }
}

