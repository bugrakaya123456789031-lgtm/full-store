// http_client.h — libcurl facade: GET into memory, chunked parallel download to disk.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fs_net {

struct Header {
    std::string name;
    std::string value;
};

struct Response {
    long status = 0;
    std::string body;
    std::vector<Header> headers;
    bool ok() const { return status >= 200 && status < 300; }
    std::string header(const std::string& name) const;
};

// progress_cb(downloaded, total_or_zero, speed_bps) — return false to abort.
using ProgressCb = std::function<bool(int64_t downloaded, int64_t total, double speed_bps)>;

void global_init();
void global_shutdown();

// Simple GET returning the body in memory. Follows redirects, sends standard headers.
Response get(const std::string& url,
             const std::vector<Header>& extra = {});

// HEAD request — returns status + headers only (used to resolve size + Range support).
Response head(const std::string& url,
              const std::vector<Header>& extra = {});

// Streaming download to disk with resume + N parallel Range chunks.
// Returns true on complete success. cancel_flag lets caller abort mid-flight.
// If server doesn't support ranges, falls back to single-stream download.
struct DownloadOptions {
    int  chunks        = 8;      // parallel Range chunks (if server supports)
    long connect_tmo   = 20;
    long low_speed_bps = 1024;
    long low_speed_sec = 30;
    std::vector<Header> headers; // extra headers (e.g. host-specific cookies)
};

bool download_to_file(const std::string& url,
                      const std::string& out_path,
                      const DownloadOptions& opts,
                      ProgressCb progress,
                      std::atomic<bool>& cancel_flag);

// URL utilities.
std::string url_encode(const std::string& s);
std::string url_host(const std::string& url);
std::string url_filename(const std::string& url);

} // namespace fs_net
