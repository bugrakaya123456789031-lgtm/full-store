// resolver.cpp — dispatch to per-host resolvers.
#include "resolver.h"
#include "http_client.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace fs_resolve {

// Forward decls — implementations in resolvers/*.cpp
namespace pixeldrain  { std::optional<Resolved> resolve(const std::string&); }
namespace buzzheavier { std::optional<Resolved> resolve(const std::string&); }
namespace onefichier  { std::optional<Resolved> resolve(const std::string&); }
namespace mediafire   { std::optional<Resolved> resolve(const std::string&); }
namespace gofile      { std::optional<Resolved> resolve(const std::string&); }
namespace direct      { std::optional<Resolved> resolve(const std::string&); }

namespace {
    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return s;
    }
    bool contains(const std::string& h, const char* n) {
        return lower(h).find(n) != std::string::npos;
    }
}

std::string host_of(const std::string& url) {
    std::string h = lower(fs_net::url_host(url));
    if (h.find("pixeldrain")  != std::string::npos ||
        h.find("pixeldra.in") != std::string::npos) return "pixeldrain";
    if (h.find("buzzheavier") != std::string::npos) return "buzzheavier";
    if (h.find("1fichier")    != std::string::npos) return "1fichier";
    if (h.find("mediafire")   != std::string::npos) return "mediafire";
    if (h.find("gofile")      != std::string::npos) return "gofile";
    return h;
}

bool supported(const std::string& url) {
    std::string h = host_of(url);
    if (h == "pixeldrain" || h == "buzzheavier" || h == "1fichier" ||
        h == "mediafire"  || h == "gofile") return true;
    // Direct file by extension.
    std::string u = lower(url);
    for (const char* e : {".pkg", ".zip", ".7z", ".rar", ".iso", ".bin"}) {
        if (u.rfind(e) != std::string::npos) return true;
    }
    return false;
}

std::optional<Resolved> resolve(const std::string& page_url) {
    std::string h = host_of(page_url);
    LOGI("resolve host=%s url=%s", h.c_str(), page_url.c_str());

    if (h == "pixeldrain")  return pixeldrain::resolve(page_url);
    if (h == "buzzheavier") return buzzheavier::resolve(page_url);
    if (h == "1fichier")    return onefichier::resolve(page_url);
    if (h == "mediafire")   return mediafire::resolve(page_url);
    if (h == "gofile")      return gofile::resolve(page_url);

    // Try as direct URL if extension matches.
    std::string u = lower(page_url);
    for (const char* e : {".pkg", ".zip", ".7z", ".rar", ".iso", ".bin"}) {
        if (u.rfind(e) != std::string::npos) return direct::resolve(page_url);
    }

    LOGW("no resolver for %s", h.c_str());
    return std::nullopt;
}

} // namespace fs_resolve
