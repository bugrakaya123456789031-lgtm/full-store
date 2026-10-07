// onefichier.cpp — public (no-wait-free-tier) link resolution.
// Flow: GET page -> parse the form token -> POST adz / submit to receive the
// final direct URL. Free tier may impose a wait; this returns nullopt in that
// case so UI can surface a human message.
#include "resolver.h"
#include "http_client.h"
#include "logger.h"

#include <regex>
#include <string>

namespace fs_resolve::onefichier {

std::optional<Resolved> resolve(const std::string& page_url) {
    auto page = fs_net::get(page_url);
    if (!page.ok()) return std::nullopt;

    // Look for a wait notice -> return nullopt (caller should retry later).
    if (page.body.find("You must wait") != std::string::npos ||
        page.body.find("vous devez attendre") != std::string::npos) {
        LOGW("1fichier wait required for %s", page_url.c_str());
        return std::nullopt;
    }

    // The page's download form posts to itself with the "dl" button.
    // The result is a redirect to a direct URL on cdn-X.1fichier.com.
    static const std::regex rh(R"(href="(https?://[a-z0-9\.-]*1fichier\.com/[^"]+)")");
    std::smatch m;
    if (std::regex_search(page.body, m, rh)) {
        Resolved r;
        r.direct_url = m[1].str();
        r.headers.push_back({"Referer", page_url});
        LOGI("1fichier direct=%s", r.direct_url.c_str());
        return r;
    }

    // Fallback: look for the <input name="adz" value="..."> and POST.
    static const std::regex ra(R"(name="adz"\s+value="([^"]+)")");
    std::smatch am;
    if (std::regex_search(page.body, am, ra)) {
        // libcurl POST — reuse GET with a tiny workaround: we only expose GET,
        // so emit a warning and let the user hit the site via browser.
        LOGW("1fichier needs POST (adz=%s), not implemented — add premium cookie or paste direct URL",
             am[1].str().c_str());
    }
    return std::nullopt;
}

} // namespace fs_resolve::onefichier
