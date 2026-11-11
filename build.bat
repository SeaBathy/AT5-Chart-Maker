@echo off
rem Builds bin\sat2at5.exe (converter) and bin\sat2at5_gui.exe (window program) with MSVC, x64.
rem Needs Visual Studio 2022 or its Build Tools ("Desktop development with C++"). Uses only Windows built-ins:
rem WIC (images) and winsqlite3 (MBTiles). Builds for any x64 CPU (no AVX requirement).
setlocal
cd /d "%~dp0"
if defined VCINSTALLDIR goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR for %%e in (BuildTools Community Professional Enterprise) do (
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\%%e"
  if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%e"
)
if not defined VSDIR ( echo Visual Studio 2022 or its Build Tools with C++ were not found. & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if not defined VCINSTALLDIR ( echo Could not set up the Visual Studio C++ tools. & exit /b 1 )
:build
if not exist bin mkdir bin
if not exist obj mkdir obj
rc /nologo /fo obj\app.res src\app.rc || exit /b 1
cl /nologo /std:c++20 /utf-8 /O2 /Oi /GL /EHsc /MT /W3 /DNDEBUG /DUNICODE /D_UNICODE ^
   /Foobj\ src\main.cpp src\at5.cpp src\sources.cpp src\vector.cpp obj\app.res ^
   /Fe:bin\sat2at5.exe /link /LTCG /MANIFEST:EMBED /MANIFESTINPUT:src\sat2at5.manifest windowscodecs.lib ole32.lib propsys.lib winsqlite3.lib || exit /b 1
cl /nologo /std:c++20 /utf-8 /O2 /EHsc /MT /W3 /DNDEBUG /DUNICODE /D_UNICODE /Foobj\ src\gui.cpp obj\app.res ^
   /Fe:bin\sat2at5_gui.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\gui.manifest user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib || exit /b 1
echo Built bin\sat2at5.exe and bin\sat2at5_gui.exe
exit /b 0
