// direct.cpp — passthrough resolver for links that already point at files.
#include "resolver.h"
#include "http_client.h"
#include "logger.h"

#include <cstdlib>

namespace fs_resolve::direct {

std::optional<Resolved> resolve(const std::string& url) {
    Resolved r;
    r.direct_url = url;
    r.suggested_filename = fs_net::url_filename(url);
    auto head = fs_net::head(url);
    if (head.ok()) {
        auto cl = head.header("Content-Length");
        if (!cl.empty()) r.size_bytes = std::strtoll(cl.c_str(), nullptr, 10);
    }
    LOGI("direct passthrough %s size=%lld", url.c_str(), (long long)r.size_bytes);
    return r;
}

} // namespace fs_resolve::direct
