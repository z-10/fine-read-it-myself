#include "voicecat.h"

#include "net.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace rm {

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string base_of(std::string url) {
    if (!url.empty() && url.back() != '/') url += '/';
    return url;
}

VoiceCatalog::~VoiceCatalog() {
    if (thread_.joinable()) thread_.join();
}

json VoiceCatalog::list() {
    const std::string base = base_of(base_url_());
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (cache_url_ == base && cache_.is_array()) return cache_;
    }
    const HttpResponse r = http_get(base + "pool.json", {}, 60);
    if (!r.error.empty()) throw std::runtime_error("voice catalog " + base + ": " + r.error);
    if (r.status != 200) throw std::runtime_error("voice catalog " + base + ": HTTP " + std::to_string(r.status));
    json out = json::array();
    for (auto v : json::parse(r.body)) {
        if (v.contains("qa") && !v["qa"].value("ok", true)) continue;
        v["sample_url"] = base + "samples/" + v["id"].get<std::string>() + ".wav";
        out.push_back(v);
    }
    std::lock_guard<std::mutex> lk(mu_);
    cache_ = out;
    cache_url_ = base;
    return out;
}

void VoiceCatalog::download(const std::vector<std::string> & ids) {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        error_.clear();
        done_ = 0;
        total_ = static_cast<int>(ids.size());
    }
    thread_ = std::thread([this, ids] {
        try {
            run(ids);
        } catch (const std::exception & e) {
            std::lock_guard<std::mutex> lk(mu_);
            error_ = e.what();
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            current_.clear();
        }
        log(error_.empty() ? "voices downloaded" : "download stopped: " + error_);
        running_ = false;
    });
}

void VoiceCatalog::run(std::vector<std::string> ids) {
    install(ids, [this](const std::string &) {
        std::lock_guard<std::mutex> lk(mu_);
        ++done_;
    });
}

std::vector<std::string> VoiceCatalog::ensure(const std::vector<std::string> & ids, const std::function<void(const std::string &)> & log_fn) {
    std::vector<std::string> got;
    try {
        const json catalog = list();
        std::vector<std::string> todo;
        std::string names;
        for (const auto & id : ids)
            for (const auto & v : catalog)
                if (v["id"] == id) {
                    todo.push_back(id);
                    names += (names.empty() ? "" : ", ") + id;
                }
        if (todo.empty()) return got;
        log_fn("downloading voices from the voice catalog: " + names);
        install(todo, [&](const std::string & id) { got.push_back(id); });
    } catch (const std::exception & e) {
        log_fn(std::string("voice catalog: ") + e.what());
    }
    return got;
}

void VoiceCatalog::install(const std::vector<std::string> & ids, const std::function<void(const std::string &)> & on_voice) {
    std::lock_guard<std::mutex> guard(install_mu_);
    const std::string base = base_of(base_url_());
    const json catalog = list();
    const fs::path dir = d_.downloaded_voices_dir();
    fs::create_directories(dir / "samples");
    const fs::path pool_file = dir / "pool.json";
    json pool = json::array();
    if (fs::exists(pool_file)) {
        std::ifstream in(pool_file, std::ios::binary);
        pool = json::parse(in);
    }
    auto installed = [&](const std::string & id) {
        for (const auto & v : pool)
            if (v["id"] == id) return true;
        return fs::exists(d_.pool_dir() / (id + ".wav"));
    };
    for (const auto & id : ids) {
        const json * entry = nullptr;
        for (const auto & v : catalog)
            if (v["id"] == id) entry = &v;
        if (!entry) throw std::runtime_error("voice " + id + " is not in the catalog");
        if (!installed(id)) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                current_ = id;
            }
            for (const std::string f : {id + ".wav", id + ".txt", "samples/" + id + ".wav"})
                http_download(base + f, dir / fs::u8path(f), [](int64_t, int64_t) {});
            json v = *entry;
            v.erase("sample_url");
            pool.push_back(v);
            const fs::path tmp = dir / "pool.json.tmp";
            std::ofstream(tmp, std::ios::binary) << pool.dump(1);
            fs::rename(tmp, pool_file);   // the voice exists only once its files are complete
            log("downloaded " + id + " (" + v.value("reader", std::string()) + ")");
        }
        on_voice(id);
    }
}

void VoiceCatalog::log(const std::string & m) {
    std::lock_guard<std::mutex> lk(mu_);
    log_.push_back(m);
    if (log_.size() > 100) log_.erase(log_.begin());
}

json VoiceCatalog::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {{"running", running_.load()}, {"done", done_}, {"total", total_}, {"current", current_}, {"error", error_}, {"log", log_}};
}

}  // namespace rm
