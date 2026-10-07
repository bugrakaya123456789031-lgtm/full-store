// installer.cpp — final placement step. Figures out whether the result is a
// loose .pkg (drop into FPKG games dir), a decrypted dump folder (copy as-is),
// or an exFAT-ready set of files (same treatment, different root). Notifies
// etaHEN's daemon on success when configured.
#include "installer.h"
#include "config.h"
#include "http_client.h"
#include "logger.h"
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <vector>

namespace fs_install {

namespace {

bool is_dir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string ext_of(const std::string& p) {
    auto d = p.find_last_of('.');
    if (d == std::string::npos) return "";
    std::string e = p.substr(d);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return e;
}

void mkpath(const std::string& dir) {
    std::string acc;
    for (size_t i = 0; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            if (!acc.empty()) mkdir(acc.c_str(), 0755);
        }
        if (i < dir.size()) acc += dir[i];
    }
}

bool copy_file(const std::string& src, const std::string& dst,
               InstallProgressCb progress, int64_t& grand_done, int64_t grand_total,
               std::atomic<bool>& cancel) {
    int fi = ::open(src.c_str(), O_RDONLY);
    if (fi < 0) { LOGE("open src %s: %s", src.c_str(), strerror(errno)); return false; }
    int fo = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fo < 0) { LOGE("open dst %s: %s", dst.c_str(), strerror(errno)); ::close(fi); return false; }

    const size_t BUF = 4 * 1024 * 1024;
    std::vector<char> buf(BUF);
    while (true) {
        if (cancel.load()) { ::close(fi); ::close(fo); return false; }
        ssize_t n = ::read(fi, buf.data(), BUF);
        if (n == 0) break;
        if (n < 0) { LOGE("read: %s", strerror(errno)); ::close(fi); ::close(fo); return false; }
        ssize_t w = 0;
        while (w < n) {
            ssize_t m = ::write(fo, buf.data() + w, n - w);
            if (m <= 0) { LOGE("write: %s", strerror(errno)); ::close(fi); ::close(fo); return false; }
            w += m;
        }
        grand_done += n;
        if (progress) progress("copy " + src, grand_done, grand_total);
    }
    ::close(fi);
    ::close(fo);
    return true;
}

int64_t dir_size(const std::string& dir) {
    int64_t total = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string full = dir + "/" + n;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) total += dir_size(full);
        else                     total += st.st_size;
    }
    closedir(d);
    return total;
}

bool copy_tree(const std::string& src, const std::string& dst,
               InstallProgressCb progress, int64_t& done, int64_t total,
               std::atomic<bool>& cancel) {
    mkpath(dst);
    DIR* d = opendir(src.c_str());
    if (!d) { LOGE("opendir %s: %s", src.c_str(), strerror(errno)); return false; }
    dirent* e;
    bool ok = true;
    while ((e = readdir(d)) != nullptr) {
        if (cancel.load()) { ok = false; break; }
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string s_full = src + "/" + n;
        std::string d_full = dst + "/" + n;
        struct stat st{};
        if (stat(s_full.c_str(), &st) != 0) { ok = false; break; }
        if (S_ISDIR(st.st_mode)) {
            if (!copy_tree(s_full, d_full, progress, done, total, cancel)) { ok = false; break; }
        } else {
            if (!copy_file(s_full, d_full, progress, done, total, cancel)) { ok = false; break; }
        }
    }
    closedir(d);
    return ok;
}

// Find first .pkg inside a dir tree.
std::string find_pkg(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    dirent* e;
    std::string found;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string full = dir + "/" + n;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            found = find_pkg(full);
            if (!found.empty()) break;
        } else if (ext_of(n) == ".pkg") {
            found = full;
            break;
        }
    }
    closedir(d);
    return found;
}

// Detect decrypted dump: contains "eboot.bin" or "sce_sys" folder somewhere.
bool looks_like_dump(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    dirent* e;
    bool match = false;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n == "eboot.bin" || n == "sce_sys" || n == "sce_module") { match = true; break; }
    }
    closedir(d);
    return match;
}

// Sanitize game title into dir name.
std::string sanitize(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ')
            o += c;
        else o += '_';
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o.empty() ? "game" : o;
}

} // anon

Result install(const std::string& src_path,
               const std::string& target_root,
               fs_scrape::Fmt /*fmt*/,
               const std::string& game_title,
               InstallProgressCb progress,
               std::atomic<bool>& cancel_flag) {
    Result r;
    mkpath(target_root);
    if (progress) progress("inspect", 0, 0);

    // Case 1: src is a file.
    if (is_file(src_path)) {
        std::string e = ext_of(src_path);
        if (e == ".pkg") {
            // Drop into FPKG dir directly.
            std::string dst = target_root + "/" + fs_net::url_filename(src_path);
            int64_t total = 0, done = 0;
            {
                struct stat st{}; stat(src_path.c_str(), &st); total = st.st_size;
            }
            if (!copy_file(src_path, dst, progress, done, total, cancel_flag)) {
                r.err = "copy failed"; return r;
            }
            r.success = true;
            r.final_path = dst;
            notify("Installed PKG: " + game_title);
            return r;
        }
        // Any other loose file we don't know — stash under <target>/<title>/
        std::string sub = target_root + "/" + sanitize(game_title);
        mkpath(sub);
        std::string dst = sub + "/" + fs_net::url_filename(src_path);
        int64_t total = 0, done = 0;
        {
            struct stat st{}; stat(src_path.c_str(), &st); total = st.st_size;
        }
        if (!copy_file(src_path, dst, progress, done, total, cancel_flag)) {
            r.err = "copy failed"; return r;
        }
        r.success = true;
        r.final_path = dst;
        notify("Installed: " + game_title);
        return r;
    }

    // Case 2: src is a directory (post-extract).
    if (is_dir(src_path)) {
        // (a) If it contains a single top-level dir that's a dump, use that.
        std::string start = src_path;
        {
            DIR* d = opendir(src_path.c_str());
            if (d) {
                dirent* e; std::vector<std::string> ents;
                while ((e = readdir(d)) != nullptr) {
                    std::string n = e->d_name;
                    if (n != "." && n != "..") ents.push_back(n);
                }
                closedir(d);
                if (ents.size() == 1 && is_dir(src_path + "/" + ents[0])) {
                    start = src_path + "/" + ents[0];
                }
            }
        }

        // (b) PKG inside tree → ship the PKG only.
        std::string pkg = find_pkg(start);
        if (!pkg.empty()) {
            std::string dst = target_root + "/" + fs_net::url_filename(pkg);
            int64_t total = 0, done = 0;
            {
                struct stat st{}; stat(pkg.c_str(), &st); total = st.st_size;
            }
            if (!copy_file(pkg, dst, progress, done, total, cancel_flag)) {
                r.err = "pkg copy failed"; return r;
            }
            r.success = true;
            r.final_path = dst;
            notify("Installed PKG: " + game_title);
            return r;
        }

        // (c) Decrypted dump / exFAT layout — copy whole tree under <title>/.
        std::string game_dir = target_root + "/" + sanitize(game_title);
        if (!looks_like_dump(start)) {
            // Walk one level deeper for a CUSA folder.
            DIR* d = opendir(start.c_str());
            if (d) {
                dirent* e;
                while ((e = readdir(d)) != nullptr) {
                    std::string n = e->d_name;
                    if (n.rfind("CUSA", 0) == 0) {
                        start = start + "/" + n;
                        game_dir = target_root + "/" + n;
                        break;
                    }
                }
                closedir(d);
            }
        } else {
            // Keep dir name from source if it exists.
            auto slash = start.find_last_of('/');
            if (slash != std::string::npos)
                game_dir = target_root + "/" + start.substr(slash + 1);
        }

        int64_t total = dir_size(start), done = 0;
        if (!copy_tree(start, game_dir, progress, done, total, cancel_flag)) {
            r.err = "tree copy failed"; return r;
        }
        r.success = true;
        r.final_path = game_dir;
        notify("Installed dump: " + game_title);
        return r;
    }

    r.err = "src not found";
    return r;
}

// etaHEN exposes a plain TCP notify on port 9028 by default (configurable).
// If unreachable we silently skip — notification is a nice-to-have.
void notify(const std::string& line) {
    if (!fs_set::current().notify_etahen) return;

    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(9028);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // Short timeout so we don't hang if etaHEN isn't listening.
    timeval tv{2, 0};
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
        std::string msg = "notify " + line + "\n";
        ::send(s, msg.data(), msg.size(), 0);
    }
    ::close(s);
}

} // namespace fs_install
