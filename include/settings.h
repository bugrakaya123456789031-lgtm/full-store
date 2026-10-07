// settings.h — user-editable config persisted as JSON.
#pragma once

#include <string>

namespace fs_set {

struct Settings {
    std::string install_path;         // where installer drops final files
    std::string install_path_usb;     // alt path
    int    parallel_downloads;        // concurrent games
    int    chunks_per_file;           // Range chunks per file
    long   connect_timeout_sec;
    long   low_speed_limit_bps;
    long   low_speed_time_sec;
    bool   auto_extract;              // decompress on arrival
    bool   delete_archive_after;      // remove .zip/.pkg source after install
    bool   notify_etahen;             // ping etaHEN notification daemon
    std::string cookie_header;        // raw "Cookie:" value for CF/sites that need it
    std::string ua_override;          // override User-Agent if site demands
    int    ui_grid_cols;
    bool   show_adult;                // include flagged entries

    static Settings defaults();
};

Settings& current();
bool load();
bool save();

} // namespace fs_set
