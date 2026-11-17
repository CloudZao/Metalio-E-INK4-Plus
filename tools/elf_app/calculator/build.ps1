param([Parameter(Mandatory=$true)][string]$Compiler)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot '..\build_app.ps1') -Compiler $Compiler -AppDirectory $PSScriptRoot
exit $LASTEXITCODE
