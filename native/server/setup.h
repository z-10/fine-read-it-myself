// Downloads the models the app needs, each from its publisher on Hugging Face (VoxCPM2, the default local
// script producer, ModernBookNLP / BookNLP as GGUF). Starts on its own at first launch; the optional script
// producer models are downloaded on request (Settings). The voice pool ships with the app (voicepool.h).
// Downloads resume (.part files); installation runs on its own thread.
#pragma once

#include "engines.h"
#include "settings.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rm {

using json = nlohmann::json;

class Setup {
public:
    Setup(const Deploy & d, Engines & engines);
    ~Setup();
    json state() const;            // items, installing, progress, log, error
    bool complete() const;         // all required models present (optional ones: extra local script producers)
    // start installing (no-op while running): what is required and missing, plus the optional items named
    void install(const std::vector<std::string> & optional = {});

private:
    struct Item {
        std::string id, title, license, url;
        std::filesystem::path dest;
        int64_t size;
        std::string group;         // "optional": on request only
    };
    bool present(const Item & it) const;
    bool pool_built() const;
    void run(std::vector<std::string> optional);
    void log(const std::string & m);

    const Deploy d_;
    Engines & engines_;
    std::vector<Item> items_;
    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    std::string current_, error_;
    int64_t done_bytes_ = 0, total_bytes_ = 0;
    std::vector<std::string> log_;
    std::thread thread_;
};

}  // namespace rm
