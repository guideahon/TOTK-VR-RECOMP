@echo off
rem TOTK VR Recomp: the graphical installer (tools/vr/installer.py).
cd /d "%~dp0"
if exist .venv\Scripts\pythonw.exe (start "" .venv\Scripts\pythonw.exe tools\vr\installer.py & exit /b)
where pyw >nul 2>nul && (start "" pyw -3 tools\vr\installer.py & exit /b)
echo Python 3 is needed: winget install Python.Python.3.12
pause
