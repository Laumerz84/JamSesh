@echo off
setlocal
rem Builds Jam Sesh (the app) and Clod Scope (the plugin) and packages them with their source code for
rem sharing in dist\ (a folder and a zip). Installs nothing; see packaging\make-dist.ps1.

set "VSDIR=G:\clod codes\toolchains\VS2022-BuildTools"

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0packaging\make-dist.ps1" || exit /b 1
exit /b 0
