@echo off
setlocal

:: Set default architecture to x64 if no argument is provided
if "%1"=="x86" (
    set ARCH=x86
    set TRIPLET=x86-windows-static-md
) else (
    set ARCH=x64
    set TRIPLET=x64-windows-static-md
)

:: Set up environment variables
set VCPKG_ROOT=%~dp0..\..\vcpkg
set VCPKG_INSTALLED_DIR=%VCPKG_ROOT%\installed

:: Check if vcpkg is already installed
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo Setting up vcpkg...
    git clone https://github.com/microsoft/vcpkg.git %VCPKG_ROOT%
    call %VCPKG_ROOT%\bootstrap-vcpkg.bat -disableMetrics
)

:: Install dependencies using vcpkg
::echo Installing dependencies...
::call %VCPKG_ROOT%\vcpkg install --triplet=%TRIPLET% sdl2 angle curl wxwidgets glew openal-soft fmt tinyxml2 minizip zlib libpng

:: Configure CMake
echo Configuring CMake for %ARCH%...
cmake -S %~dp0 -B build-%ARCH% ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
    -DVCPKG_MANIFEST_DIR=%~dp0.. ^
    -DVCPKG_TARGET_TRIPLET=%TRIPLET% ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreadedDLL" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DUSE_STATIC_LIBS_WITH_DLL_CRT=ON

:: Build the project
echo Building the project...
cmake --build build-%ARCH% --config Release --parallel -- /p:VcpkgEnableManifest=true

:: Prepare artifact directory
echo Preparing artifact directory...
set ARTIFACT_DIR=%~dp0artifact-%ARCH%
mkdir "%ARTIFACT_DIR%" 2>nul || ver >nul
copy /Y build-%ARCH%\Release\unreal_speccy_portable.exe "%ARTIFACT_DIR%\" || exit /b 1
xcopy /E /I ..\..\res "%ARTIFACT_DIR%\res"

echo Build completed successfully. Output is in the "%ARTIFACT_DIR%" directory.
endlocal
