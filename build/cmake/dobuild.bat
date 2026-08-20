@echo off
setlocal enabledelayedexpansion
rem Local dev build of the wxWidgets + OpenAL + GLEW variant, Windows x64.
rem Mirrors .github/workflows/windows.yml -> job windows-wx exactly, so a
rem successful local build is a good predictor of CI passing.
rem
rem Usage:
rem   build\cmake\dobuild.bat            - build the .exe only (default,
rem                                         same as the old desktop build)
rem   build\cmake\dobuild.bat package     - also build a .msi installer via
rem                                         CPack/WiX (requires WiX Toolset
rem                                         v3 - candle.exe/light.exe - on
rem                                         PATH; GitHub-hosted windows-latest
rem                                         runners have it preinstalled,
rem                                         a plain dev machine usually not)
rem
rem Requires: git, cmake, curl, powershell (all built into modern Windows),
rem MSVC toolchain (VS 2019+ Developer environment or plain "Visual Studio").

set DO_PACKAGE=0
if /I "%~1"=="package" set DO_PACKAGE=1
if /I "%~1"=="msi" set DO_PACKAGE=1
if /I "%~1"=="installer" set DO_PACKAGE=1

cd "%~dp0"

set CACHE_DIR=%~dp0_devcache
if not exist "%CACHE_DIR%" mkdir "%CACHE_DIR%"

rem --- 1. wxWidgets + OpenAL Soft + SDL2: bundled git submodules, same as CI ---
pushd "%~dp0..\.."
if not exist "3rdparty\wxWidgets\CMakeLists.txt" (
	echo Fetching wxWidgets submodule...
	git submodule update --init --recursive -- 3rdparty/wxWidgets
)
if not exist "3rdparty\openal-soft\CMakeLists.txt" (
	echo Fetching OpenAL Soft submodule...
	git submodule update --init --recursive -- 3rdparty/openal-soft
)
if not exist "3rdparty\SDL2\CMakeLists.txt" (
	echo Fetching SDL2 submodule...
	git submodule update --init --recursive -- 3rdparty/SDL2
)
popd

rem --- 2. GLEW (prebuilt, same version as CI) ---
set GLEWLIB=glew-2.2.0
if not exist "%CACHE_DIR%\%GLEWLIB%" (
	echo Downloading GLEW...
	curl -L -o "%CACHE_DIR%\%GLEWLIB%-win32.zip" https://github.com/nigels-com/glew/releases/download/glew-2.2.0/%GLEWLIB%-win32.zip
	powershell -NoProfile -Command "Expand-Archive -Path '%CACHE_DIR%\%GLEWLIB%-win32.zip' -DestinationPath '%CACHE_DIR%' -Force"
)
set GLEW_INCLUDE_DIR=%CACHE_DIR%\%GLEWLIB%\include
set GLEW_LIBRARY=%CACHE_DIR%\%GLEWLIB%\lib\Release\x64\glew32s.lib

rem --- 3. Configure + build ---
set BUILD_DIR=build_win32_wxwidgets_dev
if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"
pushd "%BUILD_DIR%"

cmake .. -A x64 -DUSE_SDL=0 -DUSE_WX_WIDGETS=1 ^
	-DGLEW_INCLUDE_DIR=%GLEW_INCLUDE_DIR% ^
	-DGLEW_LIBRARY=%GLEW_LIBRARY%
if errorlevel 1 exit /b 1

if "%DO_PACKAGE%"=="1" (
	echo Building .exe and packaging .msi via CPack/WiX ...
	cmake --build . --config Release --target PACKAGE
) else (
	echo Building .exe only ^(pass "package" as an argument to also build a .msi^) ...
	cmake --build . --config Release
)
if errorlevel 1 exit /b 1

popd

echo.
if "%DO_PACKAGE%"=="1" (
	echo Done. Installer: %BUILD_DIR%\*.msi
) else (
	echo Done. Executable: %BUILD_DIR%\Release\unreal_speccy_portable.exe
	echo ^(run "dobuild.bat package" to also build a .msi via WiX^)
)
endlocal
