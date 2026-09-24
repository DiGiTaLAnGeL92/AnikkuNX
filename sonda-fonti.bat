@echo off
rem Sonda veloce della home page di tutte le fonti (stato HTTP, redirect, Cloudflare).
setlocal
cd /d "%~dp0"
set "MSYS=%LOCALAPPDATA%\AnikkuDev\msys64"
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
"%MSYS%\usr\bin\bash.exe" -lc "export PATH=/ucrt64/bin:$PATH; cd \"$(cygpath -u '%~dp0switch')\" && g++ -std=c++17 -O1 -w -Isrc -Ilibrary/borealis/library/include/borealis/extern -Ilibrary/gumbo -c tools/sourcetest.cpp -o build_test/obj/sourcetest.o && g++ build_test/obj/*.o build_test/gumbo.a -lcurl -lws2_32 -o build_test/sourcetest.exe && ./build_test/sourcetest.exe sonda '' resources/cacert.pem" > "%~dp0sonda-fonti.log" 2>&1
echo FATTO >> "%~dp0sonda-fonti.log"
