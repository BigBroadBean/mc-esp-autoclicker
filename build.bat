@echo off
setlocal
cd /d "%~dp0"

set "GCC=C:\Users\11407\scoop\apps\gcc\current\bin\g++.exe"
if not exist "%GCC%" set "GCC=g++"

echo === build mc_esp.dll (ESP + clicker + Insert menu) ===
"%GCC%" -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -Iinclude -Iinclude\win32 -o mc_esp.dll src\common.cpp src\clicker.cpp src\dllmain.cpp src\esp.cpp src\glrender.cpp src\jvm.cpp src\overlay.cpp -luser32 -lgdi32 -lwinmm
if errorlevel 1 goto :err

if not exist Injector mkdir Injector
copy /Y mc_esp.dll Injector\mc_esp.dll >nul

echo === build Injector\injector.exe ===
"%GCC%" -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -o Injector\injector.exe Injector\injector.cpp -luser32
if errorlevel 1 goto :err

echo.
echo [OK] done: mc_esp.dll + Injector\injector.exe + Injector\mc_esp.dll
exit /b 0

:err
echo [!!] build failed
exit /b 1
