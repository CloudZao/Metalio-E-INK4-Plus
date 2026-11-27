param(
    [Parameter(Mandatory=$true)][string]$Compiler,
    [Parameter(Mandatory=$true)][string]$AppDirectory,
    [ValidateSet('esp32s31','esp32s3')][string]$Target = 'esp32s31',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$appPath = (Resolve-Path -LiteralPath $AppDirectory).Path
$name = Split-Path -Leaf $appPath
$sdk = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\main\business\app_store')).Path
$common = Join-Path $PSScriptRoot 'common'
$source = Join-Path $appPath "$name.c"
if (!$OutputDirectory -and $Target -eq 'esp32s3') {
    $OutputDirectory = Join-Path $appPath 'build/esp32s3'
}
$outputPath = if ($OutputDirectory) {
    [IO.Directory]::CreateDirectory($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)).FullName
} else { $appPath }
$output = Join-Path $outputPath "$name.elf"
$compilerDir = Split-Path -Parent $Compiler
$toolPrefix = (Split-Path -Leaf $Compiler) -replace 'gcc(?:\.exe)?$', ''
$machine = & $Compiler -dumpmachine
if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
if ($Target -eq 'esp32s3') {
    if ($machine -notlike 'xtensa*') { throw 'ESP32-S3 requires the Xtensa ESP32-S3 compiler.' }
    $architectureFlags = @('-mlongcalls', '-mtext-section-literals')
} else {
    if ($machine -notlike 'riscv32*') { throw 'ESP32-S31 requires the RISC-V compiler.' }
    $architectureFlags = @('-mabi=ilp32f', '-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv')
}
$prepare = Join-Path $appPath 'build_content.py'
if (Test-Path -LiteralPath $prepare) {
    & python $prepare
    if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
}
& $Compiler @architectureFlags -nostdlib -nostartfiles -ffreestanding -fPIC -shared -fdata-sections -ffunction-sections '-Wl,--gc-sections,-e,app_main,--strip-all' -fvisibility=hidden -Os "-I$sdk" "-I$common" $source -o $output
if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
$readelf = Join-Path $compilerDir ($toolPrefix + 'readelf.exe')
$allowed = @(Get-Content -LiteralPath (Join-Path $sdk 'app_runtime_math.def') | ForEach-Object {
    if ($_ -match '^XZ_APP_RUNTIME_MATH\((\w+),') { $Matches[1] }
})
$symbols = & $readelf --dyn-syms --wide $output
if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
foreach ($line in $symbols) {
    if ($line -match '\bUND\s+(\S+)\s*$' -and $Matches[1] -notin $allowed) {
        Remove-Item -LiteralPath $output -Force
        throw "Unsupported runtime symbol: $($Matches[1])"
    }
}
$icon = [IO.Path]::GetTempFileName()
try {
    & python (Join-Path $PSScriptRoot 'build_icon.py') (Join-Path $appPath 'icon.pbm') $icon
    if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
    $objcopy = Join-Path $compilerDir ($toolPrefix + 'objcopy.exe')
    & $objcopy --add-section ".xz.app.icon=$icon" --set-section-flags '.xz.app.icon=readonly,contents' $output
    if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
} finally { Remove-Item -LiteralPath $icon -Force }
$assets = Join-Path $appPath 'assets.bin'
if (Test-Path -LiteralPath $assets) {
    & $objcopy --add-section ".xz.app.assets=$assets" --set-section-flags '.xz.app.assets=readonly,contents' $output
    if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
}
Get-Item -LiteralPath $output | Select-Object Name,Length
