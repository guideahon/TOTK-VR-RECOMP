@echo off
rem TOTK VR Recomp: start the game in VR (docs/vr-setup.md).
rem First: start the headset's OpenXR runtime (Virtual Desktop connected with VDXR or SteamVR, Quest Link, SteamVR...).

rem v121 for 1.2.1, v143 for 1.4.3
set VER=v121

cd /d "%~dp0..\.."
set PATH=C:\msys64\ucrt64\bin;%PATH%

set TOTK_VR=1
rem Stereo draws a frame per eye: the game needs twice the headset's rate.
set TOTK_FPS_LIMIT=240
rem The other VR settings (view, turning, controls, rumble, motion combat...) are on the menu's VR tab (F1) and in
rem the [vr] section of totk.ini: a variable set here would fix them and grey the menu's rows out.

build\%VER%\totk.exe
if errorlevel 1 (
    echo.
    echo The game ended with an error. See the log next to totk.exe, the "vr:" lines.
    pause
)
