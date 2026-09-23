# Brief: porting Aniyomi extensions (Kotlin) to AnikkuNX (C++17, Nintendo Switch homebrew)

Project root: switch  (app sources in src/). Kotlin references: <clone di github.com/yuzono/aniyomi-extensions>
(src/<lang>/<ext>/..., shared themes in lib-multisrc/<theme>/, extractor libs in lib/<name>/).

## Framework you MUST build on (read these first, do NOT modify them)
- src/sources/source.hpp   — `src::Source` interface, structs Anime/Page/Episode/Details/Video, helpers
  (trim, replaceAll, substringAfter/Before, base64Decode (std+urlsafe), digitsOnly, parseNumber).
  Override `lang()` ("en", "es", "all"...) and `nsfw()` (true if the extension has isNsfw = true).
  `Video` has referer, userAgent, cookie, headers (extra HTTP headers for the player), quality (height px),
  subtitles/audio (external tracks: {url, lang}).
- src/sources/registry.hpp — factory declarations you must implement (see your assignment).
- src/net/http.hpp         — libcurl wrapper: http::request(method,url,headers,body,timeout,followRedirects)
  -> Response{status, body, finalUrl, headers(lowercase multimap), header(name)}; http::getText (throws on non-2xx);
  urlEncode, resolve(base, rel), originOf, hostOf, pathOf, queryParam, DEFAULT_UA, http::Error (exception).
  Cookies are shared automatically between requests (curl share + cookie engine).
- src/html/html.hpp        — gumbo-based DOM with Jsoup-like selectors. Supported: tag, *, .class, #id, [a], [a=v],
  [a*=v], [a^=v], [a$=v], [a~=v], :not(compound), descendant ' ' and child '>' combinators, comma lists.
  NOT supported: :has, :contains, :containsOwn, :eq, :nth-*, :first-child, :matches, '+', '~' — filter manually in C++.
  Node: tag(), attr(), hasAttr(), text() (normalized, like Jsoup text()), data() (raw script text), children(),
  parent(), select(), selectFirst(). Document(html, url): select, selectFirst, absUrl(node, "href").
  An empty/invalid Node returns "" for attr/text, so selectFirst(...).attr(...) is safe.
- src/util/crypto.hpp      — md5, sha1, sha256, hmacSha256, aesCbcDecrypt/Encrypt (PKCS7 opt), aesEcbDecrypt, aesCtr,
  rc4, cryptoJsDecrypt ("Salted__" base64 + passphrase), evpBytesToKey, base64Encode(urlSafe,padding), toHex, fromHex.
- src/util/unpacker.hpp    — unpacker::detect / unpacker::unpackAndCombine (Dean Edwards p,a,c,k,e,d packer = JsUnpacker).
- JSON: `#include <nlohmann/json.hpp>` (header at switch/library/borealis/library/include/borealis/extern/nlohmann).
- Reference ports to imitate (style, error handling, structure): src/sources/animeworld.cpp, animeunity.cpp, animesaturn.cpp.

## Rules
- One port = your own new .cpp file(s) in src/sources/ (namespace src). Put private helpers/extractors as `static`
  functions or inside an anonymous namespace in YOUR file so nothing collides with other agents working in parallel.
  Do not edit any existing file. If you think a shared change is needed, describe it in your final report instead.
- All methods are blocking and run on worker threads. Throw http::Error with a short Italian message on failure
  (e.g. "Nessun video trovato"). Episodes in Details must be ordered newest first (like Aniyomi's list).
- Anime.url / Episode.url: keep whatever relative/absolute form is convenient; the same string comes back to you.
- NEVER run std::regex on large strings (whole HTML pages/scripts): libstdc++ regex recurses and overflows the small
  Switch thread stack. Use find/substr, html selectors, or regex only on short snippets (< 2 KB).
- Preferences in Kotlin (quality, server, sub/dub, domain): use the defaults; sort videos so the preferred default is first.
  When the source offers sub and dub, return both as separate videos with clear titles ("Sub - 1080p", "Dub - 720p").
- Things impossible here (WebView, Cloudflare challenge solving, JS engines/WASM, torrents, local proxy servers):
  skip them; mpv handles HLS (incl. AES-128) and MP4 directly given url + referer/headers.
- Video.quality = height in px when known (e.g. 1080); title should show server and quality.
- Give each source a stable unique id string, e.g. "en.anichi".
- Syntax-check every file you write with:
    B=switch/library/borealis/library/include/borealis/extern
    g++ -std=c++17 -fsyntax-only -Iswitch/src -I$B -Iswitch/library/gumbo -I<header di curl> <file>
  Network access to the sites is blocked in this container, so you cannot test live: be careful and faithful to the Kotlin.
- Final report (concise): sources implemented (name, id, base URL), what was skipped/simplified and why, known risks.
