// buzzheavier.cpp — https://buzzheavier.com/<id> or /f/<id>
// The site exposes a direct link via /<id>/download or an HX-Redirect header
// returned from an HTMX request on the page's download button.
#include "resolver.h"
#include "http_client.h"
#include "logger.h"

#include <regex>
#include <string>

namespace fs_resolve::buzzheavier {

std::optional<Resolved> resolve(const std::string& page_url) {
    // Extract id.
    static const std::regex re(R"(buzzheavier\.com/(?:f/)?([A-Za-z0-9]+))",
                                std::regex::icase);
    std::smatch m;
    if (!std::regex_search(page_url, m, re)) return std::nullopt;
    std::string id = m[1].str();

    // Trigger the HTMX download endpoint, which responds with hx-redirect.
    std::vector<fs_net::Header> hx = {
        {"HX-Current-URL", page_url},
        {"HX-Request",     "true"},
        {"Referer",        page_url},
    };
    std::string trigger = "https://buzzheavier.com/" + id + "/download";
    auto resp = fs_net::get(trigger, hx);

    std::string redirect = resp.header("Hx-Redirect");
    if (redirect.empty()) redirect = resp.header("hx-redirect");
    if (redirect.empty()) redirect = resp.header("Location");

    Resolved r;
    if (!redirect.empty()) {
        r.direct_url = redirect;
    } else {
        // Fallback: scan body for data-url or source URL.
        static const std::regex rd(R"(data-url="([^"]+)")");
        std::smatch dm;
        if (std::regex_search(resp.body, dm, rd)) r.direct_url = dm[1].str();
    }
    if (r.direct_url.empty()) { LOGW("buzzheavier: no redirect for %s", id.c_str()); return std::nullopt; }

    // Pull filename from original page.
    auto page = fs_net::get(page_url);
    if (page.ok()) {
        static const std::regex rn(R"(<title>([^<]+)</title>)");
        std::smatch mm;
        if (std::regex_search(page.body, mm, rn)) r.suggested_filename = mm[1].str();
    }
    r.headers.push_back({"Referer", page_url});
    LOGI("buzzheavier id=%s -> %s", id.c_str(), r.direct_url.c_str());
    return r;
}

} // namespace fs_resolve::buzzheavier
