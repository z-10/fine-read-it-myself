#include "setup.h"

#include "llm_presets.h"
#include "net.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace rm {

namespace fs = std::filesystem;

// ModernBookNLP / BookNLP converted to GGUF (booknlp/convert.h, `readmyself-server --convert-booknlp`), MIT: https://huggingface.co/zloezlo/modernbooknlp-gguf
static const char * kBooknlpGguf = "https://huggingface.co/zloezlo/modernbooknlp-gguf/resolve/main/";

Setup::Setup(const Deploy & d, Engines & engines) : d_(d), engines_(engines) {
    const fs::path bn = d.booknlp_models;
    items_ = {
        {"tts", "Speech model: VoxCPM2 (OpenBMB, GGUF by audio.cpp)", "Apache-2.0",
         "https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/main/VoxCPM2-GGUF/voxcpm2-q8_0.gguf", d.tts_model, 2955000480, ""},
        {"bnlp-entities", "Speaker detection: BookNLP entity model (David Bamman)", "MIT",
         std::string(kBooknlpGguf) + "bnlp-entities.gguf", bn / "bnlp-entities.gguf", 157604896, ""},
        {"bnlp-coref", "Speaker detection: BookNLP coreference model (David Bamman)", "MIT",
         std::string(kBooknlpGguf) + "bnlp-coref.gguf", bn / "bnlp-coref.gguf", 225542048, ""},
        {"bnlp-quote", "Speaker detection: ModernBookNLP quote attribution (Gaspard Michel)", "MIT",
         std::string(kBooknlpGguf) + "bnlp-quote.gguf", bn / "bnlp-quote.gguf", 808627008, ""},
    };
    // local script producers (llm_presets.h): the default one is required, the bigger ones optional
    for (const auto & p : llm_presets())
        items_.push_back({p.required ? std::string("llm") : "llm:" + p.id,
                          "Script producer model: " + p.title + " Q4_K_M (" + std::to_string(static_cast<int>(p.vram_gb)) +
                              " GB VRAM, " + [&] {
                                  char a[16];
                                  std::snprintf(a, sizeof a, "%.1f%%", p.accuracy);
                                  return std::string(a);
                              }() + " speaker check)",
                          p.license, p.url, llm_path(d, p), p.size, p.required ? "" : "optional"});
}

Setup::~Setup() {
    if (thread_.joinable()) thread_.join();
}

bool Setup::pool_built() const { return fs::exists(d_.pool_dir() / "pool.json"); }

bool Setup::present(const Item & it) const {
    return fs::exists(it.dest);
}

bool Setup::complete() const {
    for (const auto & it : items_)
        if (it.group != "optional" && !present(it)) return false;
    return pool_built();
}

json Setup::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    json items = json::array();
    int64_t missing = 0;
    for (const auto & it : items_) {
        const bool ok = present(it);
        if (!ok && it.group != "optional") missing += it.size;
        items.push_back({{"id", it.id}, {"title", it.title}, {"license", it.license}, {"size", it.size},
                         {"installed", ok}, {"group", it.group}});
    }
    // the voice pool ships with the app (built from LibriTTS-R by `readmyself-server --build-voice-pool`)
    items.push_back({{"id", "voices"}, {"title", "Voice pool: LibriTTS-R readers (LibriVox), included with the app"},
                     {"license", "CC BY 4.0"}, {"size", 0}, {"installed", pool_built()}, {"group", "bundled"}});
    return {{"models_dir", d_.models_dir.u8string()}, {"voices_dir", d_.pool_dir().u8string()}, {"items", items}, {"complete", complete()},
            {"missing_bytes", missing}, {"installing", running_.load()}, {"current", current_},
            {"done_bytes", done_bytes_}, {"total_bytes", total_bytes_}, {"error", error_}, {"log", log_}};
}

void Setup::log(const std::string & m) {
    const std::time_t t = std::time(nullptr);
    char ts[16];
    std::strftime(ts, sizeof ts, "%H:%M:%S ", std::localtime(&t));
    std::lock_guard<std::mutex> lk(mu_);
    log_.push_back(ts + m);
    if (log_.size() > 200) log_.erase(log_.begin());
}

void Setup::install(const std::vector<std::string> & optional) {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        error_.clear();
    }
    thread_ = std::thread([this, optional] {
        try {
            run(optional);
        } catch (const std::exception & e) {
            std::lock_guard<std::mutex> lk(mu_);
            error_ = e.what();
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            current_.clear();
        }
        log(error_.empty() ? "setup finished" : "setup stopped: " + error_);
        running_ = false;
    });
}

void Setup::run(std::vector<std::string> optional) {
    int64_t total = 0;
    std::vector<const Item *> todo;
    for (const auto & it : items_)
        if (!present(it) && (it.group != "optional" || std::find(optional.begin(), optional.end(), it.id) != optional.end())) {
            todo.push_back(&it);
            total += it.size;
        }
    {
        std::lock_guard<std::mutex> lk(mu_);
        total_bytes_ = total;
        done_bytes_ = 0;
    }
    int64_t finished = 0;
    for (const Item * it : todo) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            current_ = it->title;
        }
        log("downloading " + it->title);
        http_download(it->url, it->dest, [&](int64_t done, int64_t) {
            std::lock_guard<std::mutex> lk(mu_);
            done_bytes_ = finished + done;
        });
        finished += it->size;
    }
    if (fs::exists(d_.booknlp_models / "src")) fs::remove_all(d_.booknlp_models / "src");   // originals of older installs
}

}  // namespace rm
