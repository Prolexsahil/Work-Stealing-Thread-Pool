@echo off
REM Windows build (MSYS2 / MinGW-w64 g++).  Run from PowerShell:  .\build.bat
REM -static bundles the C++ runtime, so the .exe files run without extra DLLs.
setlocal
if not exist build mkdir build
set FLAGS=-std=c++17 -O2 -Wall -Wextra -pthread -static -Iinclude

echo Building tests...        & g++ %FLAGS% tests\test_main.cpp        -o build\tests.exe         || goto :fail
echo Building benchmark...    & g++ %FLAGS% bench\benchmark.cpp        -o build\benchmark.exe     || goto :fail
echo Building basic_usage...  & g++ %FLAGS% examples\basic_usage.cpp   -o build\basic_usage.exe   || goto :fail
echo Building deadlock_demo...& g++ %FLAGS% examples\deadlock_demo.cpp -o build\deadlock_demo.exe || goto :fail

echo.
echo Build OK. Try:
echo   .\build\tests.exe
echo   .\build\basic_usage.exe
echo   .\build\deadlock_demo.exe
echo   .\build\benchmark.exe --quick
exit /b 0

:fail
echo.
echo BUILD FAILED - check that "g++ --version" shows GCC 9 or newer.
exit /b 1
