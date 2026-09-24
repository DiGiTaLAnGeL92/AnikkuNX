@echo off
rem Prova un episodio preciso: prova-episodio.bat "<fonte>" "<ricerca>" <numero>
setlocal
cd /d "%~dp0"
set "MSYS=%LOCALAPPDATA%\AnikkuDev\msys64"
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
set "FONTE=%~1"
set "CERCA=%~2"
set "NUM=%~3"
if "%FONTE%"=="" set "FONTE=AnimeSaturn"
if "%CERCA%"=="" set "CERCA=One Piece"
if "%NUM%"=="" set "NUM=1178"
"%MSYS%\usr\bin\bash.exe" -lc "export PATH=/ucrt64/bin:$PATH; cd \"$(cygpath -u '%~dp0switch')\" && g++ -std=c++17 -O1 -w -Isrc -Ilibrary/borealis/library/include/borealis/extern -Ilibrary/gumbo -c tools/sourcetest.cpp -o build_test/sonda.o && for f in src/net/hls_proxy.cpp src/net/http.cpp src/sources/registry.cpp; do g++ -std=c++17 -O1 -w -Isrc -Ilibrary/borealis/library/include/borealis/extern -Ilibrary/gumbo -c $f -o build_test/obj/$(basename $f .cpp).o; done && g++ build_test/sonda.o $(ls build_test/obj/*.o | grep -v sourcetest.o) build_test/gumbo.a -lcurl -lws2_32 -o build_test/sonda.exe && ANX_DUMP=\"$(cygpath -w '%~dp0log-fonti/pagine')\" ./build_test/sonda.exe episodio '%FONTE%' '%CERCA%' '%NUM%' resources/cacert.pem" > "%~dp0prova-episodio.log" 2>&1
echo FATTO >> "%~dp0prova-episodio.log"
