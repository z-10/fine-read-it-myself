// Local script-producer models (llama.cpp GGUF, run in-process). Each was measured on the speaker check
// (1,293 quotes from 6 books, ModernBookNLP alone: 84.9% right); only models that clearly improve on it are offered.
#pragma once

#include "settings.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rm {

struct LlmPreset {
    std::string id;       // producer name "local:<id>"
    std::string title;    // shown in the UI
    std::string family;   // chat format: "qwen35" (ChatML, thinking off) or "gemma4"
    std::string url;      // download (publisher's Hugging Face repo)
    std::string file;     // file name in the models folder
    int64_t size;         // bytes
    double vram_gb;       // GPU memory the model needs (weights + context)
    double accuracy;      // speaker check, % of quotes right
    std::string license;
    bool required;        // installed by Setup (the default producer)
};

inline const std::vector<LlmPreset> & llm_presets() {
    static const std::vector<LlmPreset> p = {
        {"qwen3.5-4b", "Qwen3.5-4B", "qwen35",
         "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/main/Qwen3.5-4B-Q4_K_M.gguf", "Qwen3.5-4B-Q4_K_M.gguf",
         2740937888, 4, 88.2, "Apache-2.0", true},
        {"qwen3.5-9b", "Qwen3.5-9B", "qwen35",
         "https://huggingface.co/unsloth/Qwen3.5-9B-GGUF/resolve/main/Qwen3.5-9B-Q4_K_M.gguf", "Qwen3.5-9B-Q4_K_M.gguf",
         5680522464, 8, 90.9, "Apache-2.0", false},
        {"gemma4-12b", "Gemma 4 12B", "gemma4",
         "https://huggingface.co/unsloth/gemma-4-12b-it-GGUF/resolve/main/gemma-4-12b-it-Q4_K_M.gguf", "gemma-4-12b-it-Q4_K_M.gguf",
         7121861440, 10, 92.1, "Apache-2.0", false},
    };
    return p;
}

inline const char * kDefaultLlm = "qwen3.5-4b";

inline const LlmPreset * find_llm_preset(const std::string & id) {
    for (const auto & p : llm_presets())
        if (p.id == id) return &p;
    return nullptr;
}

// "local" (older settings/novels) is the default model
inline std::string llm_of_producer(const std::string & producer) {
    if (producer == "local") return kDefaultLlm;
    if (producer.rfind("local:", 0) == 0) return producer.substr(6);
    return "";
}

inline std::filesystem::path llm_path(const Deploy & d, const LlmPreset & p) {
    return p.id == kDefaultLlm ? d.llm_model : d.models_dir / p.file;   // the default honours READMYSELF_LLM_MODEL
}

}  // namespace rm
