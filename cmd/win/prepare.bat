@echo on

for /f "usebackq tokens=*" %%i in (`cmd\win\vswhere -latest -prerelease -property installationPath`) do (
  set VS_DIR=%%i
)

call "%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat" x64

mkdir .build/win
if not defined GENERATOR set "GENERATOR=Visual Studio 18 2026"

rem An unset variable has to drop the flag entirely, not pass an empty one
set "DISPATCH_ARG="
if defined SHIBE_DISPATCH_METHOD set "DISPATCH_ARG=-D SHIBE_DISPATCH_METHOD=%SHIBE_DISPATCH_METHOD%"

cmake ^
    -B .build/win ^
    -G "%GENERATOR%" ^
    -D RELOADABLE=OFF ^
    -DCMAKE_C_COMPILER_LAUNCHER=sccache ^
    -DCMAKE_CXX_COMPILER_LAUNCHER=sccache ^
    -D PLATFORM_NAME=win ^
    %DISPATCH_ARG% ^
    -D CMAKE_TOOLCHAIN_FILE=../../cmake/msvc.cmake ^
    -D CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
    .
