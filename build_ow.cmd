@echo off
rem Build Agent98 with OpenWatcom for 32-bit Windows 9x/NT.
setlocal
cd /d "%~dp0"

if "%WATCOM%"=="" set "WATCOM=%~dp0tools\openwatcom\ow"
if not exist "%WATCOM%\binnt\wmake.exe" (
  echo OpenWatcom was not found at "%WATCOM%".
  echo Extract the installer there, or set WATCOM to your OpenWatcom root.
  exit /b 1
)

set "PATH=%WATCOM%\binnt;%PATH%"
set "INCLUDE=%WATCOM%\h;%WATCOM%\h\nt"

if not exist "build\ow\obj" mkdir "build\ow\obj"
if not exist "build\ow\obj\bearssl" mkdir "build\ow\obj\bearssl"

"%WATCOM%\binnt\wmake.exe" -f Makefile %*
if errorlevel 1 exit /b 1

echo Build complete.
endlocal
