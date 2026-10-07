// installer.h — after download/extract, place content where etaHEN or the chosen
// loader expects it. Handles PKG files, exFAT dumps, decrypted dump trees.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "scraper.h"

namespace fs_install {

using InstallProgressCb = std::function<void(const std::string& stage,
                                             int64_t done, int64_t total)>;

struct Result {
    bool success = false;
    std::string final_path;   // where it ended up
    std::string err;
};

// src_path = file or directory (post-extract). target_root = install dir chosen
// in settings (e.g. /data/etaHEN/games). fmt hints routing but we also sniff.
Result install(const std::string& src_path,
               const std::string& target_root,
               fs_scrape::Fmt fmt,
               const std::string& game_title,
               InstallProgressCb progress,
               std::atomic<bool>& cancel_flag);

// Fire an etaHEN notification if daemon reachable (silently no-op if not).
void notify(const std::string& line);

} // namespace fs_install
