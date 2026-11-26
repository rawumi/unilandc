@echo off
setlocal
if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if defined VSDIR if exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :build
if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
:build
cl /nologo /utf-8 /std:c11 /W3 /O2 /Fe:uniland.exe ^
   src\main.c src\bigint.c src\value.c src\lexer.c src\parser.c src\interp.c src\vm.c src\std.c src\pybridge.c src\cbridge.c src\gobridge.c src\http.c src\fmt.c src\lsp.c src\gui.c ^
   /link legacy_stdio_definitions.lib winhttp.lib user32.lib gdi32.lib gdiplus.lib winmm.lib comdlg32.lib shell32.lib ole32.lib uuid.lib
if errorlevel 1 exit /b 1
echo built: uniland.exe
