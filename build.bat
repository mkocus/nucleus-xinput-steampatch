@echo off
:: Build xinput proxy DLLs (x86 + x64, xinput1_3 + xinput1_4).
:: Requires Visual Studio Build Tools or Visual Studio with C++ workload.
:: Output: x86\ and x64\ subdirectories.

setlocal enabledelayedexpansion

:: Find vcvarsall.bat
set "VCVARS="
for %%y in (2022 2019 2017) do (
    for %%e in (Enterprise Professional Community BuildTools) do (
        set "CANDIDATE=C:\Program Files (x86)\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvarsall.bat"
        if exist "!CANDIDATE!" set "VCVARS=!CANDIDATE!"
    )
)
if not defined VCVARS (
    for /f "tokens=*" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul') do (
        set "VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat"
    )
)
if not defined VCVARS (
    echo ERROR: Cannot find Visual Studio. Install Build Tools for Visual Studio.
    exit /b 1
)

set ERRORS=0

:: ---- x86 builds ----
echo.
echo === Building x86 ===
call "%VCVARS%" x86 >nul 2>&1
if not exist x86 mkdir x86

echo   xinput1_3.dll ...
cl.exe /nologo /O2 /W3 /c xinput1_3.c /Fo:x86\xinput1_3.obj >nul
link.exe /nologo /DLL /DEF:xinput1_3.def /OUT:x86\xinput1_3.dll /INCREMENTAL:NO x86\xinput1_3.obj kernel32.lib user32.lib >nul
if errorlevel 1 (echo   FAILED & set ERRORS=1) else echo   OK

echo   xinput1_4.dll ...
cl.exe /nologo /O2 /W3 /c xinput1_3.c /Fo:x86\xinput1_4.obj >nul
link.exe /nologo /DLL /DEF:xinput1_4.def /OUT:x86\xinput1_4.dll /INCREMENTAL:NO x86\xinput1_4.obj kernel32.lib user32.lib >nul
if errorlevel 1 (echo   FAILED & set ERRORS=1) else echo   OK

:: ---- x64 builds ----
echo.
echo === Building x64 ===
call "%VCVARS%" x64 >nul 2>&1
if not exist x64 mkdir x64

echo   xinput1_3.dll ...
cl.exe /nologo /O2 /W3 /c xinput1_3.c /Fo:x64\xinput1_3.obj >nul
link.exe /nologo /DLL /DEF:xinput1_3.def /OUT:x64\xinput1_3.dll /INCREMENTAL:NO x64\xinput1_3.obj kernel32.lib user32.lib >nul
if errorlevel 1 (echo   FAILED & set ERRORS=1) else echo   OK

echo   xinput1_4.dll ...
cl.exe /nologo /O2 /W3 /c xinput1_3.c /Fo:x64\xinput1_4.obj >nul
link.exe /nologo /DLL /DEF:xinput1_4.def /OUT:x64\xinput1_4.dll /INCREMENTAL:NO x64\xinput1_4.obj kernel32.lib user32.lib >nul
if errorlevel 1 (echo   FAILED & set ERRORS=1) else echo   OK

:: Cleanup
del /q x86\*.obj x86\*.exp x86\*.lib x64\*.obj x64\*.exp x64\*.lib 2>nul

echo.
if %ERRORS%==0 (
    echo Build successful:
    echo   x86\xinput1_3.dll   x86\xinput1_4.dll
    echo   x64\xinput1_3.dll   x64\xinput1_4.dll
) else (
    echo Some builds FAILED.
)
exit /b %ERRORS%
