// extractor.cpp — libarchive wrapper. Handles zip, 7z, rar (read-only), tar + gzip/xz/bz2.
#include "extractor.h"
#include "logger.h"

#include <archive.h>
#include <archive_entry.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fs_ext {

ArchiveKind detect(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return ArchiveKind::UNKNOWN;
    unsigned char buf[8] = {0};
    std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);

    if (buf[0] == 'P' && buf[1] == 'K' && (buf[2] == 3 || buf[2] == 5)) return ArchiveKind::ZIP;
    if (buf[0] == 0x37 && buf[1] == 0x7A && buf[2] == 0xBC && buf[3] == 0xAF) return ArchiveKind::SEVENZ;
    if (buf[0] == 'R' && buf[1] == 'a' && buf[2] == 'r' && buf[3] == '!') return ArchiveKind::RAR;
    if (buf[0] == 0x1F && buf[1] == 0x8B) return ArchiveKind::TAR_GZ;
    if (buf[0] == 0xFD && buf[1] == '7' && buf[2] == 'z' && buf[3] == 'X') return ArchiveKind::TAR_XZ;
    if (buf[0] == 'B' && buf[1] == 'Z' && buf[2] == 'h') return ArchiveKind::TAR_BZ2;

    // PKG magic (Sony NPDRM): first 4 bytes 7F 43 4E 54 ("ï¿½CNT") or similar.
    if (buf[0] == 0x7F && buf[1] == 'C' && buf[2] == 'N' && buf[3] == 'T') return ArchiveKind::NONE;
    // ELF, raw dump, misc.
    return ArchiveKind::UNKNOWN;
}

const char* kind_label(ArchiveKind k) {
    switch (k) {
        case ArchiveKind::NONE:    return "raw";
        case ArchiveKind::ZIP:     return "zip";
        case ArchiveKind::SEVENZ:  return "7z";
        case ArchiveKind::RAR:     return "rar";
        case ArchiveKind::TAR:     return "tar";
        case ArchiveKind::TAR_GZ:  return "tar.gz";
        case ArchiveKind::TAR_XZ:  return "tar.xz";
        case ArchiveKind::TAR_BZ2: return "tar.bz2";
        default:                   return "?";
    }
}

namespace {

void mkpath(const std::string& dir) {
    std::string acc;
    for (size_t i = 0; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            if (!acc.empty()) mkdir(acc.c_str(), 0755);
        }
        if (i < dir.size()) acc += dir[i];
    }
}

int copy_data(struct archive* ar, struct archive* aw,
              int64_t& written_now, std::atomic<bool>& cancel) {
    const void* buff;
    size_t      size;
    la_int64_t  offset;
    for (;;) {
        if (cancel.load()) return ARCHIVE_FAILED;
        int r = archive_read_data_block(ar, &buff, &size, &offset);
        if (r == ARCHIVE_EOF) return ARCHIVE_OK;
        if (r < ARCHIVE_OK)   return r;
        r = archive_write_data_block(aw, buff, size, offset);
        if (r < ARCHIVE_OK)   return r;
        written_now += (int64_t)size;
    }
}

} // anon

bool extract(const std::string& archive_path,
             const std::string& out_dir,
             ExtractProgressCb progress,
             std::atomic<bool>& cancel_flag,
             std::string& err) {
    mkpath(out_dir);

    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    struct archive* ext = archive_write_disk_new();
    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                ARCHIVE_EXTRACT_ACL  | ARCHIVE_EXTRACT_FFLAGS |
                ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                ARCHIVE_EXTRACT_SECURE_SYMLINKS;
    archive_write_disk_set_options(ext, flags);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, archive_path.c_str(), 1024 * 1024) != ARCHIVE_OK) {
        err = archive_error_string(a);
        LOGE("open archive %s: %s", archive_path.c_str(), err.c_str());
        archive_read_free(a); archive_write_free(ext);
        return false;
    }

    // Estimate total via sum of entry sizes (one pass over header stream).
    int64_t total = 0;
    (void)total; // only used for callback hint

    struct archive_entry* entry;
    int64_t done = 0;
    bool ok = true;

    while (true) {
        if (cancel_flag.load()) { ok = false; err = "cancelled"; break; }
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_WARN) {
            err = archive_error_string(a);
            LOGE("header: %s", err.c_str());
            ok = false; break;
        }

        std::string name = archive_entry_pathname(entry);
        std::string full = out_dir + "/" + name;
        archive_entry_set_pathname(entry, full.c_str());

        r = archive_write_header(ext, entry);
        if (r < ARCHIVE_OK) {
            err = archive_error_string(ext);
            LOGE("write header %s: %s", name.c_str(), err.c_str());
        } else if (archive_entry_size(entry) > 0) {
            int64_t written = 0;
            r = copy_data(a, ext, written, cancel_flag);
            done += written;
            if (progress) progress(name, done, archive_entry_size(entry));
            if (r < ARCHIVE_OK) {
                err = archive_error_string(ext);
                LOGE("copy %s: %s", name.c_str(), err.c_str());
                ok = false; break;
            }
        }
        r = archive_write_finish_entry(ext);
        if (r < ARCHIVE_OK) {
            err = archive_error_string(ext);
            ok = false; break;
        }
        if (progress) progress(name, done, 0);
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    return ok;
}

} // namespace fs_ext
