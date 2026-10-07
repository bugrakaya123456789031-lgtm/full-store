// scraper.h — dlpsgame.com crawler. Produces GameEntry list from category pages,
// resolves per-game detail into GameDetail with every download variant found.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace fs_scrape {

// Format classification for a given download link.
enum class Fmt {
    PKG,        // .pkg (fake PKG / backup PKG)
    FPKG,       // explicitly labeled "FPKG"
    DUMP,       // decrypted dump (folder structure / tar of CUSA)
    EXFAT,      // exFAT-ready layout (ready for USB → PS5)
    ARCHIVE,    // generic .zip / .7z / .rar container
    UNKNOWN
};

const char* fmt_label(Fmt f);

struct DownloadLink {
    std::string host_label;   // "pixeldrain", "buzzheavier", "1fichier", "mega", ...
    std::string url;          // raw page link as found on the game page
    std::string label;        // link's visible text (region / version / part)
    std::string region;       // US/EU/JP/Multi (parsed from label when possible)
    std::string version;      // "v1.07" if present
    Fmt         fmt = Fmt::UNKNOWN;
    int64_t     size_bytes = -1;  // -1 if unknown (resolved later via HEAD)
    int         part_index = 0;   // for multi-part sets
    int         part_total = 0;
};

struct GameEntry {
    std::string title;
    std::string page_url;     // dlpsgame.com/<slug>
    std::string cover_url;    // thumbnail
    std::string short_meta;   // e.g. "FPKG | US | 1.07"
};

struct GameDetail {
    GameEntry entry;
    std::string description;
    std::vector<DownloadLink> links;
};

// Fetch a category page (1-based). Returns parsed entries + whether more pages exist.
struct CategoryPage {
    std::vector<GameEntry> entries;
    int  page = 1;
    int  last_page = 1;      // best-effort from pagination block
    bool more = false;
};

CategoryPage fetch_category(int page);

// Fetch and parse a game detail page.
GameDetail fetch_detail(const GameEntry& e);

// Classify a URL + label into a Fmt (used internally, exposed for tests).
Fmt classify(const std::string& label, const std::string& url);

} // namespace fs_scrape
