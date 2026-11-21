param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Material-Common.ps1')
$root = [IO.Path]::GetFullPath($Destination)
$profilePath = Join-Path $script:ModuleRoot 'assets\environment-profile.json'
$profile = Get-Content -LiteralPath $profilePath -Raw -Encoding UTF8 | ConvertFrom-Json
$receipt = Get-Content -LiteralPath (Join-Path $root 'environment.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($receipt.status -ne 'ENVIRONMENT_PREPARED' -or $receipt.sdk_commit -ne $profile.idf_commit -or
    $receipt.raw_registry_sha256 -ne $script:RegistrySHA -or
    $receipt.operations_profile_sha256 -ne (Get-MaterialSHA256 $profilePath)) {
    throw 'Existing environment identity does not match this Skill profile.'
}
if (-not (Test-ContainsPath $root $receipt.values.IDF_PATH) -or
    -not (Test-ContainsPath $root $receipt.values.IDF_TOOLS_PATH) -or
    -not (Test-ContainsPath $root $receipt.values.IDF_PYTHON_ENV_PATH)) { throw 'Environment was moved or paths are inconsistent; prepare at its final location.' }
# New profile receipts must bind the exact selection and installed SDK policy.
if ($receipt.material_profile) {
    $catalog = Join-Path $script:ModuleRoot 'assets\acquisition-profiles.json'
    if ($receipt.acquisition_profile_sha256 -ne (Get-MaterialSHA256 $catalog)) { throw 'Acquisition profile changed since installation.' }
    $selected = Get-AcquisitionProfile $receipt.material_profile
    $installedNames = @($receipt.sdk_tools | ForEach-Object { ($_ -split '@')[0] } | Sort-Object)
    if (($installedNames -join '|') -ne (($selected.tools | Sort-Object) -join '|')) { throw 'Installed tool set differs from material profile.' }
    $sdkReceipt = Get-Content -LiteralPath (Join-Path $root 'sdk-source-receipt.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($sdkReceipt.material_profile -ne $receipt.material_profile) { throw 'SDK policy profile mismatch.' }
    foreach ($entry in $sdkReceipt.critical_files.PSObject.Properties) {
        if ((Get-MaterialSHA256 (Get-SafePath $receipt.values.IDF_PATH $entry.Name)) -ne $entry.Value) { throw ('Installed SDK critical file changed: ' + $entry.Name) }
    }
}
# Run this script in a child PowerShell when only inspecting a candidate runtime.
. (Join-Path $root 'Activate.ps1')
$python = Join-Path $receipt.values.IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python -PathType Leaf)) { throw 'Runtime Python is missing; rebuild a new private environment, do not assume reuse.' }
$ErrorActionPreference = 'Continue'
& $python -I -c "import os,sys; assert os.name=='nt' and sys.version_info[:3]==(3,13,17) and sys.maxsize>2**32; print(sys.version)"
if ($LASTEXITCODE -ne 0) { throw 'Runtime Python cannot execute or is incompatible (including a missing base interpreter). Rebuild in a new private environment; do not overwrite the old runtime silently.' }
& $python (Join-Path $receipt.values.IDF_PATH 'tools\idf.py') --version
if ($LASTEXITCODE -ne 0) { throw 'SDK cannot run.' }
& $python -m pip check
if ($LASTEXITCODE -ne 0) { throw 'Python dependencies are inconsistent.' }
Write-Output 'ENVIRONMENT_REUSE_CHECK_PASSED (identity and command smoke check; not a full file audit or firmware build)'
