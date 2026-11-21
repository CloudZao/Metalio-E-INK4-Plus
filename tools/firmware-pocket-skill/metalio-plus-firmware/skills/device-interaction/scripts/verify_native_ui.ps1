param(
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Python='python',
    [string]$VsDevCmd
)
$ErrorActionPreference='Stop'
$skill=Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$font=Join-Path $skill 'skills\font-support'
$main=Join-Path $skill 'skills\firmware-build\assets\plus-app-base\main'
$Output=[IO.Path]::GetFullPath($Output)
$skillFull=[IO.Path]::GetFullPath($skill).TrimEnd('\')+'\'
if(($Output.TrimEnd('\')+'\').StartsWith($skillFull,[StringComparison]::OrdinalIgnoreCase)){throw 'Write validation output outside the installed Skill.'}
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$generated=Join-Path $Output 'font'
if(Test-Path -LiteralPath $generated){throw 'Use a fresh validation output directory; existing font output is preserved.'}
& $Python -B (Join-Path $font 'scripts\prepare_font.py') --size 32 --weight 500 --output $generated
if($LASTEXITCODE -ne 0){throw 'Font preparation failed'}
if(-not $VsDevCmd){$VsDevCmd=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'}
if(-not (Test-Path -LiteralPath $VsDevCmd)){throw 'Set -VsDevCmd to an existing native Windows MSVC environment. Do not install WSL.'}
$sources=@((Join-Path $font 'components\plus_font\plus_font.cc'),(Join-Path $generated 'font.cc'),(Join-Path (Split-Path $PSScriptRoot -Parent) 'examples\verify_ui.cpp'))
$includes=@((Join-Path $font 'components\plus_font\include'),$generated,$main)
$exe=Join-Path $Output 'verify-ui.exe'
# A .cmd only initializes the existing compiler and starts it; file operations
# stay in PowerShell. Reject cmd expansion characters in all external paths.
foreach($p in @($VsDevCmd,$Output,$exe)+$sources+$includes){if($p -match '["%\r\n!]'){throw 'Unsupported command-shell character in compiler path.'}}
$args=($includes|ForEach-Object {'/I"'+$_+'"'})+($sources|ForEach-Object {'"'+$_+'"'})
$batch="@echo off`r`nchcp 65001 >nul`r`ncall `"$VsDevCmd`" -arch=x64 >nul`r`nif errorlevel 1 exit /b 1`r`ncl /nologo /EHsc /std:c++14 /utf-8 /W4 /WX $($args -join ' ') /Fo`"$Output\\`" /Fe`"$exe`"`r`nexit /b %errorlevel%`r`n"
$cmdFile=Join-Path $Output 'compile-native.cmd'
[IO.File]::WriteAllText($cmdFile,$batch,[Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /c $cmdFile
if($LASTEXITCODE -ne 0){throw 'Native compiler failed'}
& $exe $Output
if($LASTEXITCODE -ne 0){throw 'Production UI checks failed'}
& $Python -B (Join-Path $PSScriptRoot 'preview_native_frame.py') (Join-Path $Output 'pocket.gray4') (Join-Path $Output 'pocket.pgm')
if($LASTEXITCODE -ne 0){throw 'Actual frame decode failed'}
Write-Output 'PASS_WINDOWS_NATIVE_UI; host pixels/state only; no device operations.'
