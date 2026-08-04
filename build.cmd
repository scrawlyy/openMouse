@echo off
REM openMouse - MSVC build.
REM Usage: build.cmd [output-name.exe]
REM
REM Keep this file pure ASCII. cmd.exe reads batch files in the OEM codepage,
REM so a stray UTF-8 character corrupts the whole script, not just its comment.
REM
REM /utf-8 is required: the sources ARE UTF-8, without a BOM. Without the flag
REM MSVC decodes them as the system ANSI codepage and every non-ASCII character
REM becomes mojibake - inside wide L"..." literals too, so it reaches the tray
REM menu and the log, not only narrow text.
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS (
    echo [build] Could not locate vcvars64.bat.
    echo [build] Install Visual Studio Build Tools with the C++ workload.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

set "OUT=%~1"
if "%OUT%"=="" set "OUT=openmouse.exe"

cd /d "%~dp0"
if not exist build mkdir build

cl /nologo /std:c++17 /EHsc /W4 /permissive- /O2 /MT /utf-8 ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /Fo:build\ /Fe:%OUT% ^
   src\main.cpp src\devices.cpp src\overlay.cpp src\config.cpp ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup ^
   user32.lib gdi32.lib shell32.lib ole32.lib uuid.lib advapi32.lib
exit /b %errorlevel%
