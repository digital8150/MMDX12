@echo off
rem Usage: build.cmd [build-dir] [extra cmake --build args...]
rem   build.cmd                      -> builds everything into .\build
rem   build.cmd build-x --target mmdx_core
setlocal
set BUILD_DIR=%~1
if "%BUILD_DIR%"=="" set BUILD_DIR=build
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
set PATH=%USERPROFILE%\miniconda3\Scripts;%PATH%
if not exist "%~dp0%BUILD_DIR%\build.ninja" (
  cmake -S "%~dp0." -B "%~dp0%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
)
shift
cmake --build "%~dp0%BUILD_DIR%" %1 %2 %3 %4 %5 %6 %7 %8
