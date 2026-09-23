#include "view/cover_image.hpp"

#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

#include "app/api.hpp"

#ifdef HAVE_WEBP
#include <webp/decode.h>
#endif

namespace {

/** Cache LRU dei JPEG scaricati (max ~24 MB). */
class BytesCache {
  public:
    bool get(const std::string& key, std::string& out) {
        std::lock_guard<std::mutex> lock(m);
        auto it = map.find(key);
        if (it == map.end()) return false;
        order.splice(order.begin(), order, it->second.second);
        out = it->second.first;
        return true;
    }

    void put(const std::string& key, const std::string& data) {
        std::lock_guard<std::mutex> lock(m);
        if (map.count(key)) return;
        order.push_front(key);
        map[key] = {data, order.begin()};
        size += data.size();
        while (size > maxSize && !order.empty()) {
            auto& k = order.back();
            size -= map[k].first.size();
            map.erase(k);
            order.pop_back();
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m);
        map.clear();
        order.clear();
        size = 0;
    }

  private:
    std::mutex m;
    std::list<std::string> order;
    std::unordered_map<std::string, std::pair<std::string, std::list<std::string>::iterator>> map;
    size_t size = 0;
    const size_t maxSize = 24 * 1024 * 1024;
};

BytesCache& cache() {
    static BytesCache c;
    return c;
}

bool isWebp(const std::string& d) {
    return d.size() > 12 && d.compare(0, 4, "RIFF") == 0 && d.compare(8, 4, "WEBP") == 0;
}

/** Le immagini WebP (non supportate da nanovg) vengono decodificate nel thread di lavoro in RGBA grezzo,
 *  preceduto dall'intestazione "\0RGBA" + larghezza + altezza. */
const char RAW_MAGIC[5] = {0, 'R', 'G', 'B', 'A'};

std::string decodeIfNeeded(std::string data) {
#ifdef HAVE_WEBP
    if (isWebp(data)) {
        int w = 0, h = 0;
        uint8_t* px = WebPDecodeRGBA((const uint8_t*)data.data(), data.size(), &w, &h);
        if (!px) return "";
        std::string out(RAW_MAGIC, 5);
        out.append((const char*)&w, 4);
        out.append((const char*)&h, 4);
        out.append((const char*)px, (size_t)w * h * 4);
        WebPFree(px);
        return out;
    }
#endif
    return data;
}

}  // namespace

void CoverImage::setBytes(const std::string& data) {
    if (data.size() > 13 && data.compare(0, 5, std::string(RAW_MAGIC, 5)) == 0) {
        int w, h;
        memcpy(&w, data.data() + 5, 4);
        memcpy(&h, data.data() + 9, 4);
        NVGcontext* vg = brls::Application::getNVGContext();
        int tex = nvgCreateImageRGBA(vg, w, h, 0, (const unsigned char*)data.data() + 13);
        if (tex) this->innerSetImage(tex);
        return;
    }
    this->setImageFromMem((const unsigned char*)data.data(), (int)data.size());
}

CoverImage::CoverImage() {
    this->setScalingType(brls::ImageScalingType::FILL);
    this->setInterpolation(brls::ImageInterpolation::LINEAR);
    this->setBackgroundColor(nvgRGBA(128, 128, 128, 40));
}

CoverImage::~CoverImage() { *alive = false; }

void CoverImage::clearCache() { cache().clear(); }

void CoverImage::setUrl(const std::string& pathOrUrl) {
    url = pathOrUrl;
    if (url.empty()) return;

    std::string cached;
    if (cache().get(url, cached)) {
        setBytes(cached);
        return;
    }

    std::string u = url;
    runAsync<std::string>(
        alive,
        [u] {
            std::string data = decodeIfNeeded(api::download(u));
            if (!data.empty()) cache().put(u, data);
            return data;
        },
        [this, u](std::string data) {
            if (u != this->url || data.empty()) return;
            this->setBytes(data);
        },
        [](const std::string&) { /* copertina mancante: lascia lo sfondo grigio */ });
}
