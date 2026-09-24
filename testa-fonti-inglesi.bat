@echo off
rem Prova approfondita delle fonti inglesi e multilingua (fino al primo segmento video).
setlocal
cd /d "%~dp0"
set "MSYS=%LOCALAPPDATA%\AnikkuDev\msys64"
set "LOG=%~dp0testa-fonti-inglesi.log"
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
echo Provo le fonti inglesi e multilingua... (log in %LOG%)
"%MSYS%\usr\bin\bash.exe" -lc "bash \"$(cygpath -u '%~dp0switch\scripts\test_sources.sh')\" '%~dp0.' en 'one piece'" > "%LOG%" 2>&1
"%MSYS%\usr\bin\bash.exe" -lc "bash \"$(cygpath -u '%~dp0switch\scripts\test_sources.sh')\" '%~dp0.' all 'one piece'" >> "%LOG%" 2>&1
echo FATTO >> "%LOG%"
