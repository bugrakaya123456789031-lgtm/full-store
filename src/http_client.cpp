// http_client.cpp — libcurl facade with Range-chunk parallel downloads.
#include "http_client.h"
#include "config.h"
#include "logger.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cctype>
#include <cstdlib>
#include <strings.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs_net {

namespace {

size_t write_mem(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, sz * nm);
    return sz * nm;
}

size_t write_fd(char* ptr, size_t sz, size_t nm, void* ud) {
    int fd = *static_cast<int*>(ud);
    size_t total = sz * nm;
    size_t written = 0;
    while (written < total) {
        ssize_t n = ::write(fd, ptr + written, total - written);
        if (n <= 0) return 0;
        written += (size_t)n;
    }
    return total;
}

size_t header_cb(char* buf, size_t sz, size_t nm, void* ud) {
    auto* hs = static_cast<std::vector<Header>*>(ud);
    size_t total = sz * nm;
    std::string line(buf, total);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        Header h;
        h.name = line.substr(0, colon);
        size_t v = colon + 1;
        while (v < line.size() && line[v] == ' ') v++;
        h.value = line.substr(v);
        hs->push_back(std::move(h));
    }
    return total;
}

curl_slist* build_slist(const std::vector<Header>& hs) {
    curl_slist* list = nullptr;
    for (const auto& h : hs) {
        std::string line = h.name + ": " + h.value;
        list = curl_slist_append(list, line.c_str());
    }
    const auto& s = fs_set::current();
    if (!s.cookie_header.empty()) {
        std::string c = "Cookie: " + s.cookie_header;
        list = curl_slist_append(list, c.c_str());
    }
    list = curl_slist_append(list, "Accept: */*");
    list = curl_slist_append(list, "Accept-Language: en-US,en;q=0.9");
    return list;
}

void apply_common(CURL* h) {
    const auto& s = fs_set::current();
    std::string ua = s.ua_override.empty() ? fs_cfg::USER_AGENT : s.ua_override;
    curl_easy_setopt(h, CURLOPT_USERAGENT,       ua.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS,       10L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER,  0L); // PS5 cert bundle paths vary
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST,  0L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT,  s.connect_timeout_sec);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, s.low_speed_limit_bps);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME,  s.low_speed_time_sec);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE,   1L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); // all supported
}

int64_t parse_content_length(const std::vector<Header>& hs) {
    for (const auto& h : hs) {
        if (strcasecmp(h.name.c_str(), "Content-Length") == 0) {
            return (int64_t)std::strtoll(h.value.c_str(), nullptr, 10);
        }
    }
    return -1;
}

bool server_supports_ranges(const std::vector<Header>& hs) {
    for (const auto& h : hs) {
        if (strcasecmp(h.name.c_str(), "Accept-Ranges") == 0) {
            if (h.value.find("bytes") != std::string::npos) return true;
        }
    }
    return false;
}

} // anon

std::string Response::header(const std::string& name) const {
    for (const auto& h : headers)
        if (strcasecmp(h.name.c_str(), name.c_str()) == 0) return h.value;
    return {};
}

void global_init()    { curl_global_init(CURL_GLOBAL_DEFAULT); }
void global_shutdown(){ curl_global_cleanup(); }

Response get(const std::string& url, const std::vector<Header>& extra) {
    Response r;
    CURL* h = curl_easy_init();
    if (!h) return r;
    curl_slist* hl = build_slist(extra);
    apply_common(h);
    curl_easy_setopt(h, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER,     hl);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,  write_mem);
    curl_easy_setopt(h, CURLOPT_WRITEDATA,      &r.body);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA,     &r.headers);

    CURLcode rc = curl_easy_perform(h);
    if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &r.status);
    else LOGW("GET %s failed: %s", url.c_str(), curl_easy_strerror(rc));

    curl_slist_free_all(hl);
    curl_easy_cleanup(h);
    return r;
}

Response head(const std::string& url, const std::vector<Header>& extra) {
    Response r;
    CURL* h = curl_easy_init();
    if (!h) return r;
    curl_slist* hl = build_slist(extra);
    apply_common(h);
    curl_easy_setopt(h, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER,     hl);
    curl_easy_setopt(h, CURLOPT_NOBODY,         1L);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA,     &r.headers);

    CURLcode rc = curl_easy_perform(h);
    if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &r.status);
    else LOGW("HEAD %s failed: %s", url.c_str(), curl_easy_strerror(rc));

    curl_slist_free_all(hl);
    curl_easy_cleanup(h);
    return r;
}

// ---- Chunked download implementation ------------------------------------

namespace {

struct ChunkCtx {
    int64_t start;
    int64_t end;       // inclusive
    int     fd;
    std::atomic<int64_t>* chunk_progress;
    std::atomic<bool>*    cancel;
    const DownloadOptions* opts;
    std::string url;
    bool ok = false;
};

size_t chunk_write(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* c = static_cast<ChunkCtx*>(ud);
    if (c->cancel->load()) return 0;
    size_t total = sz * nm;
    off_t offset = c->start + c->chunk_progress->load();
    size_t written = 0;
    while (written < total) {
        ssize_t n = ::pwrite(c->fd, ptr + written, total - written, offset + written);
        if (n <= 0) return 0;
        written += (size_t)n;
    }
    c->chunk_progress->fetch_add((int64_t)total);
    return total;
}

int chunk_xfer(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* c = static_cast<ChunkCtx*>(ud);
    return c->cancel->load() ? 1 : 0;
}

void download_chunk(ChunkCtx* c) {
    CURL* h = curl_easy_init();
    if (!h) return;
    curl_slist* hl = build_slist(c->opts->headers);
    apply_common(h);
    char range[64];
    std::snprintf(range, sizeof(range), "%lld-%lld",
                  (long long)c->start, (long long)c->end);
    curl_easy_setopt(h, CURLOPT_URL,              c->url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER,       hl);
    curl_easy_setopt(h, CURLOPT_RANGE,            range);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,    chunk_write);
    curl_easy_setopt(h, CURLOPT_WRITEDATA,        c);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, chunk_xfer);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA,     c);

    CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    c->ok = (rc == CURLE_OK) && (status == 206 || status == 200);
    if (!c->ok) LOGW("chunk %lld-%lld failed rc=%d http=%ld",
                     (long long)c->start, (long long)c->end, (int)rc, status);
    curl_slist_free_all(hl);
    curl_easy_cleanup(h);
}

// Streaming single-connection fallback.
bool download_single(const std::string& url,
                     const std::string& out,
                     const DownloadOptions& opts,
                     ProgressCb progress,
                     std::atomic<bool>& cancel) {
    int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { LOGE("open %s failed", out.c_str()); return false; }

    CURL* h = curl_easy_init();
    if (!h) { ::close(fd); return false; }
    curl_slist* hl = build_slist(opts.headers);
    apply_common(h);

    struct SingleCtx {
        int fd;
        std::atomic<bool>* cancel;
        int64_t done;
        int64_t total;
        ProgressCb cb;
        std::chrono::steady_clock::time_point last;
        int64_t last_done;
    } sc{fd, &cancel, 0, -1, progress, std::chrono::steady_clock::now(), 0};

    auto writecb = +[](char* ptr, size_t sz, size_t nm, void* ud) -> size_t {
        auto* s = (SingleCtx*)ud;
        if (s->cancel->load()) return 0;
        size_t total = sz * nm;
        size_t w = 0;
        while (w < total) {
            ssize_t n = ::write(s->fd, ptr + w, total - w);
            if (n <= 0) return 0;
            w += (size_t)n;
        }
        s->done += (int64_t)total;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - s->last).count();
        if (dt >= 0.5) {
            double speed = (s->done - s->last_done) / dt;
            if (s->cb && !s->cb(s->done, s->total, speed)) return 0;
            s->last = now;
            s->last_done = s->done;
        }
        return total;
    };
    auto xfercb = +[](void* ud, curl_off_t dltotal, curl_off_t, curl_off_t, curl_off_t) -> int {
        auto* s = (SingleCtx*)ud;
        if (dltotal > 0) s->total = dltotal;
        return s->cancel->load() ? 1 : 0;
    };

    curl_easy_setopt(h, CURLOPT_URL,              url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER,       hl);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,    writecb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA,        &sc);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xfercb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA,     &sc);

    CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(hl);
    curl_easy_cleanup(h);
    ::close(fd);

    bool ok = (rc == CURLE_OK) && (status >= 200 && status < 300);
    if (!ok) LOGW("single DL rc=%d http=%ld", (int)rc, status);
    else if (progress) progress(sc.done, sc.total < 0 ? sc.done : sc.total, 0.0);
    return ok;
}

} // anon

bool download_to_file(const std::string& url,
                      const std::string& out_path,
                      const DownloadOptions& opts,
                      ProgressCb progress,
                      std::atomic<bool>& cancel_flag) {
    // HEAD first to learn size + Range support.
    Response hr = head(url, opts.headers);
    int64_t total = parse_content_length(hr.headers);
    bool ranges   = server_supports_ranges(hr.headers);

    if (!ranges || total <= 0 || opts.chunks <= 1 ||
        total < fs_cfg::DEFAULT_CHUNK_SIZE_MIN) {
        LOGI("stream mode: url=%s size=%lld ranges=%d",
             url.c_str(), (long long)total, ranges);
        return download_single(url, out_path, opts, progress, cancel_flag);
    }

    // Preallocate output.
    int fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { LOGE("open %s failed", out_path.c_str()); return false; }
    if (::ftruncate(fd, total) != 0) {
        LOGW("ftruncate failed, continuing without preallocation");
    }

    int chunks = std::max(1, std::min(opts.chunks, 32));
    int64_t per = (total + chunks - 1) / chunks;

    std::vector<ChunkCtx> ctxs(chunks);
    std::vector<std::atomic<int64_t>> progs(chunks);
    for (int i = 0; i < chunks; i++) progs[i] = 0;
    std::vector<std::thread> threads;

    for (int i = 0; i < chunks; i++) {
        ctxs[i].start = i * per;
        ctxs[i].end   = std::min(total - 1, (i + 1) * per - 1);
        ctxs[i].fd    = fd;
        ctxs[i].chunk_progress = &progs[i];
        ctxs[i].cancel = &cancel_flag;
        ctxs[i].opts   = &opts;
        ctxs[i].url    = url;
        threads.emplace_back(download_chunk, &ctxs[i]);
    }

    // Progress aggregator thread.
    std::atomic<bool> done{false};
    std::thread agg([&]{
        auto t0 = std::chrono::steady_clock::now();
        int64_t last_sum = 0;
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            int64_t sum = 0;
            for (auto& p : progs) sum += p.load();
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - t0).count();
            double speed = dt > 0 ? (sum - last_sum) / 0.3 : 0;
            if (progress && !progress(sum, total, speed)) {
                cancel_flag.store(true);
            }
            last_sum = sum;
            t0 = now;
        }
    });

    for (auto& t : threads) t.join();
    done.store(true);
    agg.join();

    ::close(fd);

    bool ok = !cancel_flag.load();
    for (auto& c : ctxs) ok = ok && c.ok;
    if (!ok) LOGW("multi-chunk DL incomplete: %s", url.c_str());
    else if (progress) progress(total, total, 0.0);
    return ok;
}

// ---- URL helpers --------------------------------------------------------

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o; o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 0xF];
        }
    }
    return o;
}

std::string url_host(const std::string& url) {
    auto p = url.find("://");
    if (p == std::string::npos) return "";
    auto s = p + 3;
    auto e = url.find('/', s);
    return url.substr(s, (e == std::string::npos ? url.size() : e) - s);
}

std::string url_filename(const std::string& url) {
    auto q = url.find('?');
    std::string u = (q == std::string::npos) ? url : url.substr(0, q);
    auto slash = u.find_last_of('/');
    if (slash == std::string::npos) return u;
    return u.substr(slash + 1);
}

} // namespace fs_net
