@echo off
setlocal
rem Builds Jam Sesh and installs the plugin into FL Studio's own VST plugin folder and the standard VST3 folder.

set "VSDIR=G:\clod codes\toolchains\VS2022-BuildTools"
set "DEST=G:\Image-Line\FL Studio 21\Plugins\VST"

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1

if not exist "%DEST%" mkdir "%DEST%"
robocopy "build\ClodScope_artefacts\Release\VST3\Jam Sesh.vst3" "%DEST%\Jam Sesh.vst3" /E /NFL /NDL /NJH /NJS /NP
if %ERRORLEVEL% GEQ 8 (
    echo Copy failed - close any FL Studio project that has Jam Sesh loaded and run again.
    exit /b 1
)
echo Installed to "%DEST%\Jam Sesh.vst3"

rem FL Studio only scans .vst3 bundles in the standard VST3 folder, which needs admin to write
set "VST3DIR=C:\Program Files\Common Files\VST3"
powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','\"%~dp0install-vst3.ps1\"','-Source','\"%DEST%\Jam Sesh.vst3\"'"
fc /b "%DEST%\Jam Sesh.vst3\Contents\x86_64-win\Jam Sesh.vst3" "%VST3DIR%\Jam Sesh.vst3\Contents\x86_64-win\Jam Sesh.vst3" >nul 2>nul
if errorlevel 1 (
    echo VST3 folder copy FAILED - "%VST3DIR%\Jam Sesh.vst3" is not up to date.
    exit /b 1
)
echo Installed to "%VST3DIR%\Jam Sesh.vst3"  (restart FL Studio if it was open)
exit /b 0
