// resolver.h — file-host resolvers. Each handler takes a host "page" URL
// (pixeldrain.com/u/XXX, buzzheavier.com/XXX, 1fichier.com/?XXX, ...)
// and returns a direct downloadable URL + any required headers.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "http_client.h"

namespace fs_resolve {

struct Resolved {
    std::string direct_url;
    std::vector<fs_net::Header> headers;   // cookies etc.
    int64_t size_bytes = -1;
    std::string suggested_filename;
};

// Returns true if we can handle this host at all.
bool supported(const std::string& url);

// Returns short host label — matches fs_scrape::DownloadLink::host_label.
std::string host_of(const std::string& url);

// Resolve a page URL into a direct URL. Returns nullopt on failure.
std::optional<Resolved> resolve(const std::string& page_url);

} // namespace fs_resolve
