// extractor.h — unpack archive files via libarchive. Streams, progress callback.
#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace fs_ext {

enum class ArchiveKind {
    NONE,       // not an archive — raw file (pkg/eboot already)
    ZIP,
    SEVENZ,
    RAR,
    TAR,
    TAR_GZ,
    TAR_XZ,
    TAR_BZ2,
    UNKNOWN
};

ArchiveKind detect(const std::string& path);
const char* kind_label(ArchiveKind k);

using ExtractProgressCb = std::function<void(const std::string& entry,
                                             int64_t bytes_done,
                                             int64_t bytes_total)>;

// Extracts archive at path into out_dir (created if missing). On any failure
// returns false with reason in err. cancel_flag aborts cleanly.
bool extract(const std::string& archive_path,
             const std::string& out_dir,
             ExtractProgressCb progress,
             std::atomic<bool>& cancel_flag,
             std::string& err);

} // namespace fs_ext
