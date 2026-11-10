@echo off
rem TOTK VR Recomp: start the game in VR (docs/vr-setup.md).
rem First: start the headset's OpenXR runtime (Virtual Desktop connected with VDXR or SteamVR, Quest Link, SteamVR...).

rem v121 for 1.2.1, v143 for 1.4.3
set VER=v121

cd /d "%~dp0..\.."
set PATH=C:\msys64\ucrt64\bin;%PATH%

set TOTK_VR=1
rem The virtual screen: width and distance in metres.
set TOTK_VR_SCREEN_WIDTH=3.0
set TOTK_VR_SCREEN_DISTANCE=2.5
rem Motion combat: right-hand slashes and stabs attack, the left hand raised guards.
set TOTK_VR_MOTION=1
rem normal, or easy (slower swings count)
set TOTK_VR_SWING=normal

build\%VER%\totk.exe
if errorlevel 1 (
    echo.
    echo The game ended with an error. See the log next to totk.exe, the "vr:" lines.
    pause
)
