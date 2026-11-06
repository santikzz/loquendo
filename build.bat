@echo off
rem Build jorge.exe with MinGW gcc, without CMake. cmd does not expand *.c, so collect the files here.
setlocal enabledelayedexpansion
cd /d "%~dp0"
where gcc >nul 2>nul || (echo gcc not found in PATH & exit /b 1)
if not exist gen\funcs.h (echo gen\ is missing. Check out the full repository. & exit /b 1)
set SRC=
for %%f in (gen\*.c runtime\*.c) do set SRC=!SRC! "%%f"
echo Compiling, this takes about a minute...
rem -mwindows: no console window, so agents can call it silently
gcc -std=c11 -O1 -w -mwindows -Iruntime -Igen !SRC! -o jorge.exe -lm -lwinmm || exit /b 1
echo Built jorge.exe
