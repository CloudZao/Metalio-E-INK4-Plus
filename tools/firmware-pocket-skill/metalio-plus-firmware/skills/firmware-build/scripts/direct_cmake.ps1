param(
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$Python,
    [ValidateSet('Build','Reconfigure')][string]$Action='Build'
)
$ErrorActionPreference='Continue'
if ($env:IDF_TARGET -ne 'esp32s31') { throw 'Expected the activated S31 environment.' }
$cmakeExecutable=(Get-Command cmake -CommandType Application -ErrorAction Stop).Source
# CMake keeps the short supplied spelling. idf.py resolves SUBST/junctions.
# Use an independent build directory when switching route; never transplant cache.
$LASTEXITCODE=$null
& $cmakeExecutable -S $Project -B $BuildDirectory -G Ninja "-DPYTHON=$Python" -DESP_PLATFORM=1 -DIDF_TARGET=esp32s31 -DCCACHE_ENABLE=0
if ($null -eq $LASTEXITCODE) { throw 'CMake did not produce an exit code.' }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Action -eq 'Build') {
    $LASTEXITCODE=$null
    & $cmakeExecutable --build $BuildDirectory -- -j 2
    if ($null -eq $LASTEXITCODE) { throw 'CMake build did not produce an exit code.' }
    exit $LASTEXITCODE
}
exit 0
