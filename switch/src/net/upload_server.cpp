#include "net/upload_server.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifdef __SWITCH__
#include <switch.h>
#endif

#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace uploadserver {

namespace {

std::mutex mtx;
Status st;
std::string pinCode, dest;
int listenSock = -1;
std::atomic<int> generation{0};

const char* PAGE = R"HTML(<!doctype html>
<html lang="it"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AnikkuNX - aggiornamento debug</title>
<style>
body{font-family:system-ui,sans-serif;background:#1e1e22;color:#eee;display:flex;justify-content:center;padding:40px 16px}
.box{max-width:460px;width:100%;background:#2a2a30;border-radius:12px;padding:24px}
h1{font-size:20px;margin:0 0 16px;color:#ff78aa}
input,button{font-size:16px;width:100%;box-sizing:border-box;margin:8px 0;padding:10px;border-radius:8px;border:1px solid #555;background:#1e1e22;color:#eee}
button{background:#d6336c;border:none;cursor:pointer}
button:disabled{opacity:.5}
progress{width:100%;height:18px}
#msg{margin-top:12px;min-height:1.4em}
</style></head><body><div class="box">
<h1>AnikkuNX &middot; invia un nuovo .nro</h1>
<label>File AnikkuNX.nro</label><input type="file" id="f" accept=".nro">
<label>Codice mostrato sulla console</label><input id="pin" inputmode="numeric" maxlength="4" placeholder="1234">
<button id="go">Invia alla console</button>
<progress id="p" max="100" value="0"></progress>
<div id="msg"></div>
</div>
<script>
const $=id=>document.getElementById(id);
$('go').onclick=()=>{
  const f=$('f').files[0], pin=$('pin').value.trim();
  if(!f){$('msg').textContent='Scegli il file .nro';return;}
  if(pin.length!=4){$('msg').textContent='Inserisci il codice di 4 cifre';return;}
  const x=new XMLHttpRequest();
  x.open('POST','/upload?pin='+encodeURIComponent(pin));
  x.upload.onprogress=e=>{if(e.lengthComputable)$('p').value=e.loaded*100/e.total;};
  x.onload=()=>{$('msg').textContent=x.responseText;$('go').disabled=false;};
  x.onerror=()=>{$('msg').textContent='Connessione interrotta';$('go').disabled=false;};
  $('go').disabled=true;$('msg').textContent='Invio in corso...';
  x.send(f);
};
</script></body></html>)HTML";

void sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        long n = send(fd, data.data() + off, (int)std::min<size_t>(data.size() - off, 32 * 1024), 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

void reply(int fd, int code, const char* type, const std::string& body) {
    std::string head = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Error") +
                       "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n";
    sendAll(fd, head + body);
}

void setState(const std::string& s, const std::string& err = "") {
    std::lock_guard<std::mutex> lock(mtx);
    st.state = s;
    st.error = err;
}

void handle(int fd) {
    std::string req;
    char buf[16384];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos && req.size() < 32768) {
        long n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        req.append(buf, (size_t)n);
        headerEnd = req.find("\r\n\r\n");
    }
    if (headerEnd == std::string::npos) return;
    std::string head = req.substr(0, headerEnd);
    std::string body = req.substr(headerEnd + 4);
    size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    std::string method = head.substr(0, sp1);
    std::string path = sp1 != std::string::npos && sp2 != std::string::npos ? head.substr(sp1 + 1, sp2 - sp1 - 1) : "/";

    if (method == "GET") {
        reply(fd, 200, "text/html; charset=utf-8", PAGE);
        return;
    }
    if (method != "POST" || path.rfind("/upload", 0) != 0) {
        reply(fd, 404, "text/plain; charset=utf-8", "Non trovato");
        return;
    }
    std::string pin;
    size_t pp = path.find("pin=");
    if (pp != std::string::npos) pin = path.substr(pp + 4, 4);
    if (pin != pinCode) {
        reply(fd, 403, "text/plain; charset=utf-8", "Codice errato: controlla quello mostrato sulla console.");
        return;
    }
    // Content-Length (nome dell'intestazione senza distinzione di maiuscole)
    long long total = -1;
    {
        std::string lower = head;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        size_t cl = lower.find("\r\ncontent-length:");
        if (cl != std::string::npos) total = atoll(lower.c_str() + cl + 17);
    }
    if (total <= 0 || total > 256LL * 1024 * 1024) {
        reply(fd, 411, "text/plain; charset=utf-8", "Dimensione del file mancante o non valida.");
        return;
    }
    std::string part = dest + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) {
        reply(fd, 500, "text/plain; charset=utf-8", "Impossibile scrivere sulla scheda SD.");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mtx);
        st.state = "ricezione";
        st.received = 0;
        st.total = total;
        st.error.clear();
    }
    long long got = (long long)body.size();
    if (!body.empty()) fwrite(body.data(), 1, body.size(), f);
    bool ok = true;
    while (got < total) {
        long n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            ok = false;
            break;
        }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            ok = false;
            break;
        }
        got += n;
        std::lock_guard<std::mutex> lock(mtx);
        st.received = got;
    }
    fclose(f);
    if (!ok || got < total) {
        std::remove(part.c_str());
        setState("errore", "Invio interrotto");
        reply(fd, 400, "text/plain; charset=utf-8", "Invio interrotto: riprova.");
        return;
    }
    // e' davvero un .nro? (firma "NRO0" all'offset 0x10)
    char magic[4] = {0};
    f = fopen(part.c_str(), "rb");
    if (f) {
        fseek(f, 0x10, SEEK_SET);
        if (fread(magic, 1, 4, f) != 4) magic[0] = 0;
        fclose(f);
    }
    if (std::memcmp(magic, "NRO0", 4) != 0) {
        std::remove(part.c_str());
        setState("errore", "Il file non e' un .nro valido");
        reply(fd, 400, "text/plain; charset=utf-8", "Il file non e' un .nro valido.");
        return;
    }
    std::remove(dest.c_str());
    std::rename(part.c_str(), dest.c_str());
    {
        std::lock_guard<std::mutex> lock(mtx);
        st.received = got;
        st.state = "ricevuto";
    }
    reply(fd, 200, "text/plain; charset=utf-8", "File ricevuto! Conferma l'installazione sulla console.");
}

void* serverMain(void* arg) {
    int myGen = (int)(intptr_t)arg;
    while (generation == myGen) {
        int sock;
        {
            std::lock_guard<std::mutex> lock(mtx);
            sock = listenSock;
        }
        if (sock < 0) break;
        // attesa con timeout, per potersi fermare quando la finestra viene chiusa
        pollfd p{};
        p.fd = sock;
        p.events = POLLIN;
        int r = poll(&p, 1, 500);
        if (generation != myGen) break;
        if (r <= 0) continue;
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int fd = accept(sock, (sockaddr*)&cli, &len);
        if (fd < 0) continue;
        handle(fd);
        shutdown(fd, 2);
        close(fd);
    }
    return nullptr;
}

}  // namespace

int start(const std::string& pin, const std::string& destPath) {
    stop();
    std::lock_guard<std::mutex> lock(mtx);
    pinCode = pin;
    dest = destPath;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // raggiungibile dalla rete locale
    int port = 0;
    for (int p = 8080; p <= 8090; p++) {
        addr.sin_port = htons(p);
        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == 0 && listen(sock, 4) == 0) {
            port = p;
            break;
        }
    }
    if (!port) {
        close(sock);
        return 0;
    }
    listenSock = sock;
    st = Status{};
    st.running = true;
    st.port = port;
    st.state = "attesa";
    int gen = ++generation;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &attr, serverMain, (void*)(intptr_t)gen) != 0) {
        close(sock);
        listenSock = -1;
        st.running = false;
        port = 0;
    }
    pthread_attr_destroy(&attr);
    return port;
}

void stop() {
    std::lock_guard<std::mutex> lock(mtx);
    ++generation;  // il thread esce al prossimo giro (entro mezzo secondo)
    if (listenSock >= 0) {
        close(listenSock);
        listenSock = -1;
    }
    st.running = false;
}

Status status() {
    std::lock_guard<std::mutex> lock(mtx);
    return st;
}

std::string localIp() {
#ifdef __SWITCH__
    u32 ip = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || ip == 0) return "";
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff);
    return buf;
#else
    return "127.0.0.1";
#endif
}

}  // namespace uploadserver
