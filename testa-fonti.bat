@echo off
rem Prova le fonti italiane integrate (AnimeWorld, AnimeUnity, AnimeSaturn) dal PC.
setlocal
cd /d "%~dp0"
set "MSYS=%LOCALAPPDATA%\AnikkuDev\msys64"
set "LOG=%~dp0testa-fonti.log"
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
if not exist "%MSYS%\usr\bin\bash.exe" (
  echo Esegui prima compila-switch.bat per installare MSYS2.
  pause
  exit /b 1
)
echo Provo le fonti... (log in %LOG%)
"%MSYS%\usr\bin\bash.exe" -lc "bash \"$(cygpath -u '%~dp0switch\scripts\test_sources.sh')\" '%~dp0.' %*" > "%LOG%" 2>&1
type "%LOG%"
echo FATTO >> "%LOG%"
if "%NOPAUSE%"=="" pause
