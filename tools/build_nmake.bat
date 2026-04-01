@echo off
setlocal

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%

cd /d %~dp0\..
cmake -G "NMake Makefiles" -S . -B build-nmake
if errorlevel 1 exit /b %errorlevel%

cmake --build build-nmake
exit /b %errorlevel%
