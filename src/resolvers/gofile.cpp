// gofile.cpp — https://gofile.io/d/<id>. Requires creating a guest account to
// obtain a token used both as cookie and in Authorization header.
#include "resolver.h"
#include "http_client.h"
#include "logger.h"

#include <regex>
#include <string>

namespace fs_resolve::gofile {

namespace {

std::string create_guest_token() {
    auto r = fs_net::get("https://api.gofile.io/accounts", {{"Content-Type","application/json"}});
    if (!r.ok()) return "";
    static const std::regex rt(R"("token"\s*:\s*"([^"]+)")");
    std::smatch m;
    if (std::regex_search(r.body, m, rt)) return m[1].str();
    return "";
}

} // anon

std::optional<Resolved> resolve(const std::string& page_url) {
    static const std::regex ri(R"(gofile\.io/d/([A-Za-z0-9]+))", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(page_url, m, ri)) return std::nullopt;
    std::string id = m[1].str();

    std::string token = create_guest_token();
    if (token.empty()) { LOGW("gofile: guest token failed"); return std::nullopt; }

    std::string api = "https://api.gofile.io/contents/" + id + "?wt=4fd6sg89d7s6";
    std::vector<fs_net::Header> h = {
        {"Authorization", "Bearer " + token},
        {"Referer",       "https://gofile.io"},
    };
    auto r = fs_net::get(api, h);
    if (!r.ok()) return std::nullopt;

    static const std::regex rl(R"("link"\s*:\s*"([^"]+)")");
    std::smatch mm;
    if (!std::regex_search(r.body, mm, rl)) return std::nullopt;

    Resolved res;
    res.direct_url = mm[1].str();
    // Replace escaped slashes.
    for (size_t p = 0; (p = res.direct_url.find("\\/", p)) != std::string::npos; p += 1)
        res.direct_url.replace(p, 2, "/");

    res.headers.push_back({"Cookie",   "accountToken=" + token});
    res.headers.push_back({"Referer",  "https://gofile.io"});
    res.suggested_filename = fs_net::url_filename(res.direct_url);
    LOGI("gofile -> %s", res.direct_url.c_str());
    return res;
}

} // namespace fs_resolve::gofile
