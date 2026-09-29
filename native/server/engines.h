// In-process engines sharing one ggml: VoxCPM2 (speech), llama.cpp (local script producers, llm_presets.h) and the
// ModernBookNLP port. Each is loaded on first use and kept resident; calls are serialized per engine.
#pragma once

#include "settings.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rm::booknlp {
struct Result;
}

namespace rm {

using json = nlohmann::json;

struct Audio {
    int sample_rate = 0;
    std::vector<float> samples;   // mono
};

class Engines {
public:
    explicit Engines(const Deploy & d);
    ~Engines();

    // VoxCPM2 ultimate clone: reference clip as prompt audio + its transcript
    Audio speak(const std::string & text, const std::filesystem::path & ref_wav, const std::string & ref_text,
                uint32_t seed = 1234);
    // chat completion with a local model (llm_presets.h id); messages [{role, content}]; schema constrains the
    // output when not null. One model is resident at a time: asking for another unloads the current one.
    std::string chat(const json & messages, const json & schema, int max_tokens, float temperature = 0.2f,
                     const std::string & preset = "qwen3.5-4b");
    // ModernBookNLP on a chapter's text (paragraphs separated by blank lines)
    booknlp::Result booknlp(const std::string & chapter_text);

    // availability for /api/status (model files present)
    bool tts_available() const;
    bool llm_available(const std::string & preset = "qwen3.5-4b") const;
    bool booknlp_available() const;

private:
    struct Tts;
    struct Llm;
    struct Bnlp;
    const Deploy d_;
    std::mutex tts_mu_, llm_mu_, bnlp_mu_;
    std::unique_ptr<Tts> tts_;
    std::unique_ptr<Llm> llm_;
    std::unique_ptr<Bnlp> bnlp_;
};

}  // namespace rm
