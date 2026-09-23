@echo off
rem ============================================================
rem  Compila AnikkuNX.nro su Windows.
rem  La prima volta installa MSYS2 + devkitPro (circa 1,5 GB) in
rem  %LOCALAPPDATA%\AnikkuDev: serve solo una connessione internet.
rem ============================================================
setlocal
cd /d "%~dp0"
set "DEV=%LOCALAPPDATA%\AnikkuDev"
set "MSYS=%DEV%\msys64"
set "LOG=%~dp0compila-switch.log"
set "BASH=%MSYS%\usr\bin\bash.exe"
set "MSYSTEM=MSYS"
set "CHERE_INVOKING=1"
set "MSYS2_PATH_TYPE=minimal"

echo Log completo in: %LOG%
echo ==== %DATE% %TIME% ==== > "%LOG%"

if not exist "%BASH%" (
  echo [1/5] Scarico MSYS2...
  if not exist "%DEV%" mkdir "%DEV%"
  powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri 'https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-base-x86_64-latest.sfx.exe' -OutFile '%DEV%\msys2-base.sfx.exe'" >> "%LOG%" 2>&1
  "%DEV%\msys2-base.sfx.exe" -y -o"%DEV%" >> "%LOG%" 2>&1
  del "%DEV%\msys2-base.sfx.exe"
  if not exist "%BASH%" (
    echo [ERRORE] Installazione di MSYS2 non riuscita. Vedi il log.
    pause
    exit /b 1
  )
  echo Inizializzo MSYS2...
  "%BASH%" -lc "exit" >> "%LOG%" 2>&1
)

echo [2/5] Aggiorno MSYS2 (puo' richiedere qualche minuto)...
"%BASH%" -lc "bash \"$(cygpath -u '%~dp0switch\scripts\build_windows.sh')\" update '%~dp0.'" >> "%LOG%" 2>&1
"%BASH%" -lc "bash \"$(cygpath -u '%~dp0switch\scripts\build_windows.sh')\" update '%~dp0.'" >> "%LOG%" 2>&1

echo [3/5] Installo devkitPro, libnx, mpv e le altre librerie...
"%BASH%" -lc "bash \"$(cygpath -u '%~dp0switch\scripts\build_windows.sh')\" tools '%~dp0.'" >> "%LOG%" 2>&1
if errorlevel 1 (
  echo [ERRORE] Installazione degli strumenti non riuscita. Vedi il log.
  pause
  exit /b 1
)

echo [4/5] Compilo AnikkuNX.nro...
"%BASH%" -lc "bash \"$(cygpath -u '%~dp0switch\scripts\build_windows.sh')\" build '%~dp0.'" >> "%LOG%" 2>&1
if errorlevel 1 (
  echo [ERRORE] Compilazione non riuscita. Vedi il log.
  pause
  exit /b 1
)

echo [5/5] Fatto! Il file e' in: %~dp0dist\AnikkuNX.nro
echo FATTO >> "%LOG%"
pause
