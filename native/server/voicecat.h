// The voice catalog: tested voices published on the Hugging Face hub (same layout as the pool shipped with the
// app). The Voices page lists the ones not installed yet; downloads land in <data>/voices/downloaded, one voice
// at a time (its pool.json entry is written after its files, so a stopped download leaves no broken voice).
#pragma once

#include "settings.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace rm {

class VoiceCatalog {
public:
    VoiceCatalog(const Deploy & d, std::function<std::string()> base_url) : d_(d), base_url_(std::move(base_url)) {}
    ~VoiceCatalog();
    // the catalog's voices (pool.json entries) + "sample_url"; throws std::runtime_error when unreachable
    nlohmann::json list();
    void download(const std::vector<std::string> & ids);   // in the background; ignored while one is running
    nlohmann::json state() const;                          // {running, done, total, current, error, log}
    // deletes a downloaded voice's files and pool entry (no-op for voices shipped with the app)
    void uninstall(const std::string & id);
    // Before narration: download the voices a cast uses that are missing here but in the catalog (e.g. after
    // reinstalling the app). Returns the ids installed; unreachable catalog: logs and returns what it could.
    std::vector<std::string> ensure(const std::vector<std::string> & ids, const std::function<void(const std::string &)> & log);

private:
    void run(std::vector<std::string> ids);
    // downloads each id not installed yet (throws on failure); on_voice(id) after each id
    void install(const std::vector<std::string> & ids, const std::function<void(const std::string &)> & on_voice);
    std::mutex install_mu_;   // one installer at a time (pool.json read-modify-write)
    void log(const std::string & m);
    const Deploy & d_;
    std::function<std::string()> base_url_;
    mutable std::mutex mu_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int done_ = 0, total_ = 0;
    std::string current_, error_;
    std::vector<std::string> log_;
    nlohmann::json cache_;   // catalog entries, fetched once per base URL
    std::string cache_url_;
};

}  // namespace rm
