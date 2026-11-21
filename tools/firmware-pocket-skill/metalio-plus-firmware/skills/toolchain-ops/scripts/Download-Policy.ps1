# Loaded by Get-Materials; no network requests on import.
function Get-SafeDownloadUrl([string]$Value) {
    try {
        $uri=[Uri]$Value
        $port=''; if(-not $uri.IsDefaultPort){$port=':'+$uri.Port}
        $query=''; if($uri.Query){$query='?REDACTED'}
        return $uri.Scheme+'://'+$uri.Host+$port+$uri.AbsolutePath+$query
    } catch { return '[invalid URL]' }
}
function Assert-DownloadUrl([string]$Value) {
    $uri=[Uri]$Value
    if(-not $uri.IsAbsoluteUri -or $uri.Scheme -ne 'https' -or $uri.UserInfo){throw 'Unsafe download URL; HTTPS without embedded credentials required.'}
}
function Get-AcquisitionError($ErrorObject) {
    $exc=$ErrorObject; if($ErrorObject -is [Management.Automation.ErrorRecord]){$exc=$ErrorObject.Exception}
    $chain=@(); $current=$exc
    while($current){$chain+=,$current;$current=$current.InnerException}
    foreach($e in $chain){
        if($e -is [Net.WebException]){
            if($e.Response -is [Net.HttpWebResponse]){ $code=[int]$e.Response.StatusCode; if($code -ge 500){return 'http_5xx'}; return 'http_'+$code }
            if($e.Status -in @([Net.WebExceptionStatus]::TrustFailure,[Net.WebExceptionStatus]::SecureChannelFailure)){return 'tls'}
            if($e.Status -eq [Net.WebExceptionStatus]::NameResolutionFailure){return 'dns'}
            if($e.Status -eq [Net.WebExceptionStatus]::ProxyNameResolutionFailure){return 'proxy'}
            if($e.Status -eq [Net.WebExceptionStatus]::Timeout){return 'timeout'}
        }
        if($e -is [Net.Sockets.SocketException] -and $e.ErrorCode -eq 10013){return 'network_permission'}
    }
    foreach($e in $chain){
        if($e -is [TimeoutException]){return 'timeout'}
        if($e -is [IO.InvalidDataException]){return 'integrity'}
        if($e -is [UnauthorizedAccessException]){if($exc.Data['acquisition_context'] -eq 'network'){return 'network_permission'};return 'local_permission'}
        if(($e.HResult -band 65535) -eq 5 -or ($e -is [ComponentModel.Win32Exception] -and $e.NativeErrorCode -eq 5)){if($exc.Data['acquisition_context'] -eq 'network'){return 'network_permission'};return 'local_permission'}
        if(($e.HResult -band 65535) -in @(112,39)){return 'disk_full'}
    }
    $message=($chain | ForEach-Object {$_.Message}) -join ' '
    if($message -match '(?i)time budget|deadline'){return 'deadline'}
    if($message -match '(?i)index.*(missing|not found)|exact filename'){return 'index_missing'}
    if($message -match '(?i)size|SHA256|hash mismatch|identity validation|Content-Range|registered byte|response length|Duplicate selected ZIP'){return 'integrity'}
    if($message -match '(?i)Unsafe.*URL|requires HTTPS|redirect refused'){return 'unsafe_url'}
    if($message -match '(?i)Partial lacks|Partial belongs|Unsafe partial|reparse'){return 'cache_conflict'}
    if($message -match '(?i)Incomplete response'){return 'connection'}
    if(@($chain | Where-Object {$_ -is [Net.WebException] -or $_ -is [Net.Sockets.SocketException]}).Count){return 'connection'}
    return 'local_io'
}
function Get-AcquisitionAdvice([string]$Code) {
    switch($Code){
        'dns' {return '请检查当前网络的域名解析及列出站点的可达性。'}
        'timeout' {return '请求超时，请检查当前网络或代理能否访问列出的站点，稍后重试。'}
        'connection' {return '连接失败，请检查当前网络、代理与该资源站点。'}
        'proxy' {return '代理连接失败，请检查当前进程使用的代理设置。'}
        'network_permission' {return '当前进程的网络访问被拒绝，请按宿主权限流程检查网络执行权限；不要关闭安全边界。'}
        'tls' {return '证书校验失败，请检查系统时间、证书链或组织网络设置，不关闭TLS校验。'}
        'http_403' {return '服务拒绝访问，请核对下载地址或使用可信的同版本来源。'}
        'http_404' {return '该地址未找到指定资源，请核对版本和来源地址。'}
        'http_407' {return '代理要求认证，请检查代理认证配置。'}
        'http_429' {return '服务限制请求频率，请稍后重试。'}
        'http_5xx' {return '下载服务暂时异常，请稍后重试或使用已核验的同版本原件。'}
        'integrity' {return '文件大小或SHA与固定原件不符，未采用；请提供同版本正确原件。'}
        'index_missing' {return '镜像索引未找到同版本同文件名，不改用最新版本或其他平台的包。'}
        'local_permission' {return '本地目录无法读写，请检查该工作目录的权限。'}
        'disk_full' {return '本地磁盘空间不足，请选择有足够空间的工作目录。'}
        'local_missing' {return '本地缺少当前所需原件；本次禁止联网，请提供该原件或允许联网获取。'}
        'cache_conflict' {return '缓存中已有不匹配文件或续传身份，请保留现场并改用空缓存目录。'}
        'deadline' {return '本资源已达到本轮时间预算，保留进度；可稍后从相同身份缓存续接。'}
        'unsafe_url' {return '候选地址不符合HTTPS要求，未采用，请核对可信来源。'}
        default {return '请核对记录中的资源、输入路径与当前执行环境。'}
    }
}
function Get-DownloadCandidates($Item,[string]$Channel,[string]$MirrorBase,$MirrorMap) {
    $sources=New-Object 'System.Collections.Generic.List[object]'
    function Add-Candidate([string]$Url,[string]$Kind,[string]$Role){
        if($Url -and -not @($sources | Where-Object {$_.url -eq $Url}).Count){
            Assert-DownloadUrl $Url
            $sources.Add([pscustomobject]@{url=$Url;kind=$Kind;role=$Role})
        }
    }
    $configured=@()
    if($MirrorBase){
        Assert-HttpsRoot $MirrorBase
        $encoded=(($Item.path -split '/' | ForEach-Object {[Uri]::EscapeDataString($_)}) -join '/')
        $configured+=($MirrorBase.TrimEnd('/')+'/'+$encoded)
    }
    if($MirrorMap -and $MirrorMap.PSObject.Properties[$Item.id]){$configured+=@($MirrorMap.PSObject.Properties[$Item.id].Value)}
    if($Channel -eq 'Mirror'){foreach($url in $configured){Add-Candidate $url 'file' 'configured_mirror'}}
    Add-Candidate $Item.upstream_url 'file' 'upstream'
    if($Channel -ne 'Mirror'){foreach($url in $configured){Add-Candidate $url 'file' 'configured_mirror'}}
    Add-Candidate $Item.retrieved_url 'file' 'recorded_transport'
    $uri=[Uri]$Item.upstream_url
    if($uri.Host -eq 'github.com' -and $uri.AbsolutePath -match '^/[^/]+/[^/]+/releases/download/'){
        Add-Candidate ('https://dl.espressif.com/github_assets'+$uri.AbsolutePath) 'file' 'official_assets_mirror'
        Add-Candidate ('https://dl.espressif.cn/github_assets'+$uri.AbsolutePath) 'file' 'official_assets_alternative'
    }
    if($uri.Host -eq 'components-file.espressif.com'){
        Add-Candidate ('https://components-file.espressif.cn'+$uri.AbsolutePath) 'file' 'official_components_mirror'
        Add-Candidate ('https://components-file.espressif.com.cn'+$uri.AbsolutePath) 'file' 'official_components_alternative'
    }
    if($uri.Host -eq 'files.pythonhosted.org'){
        $package=([string]$Item.package_name).ToLowerInvariant() -replace '[-_.]+','-'
        if($package -notmatch '^[a-z0-9][a-z0-9-]*$'){throw 'Invalid Python package identity for index lookup.'}
        Add-Candidate ('https://mirrors.tuna.tsinghua.edu.cn/pypi/web/simple/'+$package+'/') 'index' 'tuna_index'
        Add-Candidate ('https://mirrors.aliyun.com/pypi/simple/'+$package+'/') 'index' 'aliyun_index'
    }
    return @($sources | Select-Object -First 4)
}
function Resolve-ExactIndex([string]$Url,$Item,[int]$TimeoutSeconds,[double]$RemainingSeconds) {
    Assert-DownloadUrl $Url
    $response=$null; $input=$null; $memory=New-Object IO.MemoryStream
    $clock=[Diagnostics.Stopwatch]::StartNew()
    try {
        [Net.ServicePointManager]::SecurityProtocol=[Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $request=[Net.HttpWebRequest]::Create($Url)
        $wait=[Math]::Max(1,[Math]::Min($TimeoutSeconds,$RemainingSeconds))
        $request.Timeout=[int]($wait*1000);$request.ReadWriteTimeout=[int]($wait*1000)
        try {$response=$request.GetResponse()} catch {$_.Exception.Data['acquisition_context']='network';throw}
        if($response.ResponseUri.Scheme -ne 'https' -or $response.ResponseUri.Host -ne ([Uri]$Url).Host){throw 'Insecure or foreign index redirect refused.'}
        $input=$response.GetResponseStream();$buffer=New-Object byte[] 16384
        while($true){
            if($clock.Elapsed.TotalSeconds -ge $RemainingSeconds){throw 'Index deadline reached.'}
            try {$n=$input.Read($buffer,0,$buffer.Length)} catch {$_.Exception.Data['acquisition_context']='network';throw}
            if($n -eq 0){break}
            if($memory.Length+$n -gt 2097152){throw 'Index exceeds size limit.'}
            $memory.Write($buffer,0,$n)
        }
        $html=[Text.Encoding]::UTF8.GetString($memory.ToArray())
        foreach($match in [regex]::Matches($html,'(?i)href\s*=\s*["'']([^"'']+)["'']')){
            $href=[Net.WebUtility]::HtmlDecode($match.Groups[1].Value)
            $candidate=New-Object Uri($response.ResponseUri,$href)
            if($candidate.Scheme -ne 'https' -or $candidate.Host -ne $response.ResponseUri.Host -or $candidate.UserInfo){continue}
            if([Uri]::UnescapeDataString(($candidate.AbsolutePath -split '/')[-1]) -cne $Item.filename){continue}
            $b=New-Object UriBuilder($candidate);$b.Fragment='';return $b.Uri.AbsoluteUri
        }
        throw 'Exact filename not found in mirror index.'
    } finally {if($input){$input.Dispose()};if($response){$response.Dispose()};$memory.Dispose()}
}
function Invoke-PlannedDownload($Item,[string]$Target,[string]$Channel,[string]$MirrorBase,$MirrorMap,[int]$Attempts,[int]$TimeoutSeconds,[int]$DeadlineSeconds) {
    $history=New-Object 'System.Collections.Generic.List[object]'
    $clock=[Diagnostics.Stopwatch]::StartNew();$last='no_source';$ok=$false
    $transient=@('dns','timeout','connection','http_408','http_429','http_5xx')
    $stop=@('network_permission','local_permission','disk_full','cache_conflict','deadline')
    :sources foreach($source in @(Get-DownloadCandidates $Item $Channel $MirrorBase $MirrorMap)){
        for($attempt=1;$attempt -le $Attempts;$attempt++){
            $entry=[ordered]@{channel='https';url=(Get-SafeDownloadUrl $source.url);host=([Uri]$source.url).Host;role=$source.role;attempt=$attempt}
            $tick=[Diagnostics.Stopwatch]::StartNew()
            try {
                $left=$DeadlineSeconds-$clock.Elapsed.TotalSeconds
                if($left -le 0){throw 'Resource deadline reached.'}
                $url=$source.url
                if($source.kind -eq 'index'){$url=Resolve-ExactIndex $url $Item $TimeoutSeconds $left}
                $left=$DeadlineSeconds-$clock.Elapsed.TotalSeconds
                if($left -le 0){throw 'Resource deadline reached.'}
                $entry.file_url=Get-SafeDownloadUrl $url
                Receive-FrozenFile $url $Target $Item.bytes $Item.sha256 ([Math]::Max(1,[Math]::Min($TimeoutSeconds,[Math]::Floor($left)))) ([Math]::Max(1,[Math]::Floor($left)))
                $entry.status='verified';$ok=$true
            } catch {$last=Get-AcquisitionError $_;$entry.error_code=$last}
            $entry.elapsed_seconds=[Math]::Round($tick.Elapsed.TotalSeconds,3);$history.Add([pscustomobject]$entry)
            if($ok){break sources}
            if($last -in $stop){break sources}
            if($last -notin $transient){break}
        }
    }
    return [pscustomobject]@{ok=$ok;error_code=$last;attempts=$history.ToArray();elapsed_seconds=[Math]::Round($clock.Elapsed.TotalSeconds,3)}
}
