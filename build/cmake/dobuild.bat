@echo off
setlocal enabledelayedexpansion
rem Local dev build script.
rem
rem Default: builds the sdl2_desktop platform (SDL2 + desktop OpenGL + Dear ImGui).
rem
rem Usage:
rem   build\cmake\dobuild.bat                  - build sdl2_desktop .exe only (default)
rem   build\cmake\dobuild.bat package     - also build a .msi installer via
rem                                              CPack/WiX (requires WiX Toolset v3 on PATH)
rem   build\cmake\dobuild.bat wx               - build the wxWidgets variant instead
rem   build\cmake\dobuild.bat wx package       - build wxWidgets .exe and .msi
rem
rem Requires: git, cmake, curl, powershell (all built into modern Windows),
rem MSVC toolchain (VS 2019+ Developer environment or plain "Visual Studio").
rem Submodules must be fetched manually before running this script.

set DO_PACKAGE=0
set BUILD_WX=0
if /I "%~1"=="package" set DO_PACKAGE=1
if /I "%~1"=="msi" set DO_PACKAGE=1
if /I "%~1"=="installer" set DO_PACKAGE=1
if /I "%~1"=="wx" set BUILD_WX=1
if /I "%~1"=="wxwidgets" set BUILD_WX=1

if "%BUILD_WX%"=="1" (
    if /I "%~2"=="package" set DO_PACKAGE=1
    if /I "%~2"=="msi" set DO_PACKAGE=1
    if /I "%~2"=="installer" set DO_PACKAGE=1
)

cd "%~dp0"

set CACHE_DIR=%~dp0_devcache
if not exist "%CACHE_DIR%" mkdir "%CACHE_DIR%"

rem --- 1. GLEW (prebuilt, same version as CI) ---

set GLEWLIB=glew-2.2.0
if not exist "%CACHE_DIR%\%GLEWLIB%" (
	echo Downloading GLEW...
	curl -L -o "%CACHE_DIR%\%GLEWLIB%-win32.zip" https://github.com/nigels-com/glew/releases/download/glew-2.2.0/%GLEWLIB%-win32.zip
	powershell -NoProfile -Command "Expand-Archive -Path '%CACHE_DIR%\%GLEWLIB%-win32.zip' -DestinationPath '%CACHE_DIR%' -Force"
)
set GLEW_INCLUDE_DIR=%CACHE_DIR%\%GLEWLIB%\include
set GLEW_LIBRARY=%CACHE_DIR%\%GLEWLIB%\lib\Release\x64\glew32s.lib

rem --- 2. Configure + build ---
if "%BUILD_WX%"=="1" (
set BUILD_DIR=build_win32_wxwidgets_dev
    set USE_SDL=0
    set USE_WX_WIDGETS=1
) else (
    set BUILD_DIR=build_win32_sdl2_desktop_dev
    set USE_SDL=0
    set USE_SDL2_DESKTOP=1
    set USE_WX_WIDGETS=0
)
if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"
pushd "%BUILD_DIR%"

if "%BUILD_WX%"=="1" (
    cmake .. -A x64 -DUSE_SDL=%USE_SDL% -DUSE_WX_WIDGETS=%USE_WX_WIDGETS% ^
	-DGLEW_INCLUDE_DIR=%GLEW_INCLUDE_DIR% ^
	-DGLEW_LIBRARY=%GLEW_LIBRARY%
) else (
    cmake .. -A x64 -DUSE_SDL=%USE_SDL% -DUSE_SDL2_DESKTOP=%USE_SDL2_DESKTOP% ^
        -DUSE_WX_WIDGETS=%USE_WX_WIDGETS% ^
        -DGLEW_INCLUDE_DIR=%GLEW_INCLUDE_DIR% ^
        -DGLEW_LIBRARY=%GLEW_LIBRARY%
)
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
    echo ^(run "dobuild.bat wx" to build the wxWidgets variant^)
)
endlocal
