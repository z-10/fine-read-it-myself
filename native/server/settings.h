// Two kinds of configuration:
//
// Deploy - where data and models are and which GPU to use. From environment variables / command line
//   only (READMYSELF_DATA, READMYSELF_MODELS, READMYSELF_TTS_MODEL, READMYSELF_LLM_MODEL,
//   READMYSELF_BOOKNLP_MODELS, READMYSELF_VOICES, READMYSELF_GPU, READMYSELF_GPU_DEVICE); never stored.
// Settings - choices the user makes in the UI, stored in <data>/settings.json.
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rm {

namespace fs = std::filesystem;

struct Deploy {
    fs::path data_dir, models_dir, tts_model, llm_model, booknlp_models;
    fs::path voices_dir;   // voice pool shipped with the app (read-only): pool.json, <id>.wav/.txt, samples/
    std::string gpu;      // "vulkan", "metal", "cpu" or "" (best available)
    int gpu_device = 0;
    fs::path db_path() const { return data_dir / "state.sqlite"; }
    fs::path audio_dir() const { return data_dir / "audio"; }
    fs::path work_dir() const { return data_dir / "work"; }
    fs::path pool_dir() const { return voices_dir; }
    fs::path downloaded_voices_dir() const { return data_dir / "voices" / "downloaded"; }   // from the voice catalog
    std::vector<fs::path> pool_dirs() const { return {voices_dir, downloaded_voices_dir()}; }
    fs::path voice_overrides() const { return data_dir / "voices" / "overrides.json"; }   // the user's flips/bans
};

struct DirectorProfile {   // an OpenAI-compatible chat endpoint used as audiobook director
    std::string name, base_url, api_key, model;
    bool json_schema = true;
};

// the tested LibriTTS-R voices (CC BY 4.0) on the Hugging Face hub: pool.json, <id>.wav/.txt, samples/<id>.wav
inline const char * kDefaultVoiceCatalog = "https://huggingface.co/datasets/zloezlo/fine-read-it-voices/resolve/main/";

struct Settings {
    std::string default_director = "local";
    std::vector<DirectorProfile> directors;
    std::optional<fs::path> user_sources_dir;
    double check_interval_hours = 6.0;
    std::string mp3_bitrate = "96k";
    std::string voice_catalog = kDefaultVoiceCatalog;   // base URL of the downloadable voices (pool.json + files)

    const DirectorProfile * director_profile(const std::string & name) const;
    nlohmann::json to_json(bool mask_keys) const;
    // throws std::invalid_argument on bad values
    static Settings from_json(const nlohmann::json & j);
};

Deploy load_deploy(const std::optional<fs::path> & data_dir, const std::optional<fs::path> & models_dir,
                   const std::optional<fs::path> & voices_dir = std::nullopt);
Settings load_settings(const Deploy & d);
void save_settings(const Deploy & d, const Settings & s);

}  // namespace rm
