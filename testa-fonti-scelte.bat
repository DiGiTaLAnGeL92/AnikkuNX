@echo off
rem Prova approfondita solo delle fonti indicate in fonti-da-provare.txt (nomi separati da virgola).
setlocal
cd /d "%~dp0"
set "MSYS=%LOCALAPPDATA%\AnikkuDev\msys64"
set "LOG=%~dp0testa-fonti-scelte.log"
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
set /p FONTI=<"%~dp0fonti-da-provare.txt"
"%MSYS%\usr\bin\bash.exe" -lc "bash \"$(cygpath -u '%~dp0switch\scripts\test_sources.sh')\" '%~dp0.' '%FONTI%' 'one piece'" > "%LOG%" 2>&1
echo FATTO >> "%LOG%"
