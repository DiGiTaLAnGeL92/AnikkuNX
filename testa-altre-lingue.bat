@echo off
rem Prova approfondita delle fonti di tutte le altre lingue (in parallelo, un log per lingua in log-fonti\).
setlocal
cd /d "%~dp0"
set "MSYS=%LOCALAPPDATA%\AnikkuDev\msys64"
set "LOG=%~dp0testa-altre-lingue.log"
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
set "LINGUE=es pt fr de ar id tr ru pl zh ko sr uk hi all"
if exist "%~dp0lingue-da-provare.txt" set /p LINGUE=<"%~dp0lingue-da-provare.txt"
"%MSYS%\usr\bin\bash.exe" -lc "bash \"$(cygpath -u '%~dp0switch\scripts\test_langs.sh')\" '%~dp0.' '%LINGUE%' 'one piece'" > "%LOG%" 2>&1
echo FATTO >> "%LOG%"
