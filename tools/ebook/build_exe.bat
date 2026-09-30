@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo === ebook-web build (Windows exe) ===
echo Workdir: %CD%

echo Syncing fonts into web\static\fonts ...
if not exist "web\static\fonts\ef" mkdir "web\static\fonts\ef"
if exist "..\epdfont\MiSans-Light\*.ef" (
  copy /Y "..\epdfont\MiSans-Light\*.ef" "web\static\fonts\ef\" >nul
) else (
  echo WARN: ..\epdfont\MiSans-Light\*.ef not found
)
if exist "..\fontpack\fonts\MiSans-Mixed\fonts_misans_25_30.fontpack" (
  copy /Y "..\fontpack\fonts\MiSans-Mixed\fonts_misans_25_30.fontpack" "web\static\fonts\" >nul
) else (
  echo WARN: fontpack not found
)

where py >nul 2>&1
if errorlevel 1 (
  echo ERROR: py.exe not found. Install Python 3.10+ first.
  exit /b 1
)

if not exist ".venv-win\Scripts\python.exe" (
  echo Creating venv .venv-win ...
  py -3 -m venv .venv-win
  if errorlevel 1 exit /b 1
)

echo Installing deps ...
".venv-win\Scripts\python.exe" -m pip install -U pip
if errorlevel 1 exit /b 1
".venv-win\Scripts\python.exe" -m pip install -r requirements.txt pyinstaller
if errorlevel 1 exit /b 1

echo Running PyInstaller ...
".venv-win\Scripts\pyinstaller.exe" --noconfirm --clean ebook_web.spec
if errorlevel 1 exit /b 1

echo.
echo DONE: %CD%\dist\ebook-web.exe
echo Double-click the exe to start server and open browser.
echo Close the console window to stop the server.
echo Library data is saved under data\ next to the exe.
exit /b 0
