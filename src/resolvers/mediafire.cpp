// mediafire.cpp — the direct URL is embedded as an <a id="downloadButton"
// href="https://download...mediafire.com/.../file.ext">.
#include "resolver.h"
#include "http_client.h"
#include "logger.h"

#include <regex>
#include <string>

namespace fs_resolve::mediafire {

std::optional<Resolved> resolve(const std::string& page_url) {
    auto page = fs_net::get(page_url);
    if (!page.ok()) return std::nullopt;

    static const std::regex re(
        R"re(id="downloadButton"[^>]*href="(https?://[^"]+mediafire\.com/[^"]+)")re",
        std::regex::icase);
    std::smatch m;
    if (!std::regex_search(page.body, m, re)) {
        // Alt: aria-label with download URL
        static const std::regex re2(
            R"re(href="(https?://download[0-9]+\.mediafire\.com/[^"]+)")re",
            std::regex::icase);
        if (!std::regex_search(page.body, m, re2)) return std::nullopt;
    }
    Resolved r;
    r.direct_url = m[1].str();
    r.headers.push_back({"Referer", page_url});
    // Extract filename from URL tail.
    r.suggested_filename = fs_net::url_filename(r.direct_url);
    LOGI("mediafire -> %s", r.direct_url.c_str());
    return r;
}

} // namespace fs_resolve::mediafire
