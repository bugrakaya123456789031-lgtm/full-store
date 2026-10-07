// download_manager.cpp — queue + worker pool + per-job state machine.
#include "download_manager.h"
#include "config.h"
#include "extractor.h"
#include "http_client.h"
#include "installer.h"
#include "logger.h"
#include "resolver.h"
#include "settings.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs_dl {

const char* status_label(Status s) {
    switch (s) {
        case Status::QUEUED:      return "Queued";
        case Status::RESOLVING:   return "Resolving";
        case Status::DOWNLOADING: return "Downloading";
        case Status::EXTRACTING:  return "Extracting";
        case Status::INSTALLING:  return "Installing";
        case Status::COMPLETE:    return "Complete";
        case Status::FAILED:      return "Failed";
        case Status::CANCELLED:   return "Cancelled";
    }
    return "?";
}

Manager& Manager::instance() {
    static Manager m;
    return m;
}

void Manager::start(int worker_count) {
    if (running_.load()) return;
    running_.store(true);
    worker_count = std::max(1, worker_count);
    for (int i = 0; i < worker_count; i++) {
        workers_.emplace_back([this]{ worker_loop(); });
    }
    LOGI("download manager started with %d workers", worker_count);
}

void Manager::stop() {
    running_.store(false);
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
}

uint64_t Manager::enqueue(const std::string& game_title,
                          const std::string& cover_url,
                          const fs_scrape::DownloadLink& link,
                          const std::string& install_target) {
    auto j = std::make_shared<Job>();
    j->id = next_id_++;
    j->game_title = game_title;
    j->cover_url  = cover_url;
    j->link       = link;
    j->install_target = install_target;
    j->enqueued_at = std::chrono::system_clock::now();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_.push_back(j);
        all_jobs_.push_back(j);
    }
    cv_.notify_one();
    LOGI("enqueued #%llu '%s' via %s",
         (unsigned long long)j->id, game_title.c_str(), link.host_label.c_str());
    return j->id;
}

void Manager::cancel(uint64_t id) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& j : all_jobs_) if (j->id == id) j->cancel.store(true);
}

void Manager::retry(uint64_t id) {
    JobPtr found;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& j : all_jobs_) if (j->id == id &&
            (j->status == Status::FAILED || j->status == Status::CANCELLED)) {
            j->status = Status::QUEUED;
            j->cancel.store(false);
            j->done_bytes = 0;
            j->speed_bps = 0;
            j->progress = 0;
            j->error_msg.clear();
            j->stage_detail.clear();
            pending_.push_back(j);
            found = j;
            break;
        }
    }
    if (found) cv_.notify_one();
}

void Manager::remove(uint64_t id) {
    std::lock_guard<std::mutex> lk(mtx_);
    all_jobs_.erase(std::remove_if(all_jobs_.begin(), all_jobs_.end(),
        [id](const JobPtr& j){
            return j->id == id &&
                   (j->status == Status::COMPLETE ||
                    j->status == Status::FAILED   ||
                    j->status == Status::CANCELLED);
        }), all_jobs_.end());
}

std::vector<JobPtr> Manager::active_snapshot() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<JobPtr> o;
    for (auto& j : all_jobs_) {
        if (j->status != Status::COMPLETE &&
            j->status != Status::FAILED   &&
            j->status != Status::CANCELLED) o.push_back(j);
    }
    return o;
}

std::vector<JobPtr> Manager::completed_snapshot() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<JobPtr> o;
    for (auto& j : all_jobs_)
        if (j->status == Status::COMPLETE ||
            j->status == Status::FAILED   ||
            j->status == Status::CANCELLED) o.push_back(j);
    return o;
}

std::vector<JobPtr> Manager::all_snapshot() {
    std::lock_guard<std::mutex> lk(mtx_);
    return all_jobs_;
}

void Manager::worker_loop() {
    while (running_.load()) {
        JobPtr job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&]{ return !running_.load() || !pending_.empty(); });
            if (!running_.load()) break;
            if (pending_.empty())  continue;
            job = pending_.front();
            pending_.pop_front();
        }
        run_job(job);
    }
}

void Manager::run_job(JobPtr j) {
    LOGI("run #%llu '%s'", (unsigned long long)j->id, j->game_title.c_str());

    // 1) Resolve.
    j->status = Status::RESOLVING;
    j->stage_detail = "resolving " + j->link.host_label;
    auto res = fs_resolve::resolve(j->link.url);
    if (!res) {
        j->status = Status::FAILED;
        j->error_msg = "host resolver failed (" + j->link.host_label + ")";
        j->finished_at = std::chrono::system_clock::now();
        save_state();
        return;
    }

    // 2) Download.
    mkdir(fs_cfg::APP_TEMP_DIR, 0755);
    std::string suggested = res->suggested_filename;
    if (suggested.empty()) suggested = fs_net::url_filename(res->direct_url);
    if (suggested.empty()) suggested = "download.bin";
    j->temp_file_path = std::string(fs_cfg::APP_TEMP_DIR) + "/" +
                        std::to_string(j->id) + "_" + suggested;

    fs_net::DownloadOptions opts;
    const auto& s = fs_set::current();
    opts.chunks        = s.chunks_per_file;
    opts.connect_tmo   = s.connect_timeout_sec;
    opts.low_speed_bps = s.low_speed_limit_bps;
    opts.low_speed_sec = s.low_speed_time_sec;
    opts.headers       = res->headers;

    j->status = Status::DOWNLOADING;
    j->stage_detail = "downloading";
    j->total_bytes  = res->size_bytes;

    bool dok = fs_net::download_to_file(res->direct_url, j->temp_file_path,
                                        opts,
                                        [&](int64_t done, int64_t total, double speed) {
        j->done_bytes = done;
        if (total > 0) {
            j->total_bytes = total;
            j->progress    = (double)done / (double)total;
        }
        j->speed_bps = speed;
        return !j->cancel.load();
    }, j->cancel);

    if (j->cancel.load()) {
        j->status = Status::CANCELLED;
        j->finished_at = std::chrono::system_clock::now();
        ::unlink(j->temp_file_path.c_str());
        save_state();
        return;
    }
    if (!dok) {
        j->status = Status::FAILED;
        j->error_msg = "download failed";
        j->finished_at = std::chrono::system_clock::now();
        save_state();
        return;
    }

    // 3) Extract if archive and auto_extract is on.
    std::string install_src = j->temp_file_path;
    std::string extract_dir;

    if (s.auto_extract) {
        auto k = fs_ext::detect(j->temp_file_path);
        if (k != fs_ext::ArchiveKind::NONE && k != fs_ext::ArchiveKind::UNKNOWN) {
            j->status = Status::EXTRACTING;
            j->stage_detail = std::string("extracting ") + fs_ext::kind_label(k);
            extract_dir = std::string(fs_cfg::APP_TEMP_DIR) + "/" +
                          std::to_string(j->id) + "_x";
            mkdir(extract_dir.c_str(), 0755);
            std::string err;
            bool eok = fs_ext::extract(j->temp_file_path, extract_dir,
                [&](const std::string& e, int64_t d, int64_t t) {
                    j->stage_detail = "extract " + e;
                    if (t > 0) j->progress = (double)d / (double)t;
                }, j->cancel, err);
            if (!eok) {
                j->status = Status::FAILED;
                j->error_msg = "extract: " + err;
                j->finished_at = std::chrono::system_clock::now();
                save_state();
                return;
            }
            install_src = extract_dir;
        }
    }

    // 4) Install.
    j->status = Status::INSTALLING;
    j->stage_detail = "installing";
    auto ir = fs_install::install(install_src, j->install_target,
                                  j->link.fmt, j->game_title,
                                  [&](const std::string& stage, int64_t d, int64_t t) {
                                      j->stage_detail = stage;
                                      if (t > 0) j->progress = (double)d / (double)t;
                                  }, j->cancel);
    if (!ir.success) {
        j->status = Status::FAILED;
        j->error_msg = "install: " + ir.err;
        j->finished_at = std::chrono::system_clock::now();
        save_state();
        return;
    }
    j->final_install_path = ir.final_path;

    // 5) Cleanup.
    if (s.delete_archive_after) {
        ::unlink(j->temp_file_path.c_str());
    }
    if (!extract_dir.empty()) {
        // Best-effort rm -rf via libarchive's helpers would be nicer; shell-free
        // version left as a follow-up — we leave extract tmp in place.
        LOGI("extract tmp left at %s (manual cleanup)", extract_dir.c_str());
    }

    j->status = Status::COMPLETE;
    j->progress = 1.0;
    j->stage_detail = "done";
    j->finished_at = std::chrono::system_clock::now();
    save_state();
    LOGI("job #%llu COMPLETE -> %s",
         (unsigned long long)j->id, j->final_install_path.c_str());
}

void Manager::save_state() {
    // Minimal history dump — JSON lines of terminal jobs so restarts show them.
    FILE* f = std::fopen(fs_cfg::APP_STATE_FILE, "wb");
    if (!f) return;
    std::fputs("[\n", f);
    bool first = true;
    for (auto& j : all_jobs_) {
        if (j->status != Status::COMPLETE &&
            j->status != Status::FAILED   &&
            j->status != Status::CANCELLED) continue;
        if (!first) std::fputs(",\n", f);
        first = false;
        std::fprintf(f,
            "  {\"id\":%llu,\"title\":\"%s\",\"host\":\"%s\",\"fmt\":\"%s\","
            "\"status\":\"%s\",\"path\":\"%s\",\"err\":\"%s\"}",
            (unsigned long long)j->id,
            j->game_title.c_str(),
            j->link.host_label.c_str(),
            fs_scrape::fmt_label(j->link.fmt),
            status_label(j->status),
            j->final_install_path.c_str(),
            j->error_msg.c_str());
    }
    std::fputs("\n]\n", f);
    std::fclose(f);
}

void Manager::load_state() {
    // Non-essential; UI shows live jobs anyway. Stub for now.
    LOGI("load_state: not restoring prior jobs (log at %s)", fs_cfg::APP_STATE_FILE);
}

} // namespace fs_dl
