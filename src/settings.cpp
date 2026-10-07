// settings.cpp — hand-rolled minimal JSON so we don't pull a parser just for this.
#include "settings.h"
#include "config.h"
#include "logger.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs_set {

namespace {
    Settings g_settings = Settings::defaults();

    std::string esc(const std::string& s) {
        std::string o; o.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\r': o += "\\r";  break;
                case '\t': o += "\\t";  break;
                default:   o += c;      break;
            }
        }
        return o;
    }

    bool extract_string(const std::string& json, const std::string& key, std::string& out) {
        std::string pat = "\"" + key + "\"";
        auto k = json.find(pat);
        if (k == std::string::npos) return false;
        auto colon = json.find(':', k);
        if (colon == std::string::npos) return false;
        auto q1 = json.find('"', colon + 1);
        if (q1 == std::string::npos) return false;
        std::string val;
        for (size_t i = q1 + 1; i < json.size(); ++i) {
            char c = json[i];
            if (c == '\\' && i + 1 < json.size()) {
                char n = json[i + 1];
                switch (n) {
                    case 'n': val += '\n'; break;
                    case 'r': val += '\r'; break;
                    case 't': val += '\t'; break;
                    case '"': val += '"';  break;
                    case '\\': val += '\\';break;
                    default:  val += n;    break;
                }
                i++;
            } else if (c == '"') {
                out = val;
                return true;
            } else {
                val += c;
            }
        }
        return false;
    }

    bool extract_int(const std::string& json, const std::string& key, long& out) {
        std::string pat = "\"" + key + "\"";
        auto k = json.find(pat);
        if (k == std::string::npos) return false;
        auto colon = json.find(':', k);
        if (colon == std::string::npos) return false;
        size_t i = colon + 1;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) i++;
        char* end = nullptr;
        long v = std::strtol(json.c_str() + i, &end, 10);
        if (end == json.c_str() + i) return false;
        out = v;
        return true;
    }

    bool extract_bool(const std::string& json, const std::string& key, bool& out) {
        std::string pat = "\"" + key + "\"";
        auto k = json.find(pat);
        if (k == std::string::npos) return false;
        auto colon = json.find(':', k);
        if (colon == std::string::npos) return false;
        auto t = json.find("true",  colon);
        auto f = json.find("false", colon);
        auto brace = json.find('}', colon);
        if (t != std::string::npos && (brace == std::string::npos || t < brace)) {
            out = true;  return true;
        }
        if (f != std::string::npos && (brace == std::string::npos || f < brace)) {
            out = false; return true;
        }
        return false;
    }

    void ensure_dir(const std::string& p) {
        mkdir(p.c_str(), 0755);
    }
}

Settings Settings::defaults() {
    Settings s;
    s.install_path          = fs_cfg::DEFAULT_INSTALL_PATH;
    s.install_path_usb      = fs_cfg::USB_INSTALL_PATH;
    s.parallel_downloads    = fs_cfg::DEFAULT_PARALLEL_DOWNLOADS;
    s.chunks_per_file       = fs_cfg::DEFAULT_CHUNKS_PER_FILE;
    s.connect_timeout_sec   = fs_cfg::DEFAULT_CONNECT_TIMEOUT;
    s.low_speed_limit_bps   = fs_cfg::DEFAULT_LOW_SPEED_LIMIT;
    s.low_speed_time_sec    = fs_cfg::DEFAULT_LOW_SPEED_TIME;
    s.auto_extract          = true;
    s.delete_archive_after  = true;
    s.notify_etahen         = true;
    s.cookie_header         = "";
    s.ua_override           = "";
    s.ui_grid_cols          = fs_cfg::GRID_COLS;
    s.show_adult            = true;
    return s;
}

Settings& current() { return g_settings; }

bool load() {
    ensure_dir(fs_cfg::APP_DATA_DIR);
    FILE* f = std::fopen(fs_cfg::APP_CONFIG_FILE, "rb");
    if (!f) {
        LOGI("no config file, using defaults");
        return save();
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string js(sz, '\0');
    std::fread(js.data(), 1, sz, f);
    std::fclose(f);

    extract_string(js, "install_path",         g_settings.install_path);
    extract_string(js, "install_path_usb",     g_settings.install_path_usb);
    long v;
    if (extract_int(js, "parallel_downloads", v))  g_settings.parallel_downloads = (int)v;
    if (extract_int(js, "chunks_per_file",    v))  g_settings.chunks_per_file    = (int)v;
    if (extract_int(js, "connect_timeout_sec",v))  g_settings.connect_timeout_sec = v;
    if (extract_int(js, "low_speed_limit_bps",v))  g_settings.low_speed_limit_bps = v;
    if (extract_int(js, "low_speed_time_sec", v))  g_settings.low_speed_time_sec  = v;
    if (extract_int(js, "ui_grid_cols",       v))  g_settings.ui_grid_cols        = (int)v;
    extract_bool(js, "auto_extract",         g_settings.auto_extract);
    extract_bool(js, "delete_archive_after", g_settings.delete_archive_after);
    extract_bool(js, "notify_etahen",        g_settings.notify_etahen);
    extract_bool(js, "show_adult",           g_settings.show_adult);
    extract_string(js, "cookie_header",      g_settings.cookie_header);
    extract_string(js, "ua_override",        g_settings.ua_override);

    LOGI("settings loaded from %s", fs_cfg::APP_CONFIG_FILE);
    return true;
}

bool save() {
    ensure_dir(fs_cfg::APP_DATA_DIR);
    std::ostringstream o;
    const Settings& s = g_settings;
    o << "{\n"
      << "  \"install_path\": \""           << esc(s.install_path)       << "\",\n"
      << "  \"install_path_usb\": \""       << esc(s.install_path_usb)   << "\",\n"
      << "  \"parallel_downloads\": "       << s.parallel_downloads      << ",\n"
      << "  \"chunks_per_file\": "          << s.chunks_per_file         << ",\n"
      << "  \"connect_timeout_sec\": "      << s.connect_timeout_sec     << ",\n"
      << "  \"low_speed_limit_bps\": "      << s.low_speed_limit_bps     << ",\n"
      << "  \"low_speed_time_sec\": "       << s.low_speed_time_sec      << ",\n"
      << "  \"auto_extract\": "             << (s.auto_extract?"true":"false")        << ",\n"
      << "  \"delete_archive_after\": "     << (s.delete_archive_after?"true":"false")<< ",\n"
      << "  \"notify_etahen\": "            << (s.notify_etahen?"true":"false")       << ",\n"
      << "  \"show_adult\": "               << (s.show_adult?"true":"false")          << ",\n"
      << "  \"cookie_header\": \""          << esc(s.cookie_header)      << "\",\n"
      << "  \"ua_override\": \""            << esc(s.ua_override)        << "\",\n"
      << "  \"ui_grid_cols\": "             << s.ui_grid_cols            << "\n"
      << "}\n";
    FILE* f = std::fopen(fs_cfg::APP_CONFIG_FILE, "wb");
    if (!f) { LOGE("settings save failed: %s", fs_cfg::APP_CONFIG_FILE); return false; }
    std::string js = o.str();
    std::fwrite(js.data(), 1, js.size(), f);
    std::fclose(f);
    return true;
}

} // namespace fs_set
