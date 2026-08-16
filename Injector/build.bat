@echo off
setlocal
cd /d "%~dp0"

if not exist mc_esp.dll (
    echo [!!] mc_esp.dll not found in this folder.
    echo      请先运行仓库根目录的 build.bat 完成整体构建。
    exit /b 1
)

set "GCC=C:\Users\11407\scoop\apps\gcc\current\bin\g++.exe"
if not exist "%GCC%" set "GCC=g++"

echo === build injector.exe ===
"%GCC%" -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -o injector.exe injector.cpp -luser32
if errorlevel 1 exit /b 1

echo [OK] injector.exe + mc_esp.dll are ready.
echo 使用: injector.exe   （自动查找 Minecraft 并注入）
echo 注入后按 INSERT 呼出连点器菜单。
exit /b 0
