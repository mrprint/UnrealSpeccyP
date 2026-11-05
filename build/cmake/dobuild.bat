@echo off
setlocal

rem Local dev build script.
rem
rem Default: the sdl2_desktop platform (SDL2 + desktop OpenGL + Dear ImGui), x64, built to run
rem on Windows 7 and later (-DWINDOWS_TARGET_WIN7=ON).
rem
rem Usage: dobuild.bat [x64] [x86] [both] [package] [wx]      (in any order)
rem   (no arguments)         build the x64 .exe
rem   x86 | win32            build the x86 .exe instead
rem   x64                    build the x64 .exe (the default, for use with "both"-like scripts)
rem   both                   build x86 and x64
rem   package | msi | installer
rem                          also build a .msi installer via CPack/WiX (WiX Toolset v3 on PATH)
rem   wx | wxwidgets         build the wxWidgets variant instead (no Windows 7 target)
rem
rem Examples: dobuild.bat x86 package      dobuild.bat both      dobuild.bat package wx
rem
rem Requires: git, cmake, curl, powershell (all built into modern Windows), MSVC toolchain
rem (VS 2019+ Developer environment or plain "Visual Studio"). Optional: python, for the
rem check that the sdl2_desktop .exe imports nothing newer than Windows 7.
rem Submodules must be fetched manually before running this script.

set DO_PACKAGE=0
set BUILD_WX=0
set BUILD_X86=0
set BUILD_X64=0

:parse
if "%~1"=="" goto parsed
set ARG_OK=0
for %%A in (package msi installer) do (
    if /I "%~1"=="%%A" (
        set DO_PACKAGE=1
        set ARG_OK=1
    )
)
for %%A in (wx wxwidgets) do (
    if /I "%~1"=="%%A" (
        set BUILD_WX=1
        set ARG_OK=1
    )
)
for %%A in (x86 win32) do (
    if /I "%~1"=="%%A" (
        set BUILD_X86=1
        set ARG_OK=1
    )
)
if /I "%~1"=="x64" (
    set BUILD_X64=1
    set ARG_OK=1
)
if /I "%~1"=="both" (
    set BUILD_X86=1
    set BUILD_X64=1
    set ARG_OK=1
)
if "%ARG_OK%"=="0" (
    echo Unknown argument: %~1 ^(see the usage at the top of this file^)
    exit /b 1
)
shift
goto parse

:parsed
if "%BUILD_X86%%BUILD_X64%"=="00" set BUILD_X64=1

cd /d "%~dp0"

set CACHE_DIR=%~dp0_devcache
if not exist "%CACHE_DIR%" mkdir "%CACHE_DIR%"

rem --- GLEW (prebuilt, same version as CI) ---

set GLEWLIB=glew-2.2.0
if exist "%CACHE_DIR%\%GLEWLIB%" goto glew_ready
echo Downloading GLEW...
curl -L -o "%CACHE_DIR%\%GLEWLIB%-win32.zip" https://github.com/nigels-com/glew/releases/download/glew-2.2.0/%GLEWLIB%-win32.zip
if errorlevel 1 exit /b 1
powershell -NoProfile -Command "Expand-Archive -Path '%CACHE_DIR%\%GLEWLIB%-win32.zip' -DestinationPath '%CACHE_DIR%' -Force"
if errorlevel 1 exit /b 1
:glew_ready
set GLEW_INCLUDE_DIR=%CACHE_DIR%\%GLEWLIB%\include
set RES_SRC=%~dp0..\..\res

if "%BUILD_X86%"=="1" call :build Win32
if errorlevel 1 exit /b 1
if "%BUILD_X64%"=="1" call :build x64
if errorlevel 1 exit /b 1

echo.
if "%DO_PACKAGE%"=="1" (echo Done. Installers: build_win32_*_dev\*.msi) else (echo Done. Executables: build_win32_*_dev\Release\unreal_speccy_portable.exe)
echo Options: package ^(.msi via WiX^), x86, both, wx
endlocal
exit /b 0

rem ----------------------------------------------------------------------------------------
rem :build <Win32|x64>
rem ----------------------------------------------------------------------------------------
:build
set BUILD_ARCH=%~1
set GLEW_LIBRARY=%CACHE_DIR%\%GLEWLIB%\lib\Release\%BUILD_ARCH%\glew32s.lib
if "%BUILD_WX%"=="1" (set BUILD_DIR=build_win32_wxwidgets_%BUILD_ARCH%_dev) else (set BUILD_DIR=build_win32_sdl2_desktop_%BUILD_ARCH%_dev)

echo.
echo === Building %BUILD_DIR% ===
if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"
pushd "%BUILD_DIR%"

if "%BUILD_WX%"=="1" goto configure_wx
cmake .. -A %BUILD_ARCH% -DUSE_SDL=0 -DUSE_SDL2_DESKTOP=1 -DWINDOWS_TARGET_WIN7=ON "-DGLEW_INCLUDE_DIR=%GLEW_INCLUDE_DIR%" "-DGLEW_LIBRARY=%GLEW_LIBRARY%"
goto configured
:configure_wx
cmake .. -A %BUILD_ARCH% -DUSE_SDL=0 -DUSE_WX_WIDGETS=1 "-DGLEW_INCLUDE_DIR=%GLEW_INCLUDE_DIR%" "-DGLEW_LIBRARY=%GLEW_LIBRARY%"
:configured
if errorlevel 1 goto build_failed

set BUILD_ARGS=--config Release
if "%DO_PACKAGE%"=="1" set BUILD_ARGS=--config Release --target PACKAGE
cmake --build . %BUILD_ARGS%
if errorlevel 1 goto build_failed
popd

xcopy /S /I /Y /Q "%RES_SRC%" "%BUILD_DIR%\Release\res"
if errorlevel 1 exit /b 1

rem --- sdl2_desktop must run on Windows 7: nothing newer may be imported (python is optional;
rem     "python --version" rather than "where": a Microsoft Store stub would be found by it) ---
if "%BUILD_WX%"=="1" exit /b 0
python --version >nul 2>nul
if errorlevel 1 echo python not found: the Windows 7 imports check is skipped
if errorlevel 1 exit /b 0
python "%~dp0..\win\check_win7_imports.py" "%BUILD_DIR%\Release\unreal_speccy_portable.exe"
exit /b %errorlevel%

:build_failed
popd
exit /b 1
