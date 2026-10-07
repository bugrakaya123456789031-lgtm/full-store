// scraper.cpp — crawls dlpsgame.com. The site is a WordPress install; we match
// the common post-card markup and extract titles, cover art, and detail URLs.
// Game detail pages carry link blocks; each anchor to a file host becomes a
// DownloadLink after classification.
#include "scraper.h"
#include "config.h"
#include "http_client.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>
#include <unordered_set>
#include <utility>

namespace fs_scrape {

const char* fmt_label(Fmt f) {
    switch (f) {
        case Fmt::PKG:     return "PKG";
        case Fmt::FPKG:    return "FPKG";
        case Fmt::DUMP:    return "DUMP";
        case Fmt::EXFAT:   return "exFAT";
        case Fmt::ARCHIVE: return "ARCHIVE";
        default:           return "?";
    }
}

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

bool contains_ci(const std::string& hay, const char* needle) {
    return lower(hay).find(needle) != std::string::npos;
}

// Strip HTML tags from a snippet.
std::string strip_tags(const std::string& s) {
    std::string o; o.reserve(s.size());
    bool in = false;
    for (char c : s) {
        if (c == '<') in = true;
        else if (c == '>') in = false;
        else if (!in) o += c;
    }
    // Decode a few common entities.
    static const std::pair<const char*, const char*> ents[] = {
        {"&amp;",  "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;","'"}, {"&nbsp;"," "},
        {"&#8211;","-"}, {"&#8217;","'"}, {"&#8220;","\""}, {"&#8221;","\""},
    };
    for (auto& e : ents) {
        size_t p = 0;
        while ((p = o.find(e.first, p)) != std::string::npos) {
            o.replace(p, std::string(e.first).size(), e.second);
            p += std::string(e.second).size();
        }
    }
    // Trim.
    auto a = o.find_first_not_of(" \t\n\r");
    auto b = o.find_last_not_of(" \t\n\r");
    return (a == std::string::npos) ? "" : o.substr(a, b - a + 1);
}

std::string make_abs(const std::string& maybe_rel) {
    if (maybe_rel.empty()) return maybe_rel;
    if (maybe_rel.rfind("http", 0) == 0) return maybe_rel;
    std::string base = fs_cfg::SOURCE_BASE_URL;
    if (maybe_rel.front() == '/') return base + maybe_rel;
    return base + "/" + maybe_rel;
}

// Known file-host domains that typically host game archives.
const std::vector<std::pair<const char*, const char*>>& host_table() {
    static const std::vector<std::pair<const char*, const char*>> t = {
        {"pixeldrain.com",     "pixeldrain"},
        {"pixeldra.in",        "pixeldrain"},
        {"buzzheavier.com",    "buzzheavier"},
        {"1fichier.com",       "1fichier"},
        {"mega.nz",            "mega"},
        {"mega.co.nz",         "mega"},
        {"gofile.io",          "gofile"},
        {"mediafire.com",      "mediafire"},
        {"rapidgator.net",     "rapidgator"},
        {"ddownload.com",      "ddownload"},
        {"nitroflare.com",     "nitroflare"},
        {"turbobit.net",       "turbobit"},
        {"katfile.com",        "katfile"},
        {"filecrypt.cc",       "filecrypt"},
        {"usersdrive.com",     "usersdrive"},
        {"up-4ever.net",       "up-4ever"},
        {"dropapk.to",         "dropapk"},
        {"anonfiles.com",      "anonfiles"},
        {"workupload.com",     "workupload"},
        {"krakenfiles.com",    "krakenfiles"},
    };
    return t;
}

std::string host_label_of(const std::string& url) {
    std::string u = lower(url);
    for (const auto& [host, label] : host_table()) {
        if (u.find(host) != std::string::npos) return label;
    }
    return fs_net::url_host(url);
}

bool looks_like_host(const std::string& url) {
    std::string u = lower(url);
    for (const auto& [host, _] : host_table()) {
        if (u.find(host) != std::string::npos) return true;
    }
    // Direct file link too.
    for (const char* ext : {".pkg", ".zip", ".7z", ".rar", ".iso"}) {
        if (u.rfind(ext) != std::string::npos) return true;
    }
    return false;
}

// Pull a candidate "label" for a link by looking at surrounding plain text.
std::string label_near(const std::string& html, size_t anchor_pos) {
    size_t start = anchor_pos > 160 ? anchor_pos - 160 : 0;
    size_t end   = std::min(html.size(), anchor_pos + 160);
    std::string window = html.substr(start, end - start);
    return strip_tags(window);
}

std::string region_from(const std::string& s) {
    std::string u = lower(s);
    if (u.find("multi")       != std::string::npos) return "Multi";
    if (u.find("usa")         != std::string::npos ||
        u.find(" us ")        != std::string::npos ||
        u.find("[us]")        != std::string::npos ||
        u.find("ntsc-u")      != std::string::npos) return "US";
    if (u.find("eur")         != std::string::npos ||
        u.find("[eu]")        != std::string::npos ||
        u.find(" eu ")        != std::string::npos ||
        u.find("pal")         != std::string::npos) return "EU";
    if (u.find("jpn")         != std::string::npos ||
        u.find("[jp]")        != std::string::npos ||
        u.find(" jp ")        != std::string::npos ||
        u.find("ntsc-j")      != std::string::npos) return "JP";
    if (u.find("asia")        != std::string::npos) return "Asia";
    return "";
}

std::string version_from(const std::string& s) {
    // match "v1.07", "v10.00", "1.07.00"
    static const std::regex re(R"((?:v|version[\s:]*)\s*(\d+\.\d+(?:\.\d+)?))",
                               std::regex::icase);
    std::smatch m;
    if (std::regex_search(s, m, re)) return "v" + m[1].str();
    return "";
}

} // anon

Fmt classify(const std::string& label, const std::string& url) {
    std::string s = lower(label + " " + url);
    if (s.find("fpkg")  != std::string::npos) return Fmt::FPKG;
    if (s.find(".pkg")  != std::string::npos) return Fmt::PKG;
    if (s.find("exfat") != std::string::npos) return Fmt::EXFAT;
    if (s.find("backport") != std::string::npos ||
        s.find("dump")  != std::string::npos ||
        s.find("decrypted") != std::string::npos) return Fmt::DUMP;
    if (s.find(".zip")  != std::string::npos ||
        s.find(".7z")   != std::string::npos ||
        s.find(".rar")  != std::string::npos ||
        s.find(".iso")  != std::string::npos) return Fmt::ARCHIVE;
    return Fmt::UNKNOWN;
}

CategoryPage fetch_category(int page) {
    CategoryPage cp;
    cp.page = page;

    std::string url = std::string(fs_cfg::SOURCE_BASE_URL) + fs_cfg::SOURCE_CATEGORY;
    if (page > 1) url += "page/" + std::to_string(page) + "/";

    auto r = fs_net::get(url);
    if (!r.ok()) {
        LOGW("category fetch %s -> %ld", url.c_str(), r.status);
        return cp;
    }

    // WordPress typical article block:
    //   <article class="post" ...> ... </article>
    // Fallback: look for h2.entry-title > a.
    static const std::regex re_article(
        R"(<article\b[^>]*>([\s\S]*?)</article>)",
        std::regex::icase);
    static const std::regex re_title(
        R"(<h[123][^>]*class="[^"]*(?:entry-title|post-title)[^"]*"[^>]*>\s*<a\s+[^>]*href="([^"]+)"[^>]*>([\s\S]*?)</a>)",
        std::regex::icase);
    static const std::regex re_cover(
        R"(<img\b[^>]*src="([^"]+)"[^>]*>)",
        std::regex::icase);
    static const std::regex re_meta(
        R"(<p[^>]*class="[^"]*(?:entry-meta|post-meta|excerpt)[^"]*"[^>]*>([\s\S]*?)</p>)",
        std::regex::icase);

    std::unordered_set<std::string> seen;
    auto it_begin = std::sregex_iterator(r.body.begin(), r.body.end(), re_article);
    auto it_end   = std::sregex_iterator();

    for (auto it = it_begin; it != it_end; ++it) {
        std::string blk = (*it)[1].str();
        GameEntry e;
        std::smatch m;
        if (std::regex_search(blk, m, re_title)) {
            e.page_url = m[1].str();
            e.title    = strip_tags(m[2].str());
        } else {
            // Fallback: any anchor with href containing the base + title attr.
            static const std::regex re_any_link(
                R"(<a\b[^>]*href="([^"]+)"[^>]*>([\s\S]*?)</a>)",
                std::regex::icase);
            if (std::regex_search(blk, m, re_any_link)) {
                e.page_url = m[1].str();
                e.title    = strip_tags(m[2].str());
            }
        }
        if (e.page_url.empty() || e.title.empty()) continue;
        if (seen.count(e.page_url)) continue;
        seen.insert(e.page_url);

        e.page_url = make_abs(e.page_url);

        if (std::regex_search(blk, m, re_cover)) e.cover_url = make_abs(m[1].str());
        if (std::regex_search(blk, m, re_meta))  e.short_meta = strip_tags(m[1].str());

        cp.entries.push_back(std::move(e));
    }

    // Fallback path: some themes don't use <article>. Match title+image independently.
    if (cp.entries.empty()) {
        auto t_begin = std::sregex_iterator(r.body.begin(), r.body.end(), re_title);
        for (auto it = t_begin; it != it_end; ++it) {
            GameEntry e;
            e.page_url = make_abs((*it)[1].str());
            e.title    = strip_tags((*it)[2].str());
            if (!seen.insert(e.page_url).second) continue;
            cp.entries.push_back(std::move(e));
        }
    }

    // Pagination hint.
    static const std::regex re_last(
        R"(page/(\d+)/[^"]*"\s*(?:class="[^"]*(?:last|page-numbers)[^"]*")?)",
        std::regex::icase);
    int max_seen = page;
    for (auto it = std::sregex_iterator(r.body.begin(), r.body.end(), re_last);
         it != it_end; ++it) {
        int p = std::atoi((*it)[1].str().c_str());
        if (p > max_seen) max_seen = p;
    }
    cp.last_page = max_seen;
    cp.more      = (page < max_seen);

    LOGI("category page %d: %zu entries (last_page=%d)",
         page, cp.entries.size(), cp.last_page);
    return cp;
}

GameDetail fetch_detail(const GameEntry& e) {
    GameDetail gd;
    gd.entry = e;

    auto r = fs_net::get(e.page_url);
    if (!r.ok()) {
        LOGW("detail fetch %s -> %ld", e.page_url.c_str(), r.status);
        return gd;
    }

    // Description paragraph.
    static const std::regex re_desc(
        R"(<div[^>]*class="[^"]*entry-content[^"]*"[^>]*>([\s\S]*?)</div>)",
        std::regex::icase);
    std::smatch dm;
    std::string body_region = r.body;
    if (std::regex_search(r.body, dm, re_desc)) {
        body_region = dm[1].str();
        gd.description = strip_tags(body_region).substr(0, 1500);
    }

    // All anchors in the content region.
    static const std::regex re_anchor(
        R"(<a\b[^>]*href="([^"]+)"[^>]*>([\s\S]*?)</a>)",
        std::regex::icase);

    std::unordered_set<std::string> seen_url;
    for (auto it = std::sregex_iterator(body_region.begin(), body_region.end(), re_anchor);
         it != std::sregex_iterator(); ++it) {
        std::string url   = (*it)[1].str();
        std::string label = strip_tags((*it)[2].str());

        if (!looks_like_host(url)) continue;
        if (!seen_url.insert(url).second) continue;

        // Enrich label with surrounding text.
        auto pos = it->position();
        std::string nearby = label_near(body_region, pos);
        std::string merged = label + " | " + nearby;

        DownloadLink dl;
        dl.url         = url;
        dl.label       = label.empty() ? host_label_of(url) : label;
        dl.host_label  = host_label_of(url);
        dl.fmt         = classify(merged, url);
        dl.region      = region_from(merged);
        dl.version     = version_from(merged);
        gd.links.push_back(std::move(dl));
    }

    // Guess part_index / part_total from labels.
    static const std::regex re_part(
        R"(part\s*(\d+)(?:\s*(?:of|/)\s*(\d+))?)",
        std::regex::icase);
    for (auto& dl : gd.links) {
        std::smatch pm;
        if (std::regex_search(dl.label, pm, re_part)) {
            dl.part_index = std::atoi(pm[1].str().c_str());
            if (pm[2].matched) dl.part_total = std::atoi(pm[2].str().c_str());
        }
    }

    LOGI("detail %s: %zu links", e.title.c_str(), gd.links.size());
    return gd;
}

} // namespace fs_scrape
