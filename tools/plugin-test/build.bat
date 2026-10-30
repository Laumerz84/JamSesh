@echo off
call "G:\clod codes\toolchains\VS2022-BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >nul || exit /b 1
cmake --build build 2>&1 | findstr /i "error FAILED"
exit /b 0
