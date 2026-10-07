// config.h — compile-time constants and runtime defaults.
#pragma once

#include <string>

namespace fs_cfg {

// Base dirs on PS5.
inline constexpr const char* APP_DATA_DIR     = "/data/full-store";
inline constexpr const char* APP_CACHE_DIR    = "/data/full-store/cache";
inline constexpr const char* APP_TEMP_DIR     = "/data/full-store/tmp";
inline constexpr const char* APP_COVERS_DIR   = "/data/full-store/covers";
inline constexpr const char* APP_CONFIG_FILE  = "/data/full-store/config.json";
inline constexpr const char* APP_STATE_FILE   = "/data/full-store/state.json";
inline constexpr const char* APP_LOG_FILE     = "/data/full-store/full-store.log";

// Default install targets.
inline constexpr const char* DEFAULT_INSTALL_PATH = "/data/etaHEN/games";
inline constexpr const char* ETAHEN_FPKG_PATH     = "/data/etaHEN/games";
inline constexpr const char* ETAHEN_FPFSC_PATH    = "/data/etaHEN/fpfsc";
inline constexpr const char* USB_INSTALL_PATH     = "/mnt/usb0/PS5/CUSA00000";

// Source site.
inline constexpr const char* SOURCE_BASE_URL  = "https://dlpsgame.com";
inline constexpr const char* SOURCE_CATEGORY  = "/category/ps5/";

// Networking defaults.
inline constexpr int  DEFAULT_PARALLEL_DOWNLOADS = 2;    // simultaneous games
inline constexpr int  DEFAULT_CHUNKS_PER_FILE    = 8;    // Range-request chunks per file
inline constexpr long DEFAULT_CHUNK_SIZE_MIN     = 4L * 1024 * 1024;   // 4 MB
inline constexpr long DEFAULT_CONNECT_TIMEOUT    = 20L;  // seconds
inline constexpr long DEFAULT_LOW_SPEED_LIMIT    = 1024L;// bytes/sec
inline constexpr long DEFAULT_LOW_SPEED_TIME     = 30L;  // seconds

// UI defaults.
inline constexpr int  WINDOW_W = 1920;
inline constexpr int  WINDOW_H = 1080;
inline constexpr int  GRID_COLS = 5;

// Spoof a modern browser so Cloudflare doesn't frown.
inline constexpr const char* USER_AGENT =
    "Mozilla/5.0 (PlayStation; PlayStation 5/2.26) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/13.0 Safari/605.1.15";

} // namespace fs_cfg
