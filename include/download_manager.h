// download_manager.h — queue, worker pool, persistent state.
// Pipeline per job: resolve -> download -> extract (if archive) -> install.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "scraper.h"

namespace fs_dl {

enum class Status {
    QUEUED,
    RESOLVING,
    DOWNLOADING,
    EXTRACTING,
    INSTALLING,
    COMPLETE,
    FAILED,
    CANCELLED
};

const char* status_label(Status s);

struct Job {
    uint64_t id = 0;
    std::string game_title;
    std::string cover_url;
    fs_scrape::DownloadLink link;
    std::string install_target;    // destination dir chosen at enqueue

    Status status = Status::QUEUED;
    std::string stage_detail;      // free text ("chunk 3/8", "unpack .pkg", ...)
    int64_t total_bytes = -1;
    int64_t done_bytes  = 0;
    double  speed_bps   = 0.0;
    double  progress    = 0.0;     // 0..1
    std::string error_msg;
    std::chrono::system_clock::time_point enqueued_at;
    std::chrono::system_clock::time_point finished_at;

    std::string temp_file_path;    // where download lands
    std::string final_install_path;// where installer put the result

    std::atomic<bool> cancel{false};
};

using JobPtr = std::shared_ptr<Job>;

class Manager {
public:
    static Manager& instance();

    void start(int worker_count);
    void stop();

    // Enqueue a download; returns the Job id.
    uint64_t enqueue(const std::string& game_title,
                     const std::string& cover_url,
                     const fs_scrape::DownloadLink& link,
                     const std::string& install_target);

    void cancel(uint64_t id);
    void retry(uint64_t id);
    void remove(uint64_t id);  // only when terminal state

    // Snapshots (copy out under lock) for UI.
    std::vector<JobPtr> active_snapshot();
    std::vector<JobPtr> completed_snapshot();
    std::vector<JobPtr> all_snapshot();

    void save_state();
    void load_state();

private:
    Manager() = default;
    void worker_loop();
    void run_job(JobPtr j);

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<JobPtr> pending_;
    std::vector<JobPtr> all_jobs_;   // full history
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> next_id_{1};
};

} // namespace fs_dl
