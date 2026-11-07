@echo off
rem Builds the Metroid Prime 4 Beyond PC EXE from this repository (the steps in the README, with checks first).
rem   build.bat                  the release EXE in bin\runtime_win64_win32.exe
rem   build.bat release 0.2.2    also packs a release folder in release\ (needs gcc\, python\, redist\zlib1.dll and redist\SDL2.dll)
rem Toolchain, first found wins:
rem   GCC     gcc\Library\bin (winlibs GCC 14.2.0, what releases ship), else SWR_GCC_BIN, else gcc on PATH (MSYS2 UCRT64 works)
rem   Python  python\python.exe, else the py launcher, else python on PATH
rem   glslang GLSLANG, else glslang on PATH, else the Vulkan SDK's (VULKAN_SDK)
setlocal
cd /d "%~dp0"

set "GBIN="
if exist "gcc\Library\bin\gcc.exe" set "GBIN=%~dp0gcc\Library\bin"
if not defined GBIN if defined SWR_GCC_BIN set "GBIN=%SWR_GCC_BIN%"
if not defined GBIN for %%G in (gcc.exe) do set "GBIN=%%~dp$PATH:G"
if not defined GBIN (
  echo GCC was not found. Either put winlibs GCC 14.2.0 ^(UCRT, POSIX threads, x64^) in gcc\ so that gcc\Library\bin\gcc.exe
  echo exists, or install MSYS2 with its UCRT64 GCC ^(pacman -S mingw-w64-ucrt-x86_64-gcc^) and put C:\msys64\ucrt64\bin on PATH,
  echo or set SWR_GCC_BIN to a GCC bin folder.
  exit /b 1
)
if "%GBIN:~-1%"=="\" set "GBIN=%GBIN:~0,-1%"
if not exist "%GBIN%\g++.exe" (
  echo "%GBIN%" has gcc.exe but no g++.exe: the runtime needs both.
  exit /b 1
)
rem the same GCC for every step: the build scripts read SWR_GCC_BIN, switchrecomp.py finds gcc on PATH
set "SWR_GCC_BIN=%GBIN%"
set "PATH=%GBIN%;%PATH%"
echo Using GCC in %GBIN%
if not exist "gcc\Library\bin\gcc.exe" echo   ^(not the bundled gcc\: fine for development; releases are built with winlibs GCC 14.2.0^)

set "PY="
if exist "python\python.exe" set "PY=python\python.exe"
if not defined PY (
  py -3 --version >nul 2>&1 && set "PY=py -3"
)
if not defined PY (
  python --version >nul 2>&1 && set "PY=python"
)
if not defined PY (
  echo Python 3 was not found: put Python 3.12 for Windows ^(the embeddable package is enough^) in python\, or install Python 3.
  exit /b 1
)
echo Using Python: %PY%

if "%GLSLANG%"=="" (
  for %%G in (glslang.exe) do set "GLSLANG=%%~$PATH:G"
)
if "%GLSLANG%"=="" if defined VULKAN_SDK if exist "%VULKAN_SDK%\Bin\glslang.exe" set "GLSLANG=%VULKAN_SDK%\Bin\glslang.exe"
if "%GLSLANG%"=="" (
  echo glslang was not found: install the Vulkan SDK, set GLSLANG=C:\path\to\glslang.exe or put glslang.exe on PATH.
  echo The renderer's cmake pins glslang 16.6.0.
  exit /b 1
)
if not exist "%GLSLANG%" (
  echo GLSLANG points to "%GLSLANG%", which does not exist.
  exit /b 1
)
echo Using %GLSLANG%

echo [1/4] Dear ImGui
%PY% work-profiler\build_imgui.py || goto :fail
echo [2/4] Native Vulkan renderer (with DLSS for DLAA)
set SWR_NO_DLSS=
%PY% work-profiler\build_snxvk2.py || goto :fail
echo [3/4] Runtime EXE
set SWR_RELEASE=1
%PY% switchrecomp.py --build-runtime --cc gcc || goto :fail
set SWR_RELEASE=
if /i not "%1"=="release" (
  echo Done: bin\runtime_win64_win32.exe
  exit /b 0
)
if "%2"=="" (
  echo usage: build.bat release X.Y.Z
  exit /b 1
)
rem the release ships the recompiler with its own GCC and Python: they must be the pinned ones in gcc\ and python\
if not exist "gcc\Library\bin\gcc.exe" (
  echo A release needs winlibs GCC 14.2.0 in gcc\ ^(it is copied into the release for the player's setup^).
  exit /b 1
)
if not exist "python\python.exe" (
  echo A release needs the Python 3.12 embeddable package in python\ ^(it is copied into the release^).
  exit /b 1
)
echo [4/4] Release %2
python\python.exe beyond\make_release.py release release\%2 --version %2 --exe bin\runtime_win64_win32.exe || goto :fail
echo Done: release\%2
exit /b 0

:fail
echo Build failed: see the messages above.
exit /b 1
