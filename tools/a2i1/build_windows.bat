@echo off
setlocal
cd /d "%~dp0"

set PY=python
where py >nul 2>&1 && set PY=py -3

echo [1/2] Ensure deps...
%PY% -m pip install -r requirements.txt -q
if errorlevel 1 exit /b 1

echo [2/2] Build A2I1_GUI.exe ...
%PY% -m PyInstaller --noconfirm --clean ^
  --onefile --windowed ^
  --name A2I1_GUI ^
  --distpath . ^
  --workpath build ^
  --specpath build ^
  --hidden-import a2i1_core ^
  a2i1_gui.py

if errorlevel 1 exit /b 1
echo.
echo Done: %~dp0A2I1_GUI.exe
endlocal
