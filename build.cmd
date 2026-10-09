@echo off
rem Builds otzaria-voice.exe (x64, static runtime, no console window).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++17 /O2 /MT /EHsc /utf-8 /W3 /DUNICODE /D_UNICODE /DNDEBUG ^
  src\main.cpp src\engine.cpp src\util.cpp src\pack.cpp ^
  /Fo:out\ /Fe:out\otzaria-voice.exe ^
  /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ws2_32.lib winmm.lib advapi32.lib shell32.lib user32.lib winhttp.lib bcrypt.lib psapi.lib || exit /b 1
