@echo off
rem Build the Code Annotator C++ port with the Visual Studio 18 toolchain.
setlocal
call "E:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo Could not initialize the Visual Studio build environment.
  exit /b 1
)
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE ^
   /D_CRT_SECURE_NO_WARNINGS /Fobuild\ ^
   core.cpp highlight.cpp gui.cpp ^
   /Fe:build\code_annotator.exe /link /SUBSYSTEM:WINDOWS
exit /b %errorlevel%
