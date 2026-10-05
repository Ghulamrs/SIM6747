@echo off
rem Build vm6747.exe with Visual Studio 2022's cl, from a plain cmd prompt or
rem over ssh (the box's shell is cmd, so this is run by its full path with no
rem "cmd /c" in front). Objects go to ..\build\Emulator\msvc, the binary to
rem the repository root, as the Makefile does on the Mac and the box.
setlocal
set HERE=%~dp0
set ROOT=%HERE%..
set OBJ=%ROOT%\..\build\Emulator\msvc
if not exist "%OBJ%" mkdir "%OBJ%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%ROOT%"
cl -nologo -std:c++14 -O2 -W4 -WX -EHsc -D_CRT_SECURE_NO_WARNINGS -Fo"%OBJ%\\" -Fe"%ROOT%\vm6747.exe" src\Asm.cpp src\Cpu.cpp src\Isa.cpp src\Runtime.cpp src\main.cpp
exit /b %errorlevel%
