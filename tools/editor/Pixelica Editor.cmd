@echo off
setlocal
cd /d "%~dp0"
python -c "import sys, tkinter, PIL; assert sys.version_info >= (3, 10)" >nul 2>nul
if not errorlevel 1 (
  python launch_editor.py %*
) else (
  py -3 launch_editor.py %*
)
set "EDITOR_EXIT=%ERRORLEVEL%"
if not "%EDITOR_EXIT%"=="0" pause
exit /b %EDITOR_EXIT%
