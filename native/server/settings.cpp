#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace rm {

using json = nlohmann::json;

static std::string env(const char * k, const std::string & dflt = "") {
    const char * v = std::getenv(k);
    return v && *v ? v : dflt;
}

const DirectorProfile * Settings::director_profile(const std::string & name) const {
    for (const auto & d : directors)
        if (d.name == name) return &d;
    return nullptr;
}

json Settings::to_json(bool mask_keys) const {
    json ds = json::array();
    for (const auto & d : directors)
        ds.push_back({{"name", d.name}, {"base_url", d.base_url},
                      {"api_key", mask_keys ? (d.api_key.empty() ? "" : "***") : d.api_key},
                      {"model", d.model}, {"json_schema", d.json_schema}});
    return {{"default_director", default_director},
            {"directors", ds},
            {"user_sources_dir", user_sources_dir ? json(user_sources_dir->u8string()) : json(nullptr)},
            {"check_interval_hours", check_interval_hours},
            {"mp3_bitrate", mp3_bitrate},
            {"voice_catalog", voice_catalog},
            {"share_network", share_network},
            {"share_port", share_port}};
}

Settings Settings::from_json(const json & j) {
    Settings s;
    try {
        if (j.contains("default_director")) s.default_director = j["default_director"].get<std::string>();
        if (j.contains("directors"))
            for (const auto & d : j["directors"]) {
                DirectorProfile p;
                p.name = d.at("name").get<std::string>();
                p.base_url = d.at("base_url").get<std::string>();
                p.api_key = d.value("api_key", "");
                p.model = d.at("model").get<std::string>();
                p.json_schema = d.value("json_schema", true);
                s.directors.push_back(p);
            }
        if (j.contains("user_sources_dir") && !j["user_sources_dir"].is_null())
            s.user_sources_dir = fs::u8path(j["user_sources_dir"].get<std::string>());
        if (j.contains("check_interval_hours")) s.check_interval_hours = j["check_interval_hours"].get<double>();
        if (j.contains("share_network")) s.share_network = j["share_network"].get<bool>();
        if (j.contains("share_port")) {
            s.share_port = j["share_port"].get<int>();
            if (s.share_port < 1024 || s.share_port > 65535) throw std::invalid_argument("share_port must be 1024-65535");
        }
        if (j.contains("mp3_bitrate")) {   // MPEG-1 layer III rates (the narration is 48 kHz mono)
            s.mp3_bitrate = j["mp3_bitrate"].get<std::string>();
            static const int kRates[] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
            const int kbps = std::atoi(s.mp3_bitrate.c_str());
            if (std::find(std::begin(kRates), std::end(kRates), kbps) == std::end(kRates))
                throw std::invalid_argument("mp3_bitrate must be one of 32k-320k MPEG-1 rates, e.g. 96k");
            s.mp3_bitrate = std::to_string(kbps) + "k";
        }
        if (j.contains("voice_catalog") && !j["voice_catalog"].get<std::string>().empty()) s.voice_catalog = j["voice_catalog"].get<std::string>();
    } catch (const json::exception & e) {
        throw std::invalid_argument(e.what());
    }
    return s;
}

Deploy load_deploy(const std::optional<fs::path> & data_dir, const std::optional<fs::path> & models_dir,
                   const std::optional<fs::path> & voices_dir) {
    Deploy d;
    d.data_dir = fs::absolute(data_dir ? *data_dir : fs::u8path(env("READMYSELF_DATA", "data")));
    d.models_dir = fs::absolute(models_dir ? *models_dir : fs::u8path(env("READMYSELF_MODELS", "models")));
    d.tts_model = fs::u8path(env("READMYSELF_TTS_MODEL", (d.models_dir / "voxcpm2-q8_0.gguf").u8string()));
    d.llm_model = fs::u8path(env("READMYSELF_LLM_MODEL", (d.models_dir / "Qwen3.5-4B-Q4_K_M.gguf").u8string()));
    d.booknlp_models = fs::u8path(env("READMYSELF_BOOKNLP_MODELS", (d.models_dir / "booknlp").u8string()));
    // the voice pool ships with the app (--voices); <data>/voices holds only the user's overrides
    if (voices_dir) d.voices_dir = fs::absolute(*voices_dir);
    else if (!env("READMYSELF_VOICES").empty()) d.voices_dir = fs::absolute(fs::u8path(env("READMYSELF_VOICES")));
    else d.voices_dir = d.data_dir / "voices";
    d.gpu = env("READMYSELF_GPU");
    d.gpu_device = std::atoi(env("READMYSELF_GPU_DEVICE", "0").c_str());
    return d;
}

Settings load_settings(const Deploy & d) {
    Settings s;
    const fs::path f = d.data_dir / "settings.json";
    if (fs::exists(f)) {
        std::ifstream in(f, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        if (text.rfind("\xEF\xBB\xBF", 0) == 0) text = text.substr(3);   // BOM from Windows editors
        json j = json::parse(text);
        // keys of the old Python service's deployment config are ignored
        for (const char * k : {"audiocpp_url", "llama_url", "llama_model", "booknlp_python", "tts_model", "data_dir"})
            j.erase(k);
        s = Settings::from_json(j);
    }
    save_settings(d, s);
    return s;
}

void save_settings(const Deploy & d, const Settings & s) {
    fs::create_directories(d.data_dir);
    std::ofstream(d.data_dir / "settings.json", std::ios::binary) << s.to_json(false).dump(2);
}

}  // namespace rm
