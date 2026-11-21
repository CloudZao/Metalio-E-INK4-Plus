param(
    [ValidateSet('Auto','Local','Official','Mirror')][string]$Channel='Auto',
    [Parameter(Mandatory=$true)][string]$Cache,
    [string]$Source,[string]$MirrorBase,[string]$DeviceRoot,[string]$ContextFile,
    [ValidateSet('plus-base','official','debug','full')][string]$Profile='plus-base',
    [ValidateSet('install','profile','python-bootstrap','git-bootstrap','sdk','tools','python-core','extra')][string]$Stage='profile',
    [string[]]$Id=@(),[string]$Reason,[string]$PlanFile,[string]$MirrorMap,[string]$ReportPath,[switch]$Offline,
    [ValidateRange(1,2)][int]$Attempts=2,
    [ValidateRange(5,120)][int]$TimeoutSeconds=30,
    [ValidateRange(30,7200)][int]$DeadlineSeconds=900
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Material-Common.ps1')
. (Join-Path $PSScriptRoot 'Download-Policy.ps1')
$registry=Read-FrozenRegistry (Join-Path $script:ModuleRoot 'references\registry.json')
if($PlanFile){
    if($PSBoundParameters.ContainsKey('Stage') -or $Id.Count -or $Reason){throw 'PlanFile cannot be combined with Stage, Id or Reason.'}
    $plan=Get-Content -LiteralPath $PlanFile -Raw -Encoding UTF8 | ConvertFrom-Json
} else {
    if($Id.Count -and -not $PSBoundParameters.ContainsKey('Stage')){$Stage='extra'}
    $plan=& (Join-Path $PSScriptRoot 'Plan-Materials.ps1') -Stage $Stage -Profile $Profile -Id $Id -Reason $Reason
}
if($plan.schema -ne 1 -or $plan.registry_sha256 -ne $script:RegistrySHA -or $plan.target -ne 'esp32s31'){throw 'Plan does not match the frozen Plus registry.'}
$ids=@($plan.selected_ids)
if(@($ids | Select-Object -Unique).Count -ne $ids.Count){throw 'Duplicate resource IDs in plan.'}
$items=@($registry.items | Where-Object {$_.id -in $ids})
if($items.Count -ne $ids.Count){throw 'Plan contains unknown resource IDs.'}
function Assert-AcquisitionWritePath([string]$Value){
    foreach($path in @([IO.Path]::GetFullPath($Value),(Get-ResolvedMaterialPath $Value))){
        if(($path -split '[\\/]') -contains '02_测试预置_原始材料'){throw 'Cache and report must remain outside 02_测试预置_原始材料; originals are read-only inputs.'}
    }
}
$cachePath=[IO.Path]::GetFullPath($Cache)
Assert-Separate $cachePath $script:SkillRoot
Assert-AcquisitionWritePath $cachePath
$destRegistry=Join-Path $cachePath 'registry.json'
if(Test-Path -LiteralPath $destRegistry){$null=Read-FrozenRegistry $destRegistry}
# An install-only plan intentionally does not resolve missing material contexts or network channels.
$selection=$null;$strict=[bool]$Offline;$strictLocal=$false
if($items.Count){
    $selection=& (Join-Path $PSScriptRoot 'Resolve-Materials.ps1') -Channel $Channel -Source $Source -DeviceRoot $DeviceRoot -MirrorBase $MirrorBase -MirrorMap $MirrorMap -ContextFile $ContextFile
    $strictLocal=[bool]$selection.strict_local
    $strict=$strict -or $strictLocal
    $Channel=$selection.channel;$Source=$selection.source;$MirrorBase=$selection.mirror_base;$MirrorMap=$selection.mirror_map
    if($Source){Assert-Separate $Source $cachePath}
}
if(-not $ReportPath){$ReportPath=Join-Path $cachePath ('acquisition-'+[Guid]::NewGuid().ToString('N')+'.json')}
$reportPathFull=[IO.Path]::GetFullPath($ReportPath)
Assert-AcquisitionWritePath $reportPathFull
if(Test-ContainsPath $script:SkillRoot $reportPathFull){throw 'Report must be outside Skill.'}
if($Source -and (Test-ContainsPath $Source $reportPathFull)){throw 'Report must be outside raw materials.'}
if(Test-Path -LiteralPath $reportPathFull){throw 'Report already exists; choose a new path to preserve evidence.'}
$mirrors=$null
if($MirrorMap){
    $mirrors=Get-Content -LiteralPath $MirrorMap -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach($property in $mirrors.PSObject.Properties){
        if($property.Name -notin @($registry.items.id) -or $property.Value -is [string]){throw 'MirrorMap must map known IDs to arrays of exact HTTPS file URLs.'}
        if(@($property.Value).Count -gt 2){throw 'At most two configured mirror URLs per resource.'}
        foreach($url in @($property.Value)){Assert-DownloadUrl $url}
    }
}
New-Item -ItemType Directory -Path $cachePath -Force | Out-Null
$report=[ordered]@{schema=1;registry_sha256=$script:RegistrySHA;target='esp32s31';status='ready';stage='resource_acquisition';installation_performed=$false;selected_count=$items.Count;items=@();failures=@();not_attempted=@();verified_download_bytes=0L;byte_measurement='Complete size of originals acquired by HTTPS, including previously resumed bytes; not actual network traffic. Indexes, failed transfers and protocol overhead are excluded.';report_path=$reportPathFull}
$started=[Diagnostics.Stopwatch]::StartNew()
$snapshotDoc=Get-Content -LiteralPath (Join-Path $script:ModuleRoot 'assets\snapshot-provenance.json') -Raw -Encoding UTF8 | ConvertFrom-Json

function Copy-SelectedOriginal($Item,[string]$Target,[string]$MaterialSource){
    if(-not $MaterialSource -or -not (Test-Path -LiteralPath $MaterialSource)){return $false}
    $src=Get-Item -LiteralPath $MaterialSource
    if($src.PSIsContainer){
        foreach($relative in @($Item.path,('PlusRawMirror/'+$Item.path),$Item.filename)){
            $candidate=Get-SafePath $MaterialSource $relative
            if(Test-Path -LiteralPath $candidate -PathType Leaf){
                Assert-FileIdentity $candidate $Item.bytes $Item.sha256
                $temporary=$Target+'.local-'+[Guid]::NewGuid().ToString('N')
                try {Copy-Item -LiteralPath $candidate -Destination $temporary;Assert-FileIdentity $temporary $Item.bytes $Item.sha256;[IO.File]::Move($temporary,$Target)} finally {if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary}}
                return $true
            }
        }
        $archives=@(Get-ChildItem -LiteralPath $MaterialSource -File -Filter '*.zip')
    } elseif($src.Name -eq $Item.filename){
        Assert-FileIdentity $src.FullName $Item.bytes $Item.sha256
        $temporary=$Target+'.local-'+[Guid]::NewGuid().ToString('N')
        try{Copy-Item -LiteralPath $src.FullName -Destination $temporary;Assert-FileIdentity $temporary $Item.bytes $Item.sha256;[IO.File]::Move($temporary,$Target)} finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary}}
        return $true
    } else {$archives=@($src)}
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach($archive in $archives){
        $zip=[IO.Compression.ZipFile]::OpenRead($archive.FullName)
        try{
            $members=@($zip.Entries | Where-Object {$_.FullName -eq $Item.zip_entry -or $_.FullName -eq $Item.path})
            if($members.Count -gt 1){throw 'Duplicate selected ZIP member.'}
            if(-not $members.Count){continue}
            if($members[0].Length -ne $Item.bytes){throw 'ZIP member size mismatch.'}
            $temporary=$Target+'.local-'+[Guid]::NewGuid().ToString('N')
            try {
                $incoming=$members[0].Open()
                try{$out=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write);try{$incoming.CopyTo($out)}finally{$out.Dispose()}}finally{$incoming.Dispose()}
                Assert-FileIdentity $temporary $Item.bytes $Item.sha256;[IO.File]::Move($temporary,$Target)
            }finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary}}
            return $true
        }finally{$zip.Dispose()}
    }
    return $false
}

foreach($item in $items){
    $reasons=@();$planned=@($plan.selected | Where-Object {$_.id -eq $item.id} | Select-Object -First 1)
    if($planned.Count){$reasons=@($planned[0].reasons | Select-Object -First 4 | ForEach-Object {$clean=[string]$_ -replace '[\r\n]',' ';$clean.Substring(0,[Math]::Min(240,$clean.Length))})}
    $row=[ordered]@{id=$item.id;filename=$item.filename;version=$item.version;bytes=$item.bytes;sha256=$item.sha256;upstream_url=(Get-SafeDownloadUrl $item.upstream_url);reasons=$reasons;status='pending';attempts=@()}
    try{
        $target=Get-SafePath $cachePath $item.path
        if(Test-Path -LiteralPath $target){
            try{Assert-FileIdentity $target $item.bytes $item.sha256}catch{throw 'Existing cache identity mismatch; retained. Partial belongs to another material identity.'}
            $row.status='verified';$row.channel='destination_cache'
        } else {
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            if($Source){
                try{
                    if(Copy-SelectedOriginal $item $target $Source){$row.status='verified';$row.channel='local';$row.attempts+=@{channel='local';status='verified';path=$Source}}
                }catch{
                    $code=Get-AcquisitionError $_;$row.attempts+=@{channel='local';error_code=$code;path=$Source}
                    if($strict -or $code -in @('integrity','local_permission','disk_full','cache_conflict')){throw}
                }
            }
            if($row.status -ne 'verified' -and -not $strictLocal){
                $snapshot=@($snapshotDoc.items | Where-Object {$_.id -eq $item.id})
                if($snapshot.Count -eq 1){
                    $asset=Get-SafePath (Join-Path $script:ModuleRoot 'assets') $snapshot[0].asset
                    Assert-FileIdentity $asset $item.bytes $item.sha256
                    Copy-Item -LiteralPath $asset -Destination $target;Assert-FileIdentity $target $item.bytes $item.sha256
                    $row.status='verified';$row.channel='bundled_metadata'
                }
            }
            if($row.status -ne 'verified'){
                if($strict){$row.error_code='local_missing';throw 'Required local original is missing.'}
                $result=Invoke-PlannedDownload $item $target $Channel $MirrorBase $mirrors $Attempts $TimeoutSeconds $DeadlineSeconds
                $row.attempts+=@($result.attempts)
                if(-not $result.ok){$row.error_code=$result.error_code;throw 'Necessary resource could not be acquired.'}
                $row.status='verified';$row.channel='https';$report.verified_download_bytes+=$item.bytes
            }
        }
        $row.path=$target
    }catch{
        if(-not $row.Contains('error_code')){$row.error_code=Get-AcquisitionError $_}
        $row.status='unavailable';$row.action=Get-AcquisitionAdvice $row.error_code
    }
    $report.items+=,[pscustomobject]$row
    if($row.status -ne 'verified'){
        $report.status='blocked';$report.failures+=,[pscustomobject]$row
        $report.not_attempted=@($items | Select-Object -Skip $report.items.Count | ForEach-Object {$_.id})
        break
    }
    Write-Host ('Verified selected original: '+$item.filename)
}
if($report.status -eq 'ready' -and $items.Count){
    $destRegistry=Join-Path $cachePath 'registry.json'
    if(Test-Path -LiteralPath $destRegistry){$null=Read-FrozenRegistry $destRegistry}else{Copy-Item -LiteralPath (Join-Path $script:ModuleRoot 'references\registry.json') -Destination $destRegistry}
}
$report.elapsed_seconds=[Math]::Round($started.Elapsed.TotalSeconds,3)
$report.summary=if($report.status -eq 'ready'){'所选原件已核验；尚未安装工具或构建固件。'}else{'必要资源获取受阻，未尝试的条目不代表下载失败；详见失败资源及处理建议。'}
New-Item -ItemType Directory -Path (Split-Path $reportPathFull -Parent) -Force | Out-Null
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPathFull -Encoding UTF8
Write-Host ('Acquisition report: '+$reportPathFull)
if($report.status -ne 'ready'){
    $f=$report.failures[0]
    throw ('资源未取得：{0}；原因：{1}；{2} 详情：{3}' -f $f.filename,$f.error_code,$f.action,$reportPathFull)
}
Write-Output $cachePath
