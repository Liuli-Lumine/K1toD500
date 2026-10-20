@echo off
rem ===================================================================
rem  Build K1toD500.exe  (single-file, no external runtime)
rem
rem  Compiler, either one:
rem    1) MSVC  : use "x64 Native Tools Command Prompt for VS 2022"
rem               (or call vcvars64.bat first), then run this script
rem    2) MinGW : g++ / windres on PATH
rem
rem  Output: build\K1toD500.exe
rem ===================================================================
setlocal
cd /d "%~dp0"
if not exist build mkdir build

where cl.exe >nul 2>nul
if not errorlevel 1 goto msvc
where g++ >nul 2>nul
if not errorlevel 1 goto mingw
echo [ERROR] No compiler found.
echo         Install Visual Studio 2022 with "Desktop development with C++",
echo         or MinGW-w64 (g++ + windres).
exit /b 1

:msvc
echo [MSVC] compiling resources...
rc /nologo /fo build\app.res /I src /I res res\app.rc
if errorlevel 1 goto fail
echo [MSVC] compiling and linking...
cl /nologo /O2 /utf-8 /std:c++17 /EHsc /W4 /MT /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DUNICODE /D_UNICODE /I src ^
    src\core.cpp src\util.cpp src\xlsx.cpp src\zip.cpp src\inflate.cpp ^
    src\d500dat.cpp src\reverse.cpp src\xlsxgen.cpp src\gui.cpp build\app.res ^
    /Fe:build\K1toD500.exe ^
    /link /SUBSYSTEM:WINDOWS /NODEFAULTLIB:OLDNAMES.lib ^
    user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib
if errorlevel 1 goto fail
goto done

:mingw
echo [MinGW] compiling resources...
windres -I res -I src --codepage=65001 res\app.rc -O coff -o build\app.res.o
if errorlevel 1 goto fail
echo [MinGW] compiling and linking...
g++ -std=c++17 -O2 -mwindows -static -static-libgcc -static-libstdc++ ^
    -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE -I src ^
    src\core.cpp src\util.cpp src\xlsx.cpp src\zip.cpp src\inflate.cpp ^
    src\d500dat.cpp src\reverse.cpp src\xlsxgen.cpp src\gui.cpp build\app.res.o ^
    -o build\K1toD500.exe -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32
if errorlevel 1 goto fail
goto done

:fail
echo.
echo [FAILED] build did not finish.
exit /b 1

:done
echo.
echo [OK] build\K1toD500.exe
exit /b 0
