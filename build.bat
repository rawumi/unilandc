@echo off
setlocal
if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :build
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
:build
cl /nologo /utf-8 /std:c11 /W3 /O2 /Fe:uniland.exe ^
   src\main.c src\bigint.c src\value.c src\lexer.c src\parser.c src\interp.c src\std.c src\pybridge.c src\cbridge.c src\gobridge.c src\http.c src\fmt.c src\lsp.c src\gui.c ^
   /link legacy_stdio_definitions.lib winhttp.lib user32.lib gdi32.lib winmm.lib comdlg32.lib shell32.lib ole32.lib uuid.lib
if errorlevel 1 exit /b 1
echo built: uniland.exe
