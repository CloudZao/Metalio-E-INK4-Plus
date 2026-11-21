param(
    [Parameter(Mandatory=$true)][string]$Mirror,
    [Parameter(Mandatory=$true)][string]$Destination,
    [ValidateSet('plus-base','official','debug','full')][string]$Profile = 'plus-base',
    [string]$BasePython
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Material-Common.ps1')
$mirrorPath = [IO.Path]::GetFullPath($Mirror)
$destinationPath = [IO.Path]::GetFullPath($Destination)
Assert-Separate $mirrorPath $destinationPath
Assert-Separate $script:SkillRoot $destinationPath
if ($destinationPath.Length -gt 40 -or $destinationPath -match '[^\x21-\x7e]') {
    throw ('Environment path must be ASCII without spaces and <=40 characters; actual length=' + $destinationPath.Length + '. Choose a writable short path.')
}
$profilePath = Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\environment-profile.json'
$environmentProfile = Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json
$registry = Read-FrozenRegistry (Join-Path $mirrorPath 'registry.json')
# Reject incomplete local materials before any installer is launched.
Assert-RawMaterials $mirrorPath $registry $Profile
if ($BasePython) {
    $python = [IO.Path]::GetFullPath($BasePython)
    if (-not (Test-Path -LiteralPath $python -PathType Leaf)) { throw 'BasePython does not exist.' }
} else {
    $pythonDir = Join-Path $destinationPath 'python'
    $python = Join-Path $pythonDir 'python.exe'
    if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
        $installerItems = @($registry.items | Where-Object {
            $_.filename -eq $environmentProfile.python_installer_filename -and $_.sha256 -eq $environmentProfile.python_installer_sha256
        })
        if ($installerItems.Count -ne 1) { throw 'The registry must contain exactly one frozen CPython installer.' }
        $relative = $installerItems[0].path
        if ($relative -match '(^/|\\|:|(^|/)\.\.(/|$))') { throw 'Unsafe installer registry path.' }
        $installer = [IO.Path]::GetFullPath((Join-Path $mirrorPath $relative))
        if (-not $installer.StartsWith($mirrorPath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Installer escapes the input mirror.'
        }
        $file = Get-Item -LiteralPath $installer
        if ($file.Length -ne $installerItems[0].bytes -or
            (Get-MaterialSHA256 $installer) -ne $environmentProfile.python_installer_sha256) {
            throw 'CPython installer size or SHA256 does not match the frozen profile.'
        }
        New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
        $installLog = Join-Path $destinationPath 'python-install.log'
        $arguments = @('/quiet', '/log', ('"' + $installLog + '"'), 'InstallAllUsers=0', ('TargetDir="' + $pythonDir + '"'),
            'Include_launcher=0', 'InstallLauncherAllUsers=0', 'AssociateFiles=0',
            'Shortcuts=0', 'PrependPath=0', 'AppendPath=0', 'Include_test=0', 'Include_doc=0',
            'Include_debug=0', 'Include_symbols=0', 'Include_dev=0')
        $process = Start-Process -FilePath $installer -ArgumentList $arguments -PassThru -Wait -WindowStyle Hidden
        if ($process.ExitCode -notin @(0,3010)) { throw "CPython installer exit: $($process.ExitCode); retained log: $installLog" }
    }
}
& $python -I -c "import os,sys; assert os.name=='nt' and sys.version_info[:3]==(3,13,17) and sys.maxsize>2**32, sys.version"
if ($LASTEXITCODE -ne 0) { throw 'Expected standard Windows x64 CPython 3.13.17.' }
& $python -I (Join-Path $PSScriptRoot 'toolchain_ops.py') prepare --mirror $mirrorPath --destination $destinationPath --profile $Profile
if ($LASTEXITCODE -ne 0) { throw "Offline preparation failed: $LASTEXITCODE" }
Write-Host ('Environment prepared. Activate in each build terminal: . "' + $destinationPath + '\Activate.ps1"')
