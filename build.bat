@echo off
setlocal
cd /d "%~dp0"

set "GCC=C:\Users\11407\scoop\apps\gcc\current\bin\g++.exe"
if not exist "%GCC%" set "GCC=g++"

if not exist build_tmp mkdir build_tmp
if not exist loader (
    echo [!!] loader\loader.cpp not found
    exit /b 1
)

echo === [1/4] build embedded payload DLL (build_tmp\mc_esp.dll) ===
"%GCC%" -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -Iinclude -Iinclude\win32 -o build_tmp\mc_esp.dll src\common.cpp src\clicker.cpp src\dllmain.cpp src\esp.cpp src\glrender.cpp src\jvm.cpp src\overlay.cpp -luser32 -lgdi32 -lwinmm
if errorlevel 1 goto :err

echo === [2/4] build bin2h.exe ===
"%GCC%" -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -municode -o build_tmp\bin2h.exe tools\bin2h.cpp
if errorlevel 1 goto :err

echo === [3/4] embed payload into build_tmp\payload.h ===
build_tmp\bin2h.exe build_tmp\mc_esp.dll build_tmp\payload.h
if errorlevel 1 goto :err

echo === [4/4] build single-file mc_esp.exe ===
"%GCC%" -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -municode -Ibuild_tmp -o mc_esp.exe loader\loader.cpp -luser32
if errorlevel 1 goto :err

echo.
echo [OK] done: mc_esp.exe
echo      单文件自注入 EXE，无需再分发 mc_esp.dll 或 injector.exe。
echo      用法: mc_esp.exe                自动查找 Minecraft 并注入
echo            mc_esp.exe -pid ^<PID^>     注入指定 PID
echo            mc_esp.exe -find          仅查找进程
exit /b 0

:err
echo [!!] build failed
exit /b 1
