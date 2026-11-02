@echo off
rem Builds out\otzaria-voice.exe (x64, static runtime, no console window) with
rem CMake and Visual Studio 2022. Same as:
rem   cmake -S service -B build && cmake --build build --config Release
rem With OTZARIA_VOICE_ARCH=ARM64 it builds out\otzaria-voice-arm64.exe instead
rem (Windows on ARM; see src\common\handoff.h), in out\build-arm64.
setlocal
set "ARCH=%OTZARIA_VOICE_ARCH%"
if not defined ARCH set "ARCH=x64"
set "BUILD=%~dp0out\build"
set "EXE=otzaria-voice.exe"
if /i "%ARCH%"=="ARM64" (
  set "BUILD=%~dp0out\build-arm64"
  set "EXE=otzaria-voice-arm64.exe"
)
set "CMAKE=cmake"
where cmake >nul 2>nul && goto build
for %%e in (Community Professional Enterprise BuildTools) do (
  if exist "C:\Program Files\Microsoft Visual Studio\2022\%%e\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\%%e\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    goto build
  )
)
echo CMake not found: install it or Visual Studio 2022 with C++ tools. 1>&2
exit /b 1

:build
"%CMAKE%" -S "%~dp0." -B "%BUILD%" -A %ARCH% %* || exit /b 1
"%CMAKE%" --build "%BUILD%" --config Release || exit /b 1
copy /y "%BUILD%\Release\otzaria-voice.exe" "%~dp0out\%EXE%" >nul || exit /b 1
echo %~dp0out\%EXE%
