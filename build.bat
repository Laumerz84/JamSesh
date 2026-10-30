@echo off
setlocal
rem Builds Clod Scope and installs it into FL Studio's own VST plugin folder.

set "VSDIR=G:\clod codes\toolchains\VS2022-BuildTools"
set "DEST=G:\Image-Line\FL Studio 21\Plugins\VST"

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1

if not exist "%DEST%" mkdir "%DEST%"
robocopy "build\ClodScope_artefacts\Release\VST3\Clod Scope.vst3" "%DEST%\Clod Scope.vst3" /E /NFL /NDL /NJH /NJS /NP
if %ERRORLEVEL% GEQ 8 (
    echo Copy failed - close any FL Studio project that has Clod Scope loaded and run again.
    exit /b 1
)
echo Installed to "%DEST%\Clod Scope.vst3"

rem FL Studio only scans .vst3 bundles in the standard VST3 folder, which needs admin to write
set "VST3DIR=C:\Program Files\Common Files\VST3"
powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','\"%~dp0install-vst3.ps1\"','-Source','\"%DEST%\Clod Scope.vst3\"'"
fc /b "%DEST%\Clod Scope.vst3\Contents\x86_64-win\Clod Scope.vst3" "%VST3DIR%\Clod Scope.vst3\Contents\x86_64-win\Clod Scope.vst3" >nul 2>nul
if errorlevel 1 (
    echo VST3 folder copy FAILED - "%VST3DIR%\Clod Scope.vst3" is not up to date.
    exit /b 1
)
echo Installed to "%VST3DIR%\Clod Scope.vst3"  (restart FL Studio if it was open)
exit /b 0
