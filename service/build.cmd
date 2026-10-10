@echo off
rem Builds out\otzaria-voice.exe (x64, static runtime, no console window) with
rem CMake and Visual Studio 2022. Same as:
rem   cmake -S service -B build && cmake --build build --config Release
setlocal
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
"%CMAKE%" -S "%~dp0." -B "%~dp0out\build" -A x64 %* || exit /b 1
"%CMAKE%" --build "%~dp0out\build" --config Release || exit /b 1
copy /y "%~dp0out\build\Release\otzaria-voice.exe" "%~dp0out\otzaria-voice.exe" >nul || exit /b 1
echo %~dp0out\otzaria-voice.exe
