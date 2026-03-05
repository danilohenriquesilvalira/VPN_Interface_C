@echo off
:: -------------------------------------------------------
:: build_local.bat  -  Compilar e testar sem CI/GitHub
:: Requer: MSYS2 em C:\msys64  (mingw-w64-x86_64-gcc ja instalado)
:: Uso:
::   build_local.bat          -> compila
::   build_local.bat run      -> compila e lanca o .exe
::   build_local.bat clean    -> limpa objetos e .exe
:: -------------------------------------------------------
setlocal

set MINGW=%MSYS2_ROOT%
if "%MINGW%"=="" set MINGW=C:\msys64

if not exist "%MINGW%\mingw64\bin\gcc.exe" (
    echo [ERRO] GCC nao encontrado em %MINGW%\mingw64\bin
    echo Instala com: %MINGW%\msys2_shell.cmd -mingw64
    echo  depois escreve: pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make
    pause
    exit /b 1
)

set PATH=%MINGW%\mingw64\bin;%MINGW%\usr\bin;%PATH%

cd /d "%~dp0"

if /i "%1"=="clean" (
    del /q src\*.o 2>nul
    del /q rls_vpn.exe 2>nul
    echo Limpo.
    goto :eof
)

echo [BUILD] Compilando v1.5.4...
taskkill /F /IM rls_vpn.exe >nul 2>&1
timeout /t 1 /nobreak >nul
mingw32-make 2>&1
if errorlevel 2 (
    echo [ERRO] Falha na compilacao.
    pause
    exit /b 1
)

echo [OK] rls_vpn.exe gerado com sucesso.

if /i "%1"=="run" (
    echo [RUN] Lancando rls_vpn.exe...
    start "" "%~dp0rls_vpn.exe"
)

endlocal
