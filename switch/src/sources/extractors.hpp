#pragma once

// Utilita' ed estrattori video condivisi tra le fonti (StreamWish, VidHide, ok.ru, Dailymotion, Rumble, StreamPlay,
// Mp4Upload, DoodStream, StreamTape, Voe, VidMoly, Vtube, WolfStream, Moon/Filemoon, Vatchus, HLS generico).
// Ogni estrattore ritorna una lista vuota (o lancia http::Error) se non trova video.

#include <initializer_list>
#include <string>
#include <vector>

#include "html/html.hpp"
#include "net/http.hpp"
#include "sources/source.hpp"

namespace src {
namespace ext {

std::string lower(std::string s);
bool contains(const std::string& s, const std::string& what);
bool containsCI(const std::string& s, const std::string& what);
bool startsWith(const std::string& s, const std::string& p);
bool endsWith(const std::string& s, const std::string& p);
std::string after(const std::string& s, const std::string& d, const std::string& missing);
std::string ownText(const html::Node& node);
std::string scriptWith(const html::Document& doc, std::initializer_list<const char*> needles);
std::string fixUrl(const std::string& url, const std::string& base = "");
int qualityOf(const std::string& title);
std::string randomHex(size_t len);
std::string randomAlnum(size_t len);
std::string jsField(const std::string& obj, const std::string& key);
std::vector<Video::Track> captionTracks(const std::string& script, const std::string& base = "");
std::vector<std::string> sourcesFiles(const std::string& script);
std::vector<Video> hlsVideos(const std::string& master, const std::string& referer, const std::string& titlePrefix,
                             const std::vector<Video::Track>& subs = {}, const http::Headers& extra = {},
                             const std::string& userAgent = "");
std::vector<Video> streamWish(const std::string& url, const std::string& prefix, const http::Headers& headers = {});
std::vector<Video> vidHide(const std::string& url, const std::string& prefix, const http::Headers& headers = {});
std::vector<Video> okru(const std::string& rawUrl, const std::string& prefix);
std::vector<Video> dailymotion(const std::string& url, const std::string& titlePrefix);
std::vector<Video> rumble(const std::string& url, const std::string& prefix);
std::vector<Video> streamPlayServer(const std::string& url, const std::string& prefix);
std::vector<Video> streamPlay(const std::string& url, const std::string& prefix);
std::vector<Video> mp4upload(const std::string& url, const std::string& prefix);
std::vector<Video> dood(const std::string& url, const std::string& prefix);
std::vector<Video> streamtape(const std::string& url, const std::string& prefix);
std::vector<Video> voe(const std::string& url, const std::string& prefix);
std::vector<Video> vidMoly(const std::string& iframeUrl, const std::string& prefix);
std::vector<Video> vtube(const std::string& url, const std::string& siteUrl, const std::string& prefix);
std::vector<Video> wolfstream(const std::string& url, const std::string& prefix);
std::string b64url(const std::string& s);
std::vector<Video> moon(const std::string& url, const std::string& siteUrl, const std::string& prefix);
std::vector<Video> vatchus(const std::string& url, const std::string& prefix);

}  // namespace ext
}  // namespace src
