param(
    [Parameter(Mandatory=$true)][string]$Compiler,
    [Parameter(Mandatory=$true)][string]$AppDirectory
)
$ErrorActionPreference = 'Stop'
$appPath = (Resolve-Path -LiteralPath $AppDirectory).Path
$name = Split-Path -Leaf $appPath
$sdk = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\main\app_store')).Path
$common = Join-Path $PSScriptRoot 'common'
$source = Join-Path $appPath "$name.c"
$output = Join-Path $appPath "$name.elf"
$prepare = Join-Path $appPath 'build_content.py'
if (Test-Path -LiteralPath $prepare) {
    & python $prepare
    if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
}
& $Compiler -mabi=ilp32f -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv -nostdlib -nostartfiles -ffreestanding -fPIC -shared -fdata-sections -ffunction-sections '-Wl,--gc-sections,-e,app_main,--strip-all' -fvisibility=hidden -Os "-I$sdk" "-I$common" $source -o $output
if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
$compilerDir = Split-Path -Parent $Compiler
$readelf = Join-Path $compilerDir 'riscv32-esp-elf-readelf.exe'
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
    $objcopy = Join-Path (Split-Path -Parent $Compiler) 'riscv32-esp-elf-objcopy.exe'
    & $objcopy --add-section ".xz.app.icon=$icon" --set-section-flags '.xz.app.icon=readonly,contents' $output
    if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
} finally { Remove-Item -LiteralPath $icon -Force }
$assets = Join-Path $appPath 'assets.bin'
if (Test-Path -LiteralPath $assets) {
    & $objcopy --add-section ".xz.app.assets=$assets" --set-section-flags '.xz.app.assets=readonly,contents' $output
    if ($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
}
Get-Item -LiteralPath $output | Select-Object Name,Length
