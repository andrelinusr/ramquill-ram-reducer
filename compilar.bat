@echo off
chcp 65001 >nul
title Compilar RamQuill
cd /d "%~dp0"

REM --- procura o gcc automaticamente (varios lugares e pastas com nome winlibs*) ---
where gcc >nul 2>nul
if not errorlevel 1 goto tem_gcc
if exist "C:\mingw64\bin\gcc.exe" set "PATH=C:\mingw64\bin;%PATH%"
if exist "C:\mingw64\mingw64\bin\gcc.exe" set "PATH=C:\mingw64\mingw64\bin;%PATH%"
if exist "C:\msys64\ucrt64\bin\gcc.exe" set "PATH=C:\msys64\ucrt64\bin;%PATH%"
if exist "C:\msys64\mingw64\bin\gcc.exe" set "PATH=C:\msys64\mingw64\bin;%PATH%"
for /d %%D in ("C:\winlibs*") do if exist "%%~D\mingw64\bin\gcc.exe" set "PATH=%%~D\mingw64\bin;%PATH%"
for /d %%D in ("C:\winlibs*") do if exist "%%~D\bin\gcc.exe" set "PATH=%%~D\bin;%PATH%"
for /d %%D in ("%USERPROFILE%\Downloads\winlibs*") do if exist "%%~D\mingw64\bin\gcc.exe" set "PATH=%%~D\mingw64\bin;%PATH%"
for /d %%D in ("%USERPROFILE%\Downloads\winlibs*") do if exist "%%~D\bin\gcc.exe" set "PATH=%%~D\bin;%PATH%"
where gcc >nul 2>nul
if not errorlevel 1 goto tem_gcc

REM --- nao achou: pede o caminho da pasta bin ---
echo.
echo Nao encontrei o gcc automaticamente.
echo Abra a pasta onde voce extraiu o WinLibs no Explorador de Arquivos,
echo entre ate ver a pasta "bin" que contem o arquivo gcc.exe,
echo clique na barra de endereco, copie o caminho e cole aqui.
echo Exemplo: C:\winlibs-xxxx\mingw64\bin
echo.
set "GCCDIR="
set /p "GCCDIR=Caminho da pasta bin: "
if not defined GCCDIR goto sem_gcc
set "GCCDIR=%GCCDIR:"=%"
if exist "%GCCDIR%\gcc.exe" set "PATH=%GCCDIR%;%PATH%"
where gcc >nul 2>nul
if errorlevel 1 goto sem_gcc

:tem_gcc
if not exist main.c (
    echo [ERRO] main.c nao encontrado nesta pasta.
    pause
    exit /b 1
)

REM --- recursos: manifesto (pede Administrador) + icone opcional ---
if exist ramquill.ico (
    (echo 1 ICON "ramquill.ico") > ramquill.rc
) else (
    echo [aviso] ramquill.ico nao encontrado - o .exe sera gerado sem icone proprio.
    break > ramquill.rc
)
(echo 1 24 "ramquill.manifest") >> ramquill.rc

windres ramquill.rc -O coff -o ramquill_res.o
if errorlevel 1 (
    echo [ERRO] windres falhou. Confira se ramquill.manifest esta nesta pasta.
    pause
    exit /b 1
)

gcc -std=gnu17 -O2 -s -municode -mwindows -static -Wall -Wextra ^
    -ffunction-sections -fdata-sections -Wl,--gc-sections ^
    main.c ramquill_res.o -o RamQuill.exe ^
    -lgdi32 -luser32 -ladvapi32 -lpsapi -ldwmapi -lshell32 -lm
if errorlevel 1 (
    echo [ERRO] Falha na compilacao.
    pause
    exit /b 1
)

del ramquill_res.o ramquill.rc >nul 2>nul
echo.
echo Pronto! RamQuill.exe gerado em %~dp0
pause
exit /b 0

:sem_gcc
echo.
echo [ERRO] O compilador gcc nao foi encontrado.
echo Baixe o WinLibs em https://winlibs.com ^(versao Win64, ZIP^), extraia
echo e rode este arquivo de novo, informando a pasta bin quando ele pedir.
echo.
pause
exit /b 1