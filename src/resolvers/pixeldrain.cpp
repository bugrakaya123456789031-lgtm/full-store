// pixeldrain.cpp — https://pixeldrain.com/u/<id> → https://pixeldrain.com/api/file/<id>
#include "resolver.h"
#include "http_client.h"
#include "logger.h"

#include <cstdlib>
#include <regex>
#include <string>

namespace fs_resolve::pixeldrain {

std::optional<Resolved> resolve(const std::string& page_url) {
    // Extract id from /u/<id>, /l/<id>, or raw /<id>.
    static const std::regex re(R"(pixeldr(?:ain|a\.in)\.com/(?:u/|l/)?([A-Za-z0-9]+))",
                                std::regex::icase);
    std::smatch m;
    if (!std::regex_search(page_url, m, re)) return std::nullopt;
    std::string id = m[1].str();

    Resolved r;
    r.direct_url = "https://pixeldrain.com/api/file/" + id + "?download";

    // Hit info endpoint for size + filename.
    std::string info = "https://pixeldrain.com/api/file/" + id + "/info";
    auto resp = fs_net::get(info);
    if (resp.ok()) {
        static const std::regex rn(R"("name"\s*:\s*"([^"]+)")");
        static const std::regex rs(R"("size"\s*:\s*(\d+))");
        std::smatch mm;
        if (std::regex_search(resp.body, mm, rn)) r.suggested_filename = mm[1].str();
        if (std::regex_search(resp.body, mm, rs))
            r.size_bytes = std::strtoll(mm[1].str().c_str(), nullptr, 10);
    }
    LOGI("pixeldrain id=%s size=%lld name=%s",
         id.c_str(), (long long)r.size_bytes, r.suggested_filename.c_str());
    return r;
}

} // namespace fs_resolve::pixeldrain
