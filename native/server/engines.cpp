#include "engines.h"

#include "llm_presets.h"

#include "engine/framework/audio/wav_reader.h"
#include "engine/framework/runtime/session.h"
#include "engine/models/voxcpm2/loader.h"

#include "ggml-backend.h"
#include "json-schema-to-grammar.h"
#include "llama.h"

#include "pipeline.h"

#include <cmath>
#include <random>
#include <stdexcept>

namespace rm {

namespace rt = engine::runtime;

// ---------------------------------------------------------------- VoxCPM2

struct Engines::Tts {
    std::unique_ptr<rt::ILoadedVoiceModel> model;
    std::unique_ptr<rt::IVoiceTaskSession> session;
    rt::IOfflineVoiceTaskSession * offline = nullptr;
};

static engine::core::BackendType backend_type(const std::string & gpu) {
    if (gpu == "cpu") return engine::core::BackendType::Cpu;
    if (gpu == "metal") return engine::core::BackendType::Metal;
#ifdef __APPLE__
    if (gpu.empty()) return engine::core::BackendType::Metal;
#endif
    if (gpu.empty()) {   // best available: the CPU when there is no GPU driver (no Vulkan device)
        bool any_gpu = false;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i)
            any_gpu = any_gpu || ggml_backend_dev_type(ggml_backend_dev_get(i)) == GGML_BACKEND_DEVICE_TYPE_GPU;
        if (!any_gpu) return engine::core::BackendType::Cpu;
    }
    return engine::core::BackendType::Vulkan;
}

// Band-limited resampling (Kaiser-windowed sinc). The engine's own fallback without libsoxr is linear interpolation,
// which aliases the reference's 8-12 kHz into the voice prompt: VoxCPM2 clones that as hissing "s" endings.
static std::vector<float> resample(const std::vector<float> & x, int from, int to) {
    if (from == to || x.empty()) return x;
    const double ratio = double(to) / from, fc = 0.475 * std::min(1.0, ratio);   // cutoff, cycles per input sample
    const int half = static_cast<int>(std::ceil(16 / fc / 2));                    // ~16 zero crossings each side
    const double beta = 8.6, kTwoPi = 6.283185307179586;
    auto bessel_i0 = [](double v) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (v / (2 * k)) * (v / (2 * k)); s += t; } return s; };
    const double i0b = bessel_i0(beta);
    std::vector<float> y(static_cast<size_t>(std::llround(x.size() * ratio)));
    for (size_t n = 0; n < y.size(); ++n) {
        const double t = n / ratio;
        const long c = static_cast<long>(std::floor(t));
        double acc = 0;
        for (long k = c - half + 1; k <= c + half; ++k) {
            if (k < 0 || k >= static_cast<long>(x.size())) continue;
            const double d = t - k, r = d / half;
            if (std::abs(r) >= 1) continue;
            const double s = d == 0 ? 1 : std::sin(kTwoPi * fc * d) / (kTwoPi * fc * d);
            acc += x[k] * 2 * fc * s * bessel_i0(beta * std::sqrt(1 - r * r)) / i0b;
        }
        y[n] = static_cast<float>(acc);
    }
    return y;
}

Audio Engines::speak(const std::string & text, const std::filesystem::path & ref_wav, const std::string & ref_text,
                     uint32_t seed) {
    std::lock_guard<std::mutex> lk(tts_mu_);
    if (!tts_) {
        auto t = std::make_unique<Tts>();
        t->model = engine::models::voxcpm2::load_voxcpm2_model(d_.tts_model.u8string());
        rt::SessionOptions so;
        so.backend.type = backend_type(d_.gpu);
        so.backend.device = d_.gpu_device;
        t->session = t->model->create_task_session(rt::TaskSpec{rt::VoiceTaskKind::Tts, rt::RunMode::Offline}, so);
        t->offline = dynamic_cast<rt::IOfflineVoiceTaskSession *>(t->session.get());
        if (!t->offline) throw std::runtime_error("VoxCPM2 session is not offline-capable");
        tts_ = std::move(t);
    }
    const auto wav = engine::audio::read_wav_f32(ref_wav);
    std::vector<float> mono(wav.samples.size() / std::max(1, wav.channels));
    for (size_t i = 0; i < mono.size(); ++i) {
        float acc = 0;
        for (int c = 0; c < wav.channels; ++c) acc += wav.samples[i * wav.channels + c];
        mono[i] = acc / std::max(1, wav.channels);
    }
    constexpr int kVaeRate = 16000;   // VoxCPM2 AudioVAE input rate
    rt::AudioBuffer ref{kVaeRate, 1, resample(mono, wav.sample_rate, kVaeRate)};
    rt::TaskRequest req;
    req.text_input = rt::Transcript{text, ""};
    req.audio_input = ref;
    req.voice = rt::VoiceCondition{rt::VoiceReference{ref, std::nullopt}, std::nullopt};
    req.options["reference_text"] = ref_text;
    req.options["seed"] = std::to_string(seed);
    req.options["guidance_scale"] = "2.0";
    req.options["num_inference_steps"] = "25";   // 10 (default) leaves hissing "s" endings
    tts_->offline->prepare(rt::build_preparation_request(req));
    const auto result = tts_->offline->run(req);
    if (!result.audio_output) throw std::runtime_error("VoxCPM2 produced no audio");
    Audio a;
    a.sample_rate = result.audio_output->sample_rate;
    const int ch = std::max(1, result.audio_output->channels);
    const auto & s = result.audio_output->samples;
    a.samples.resize(s.size() / ch);
    for (size_t i = 0; i < a.samples.size(); ++i) {
        float acc = 0;
        for (int c = 0; c < ch; ++c) acc += s[i * ch + c];
        a.samples[i] = acc / ch;
    }
    return a;
}

// ---------------------------------------------------------------- local LLM (presets in llm_presets.h, thinking off)

struct Engines::Llm {
    std::string preset;   // the model loaded (one at a time: VRAM)
    llama_model * model = nullptr;
    llama_context * ctx = nullptr;
    const llama_vocab * vocab = nullptr;
    int n_ctx = 32768;
    ~Llm() {
        if (ctx) llama_free(ctx);
        if (model) llama_model_free(model);
    }
};

std::string Engines::chat(const json & messages, const json & schema, int max_tokens, float temperature,
                          const std::string & preset_id) {
    std::lock_guard<std::mutex> lk(llm_mu_);
    const LlmPreset * preset = find_llm_preset(preset_id);
    if (!preset) throw std::runtime_error("unknown local model " + preset_id);
    const std::filesystem::path path = llm_path(d_, *preset);
    if (llm_ && llm_->preset != preset->id) llm_.reset();   // another model: free the GPU first
    if (!llm_) {
        auto l = std::make_unique<Llm>();
        static std::once_flag once;
        std::call_once(once, [] {
            llama_log_set([](ggml_log_level lvl, const char * msg, void *) {
                if (lvl >= GGML_LOG_LEVEL_WARN) std::fputs(msg, stderr);
            }, nullptr);
            llama_backend_init();
        });
        auto mp = llama_model_default_params();
        mp.n_gpu_layers = d_.gpu == "cpu" ? 0 : 99;
        mp.split_mode = LLAMA_SPLIT_MODE_NONE;
        mp.main_gpu = d_.gpu_device;
        l->model = llama_model_load_from_file(path.u8string().c_str(), mp);
        if (!l->model) throw std::runtime_error("cannot load " + preset->title + " from " + path.u8string());
        l->preset = preset->id;
        l->vocab = llama_model_get_vocab(l->model);
        auto cp = llama_context_default_params();
        cp.n_ctx = l->n_ctx;
        cp.n_batch = 2048;
        l->ctx = llama_init_from_model(l->model, cp);
        if (!l->ctx) throw std::runtime_error("cannot create director LLM context");
        llm_ = std::move(l);
    }
    // each family's chat template as its GGUF renders it with enable_thinking = false
    std::string prompt;
    if (preset->family == "gemma4") {   // <|turn>role ... <turn|>; the assistant is "model"; empty thought channel
        for (const auto & m : messages) {
            const std::string role = m["role"] == "assistant" ? "model" : m["role"].get<std::string>();
            prompt += "<|turn>" + role + "\n" + m["content"].get<std::string>() + "<turn|>\n";
        }
        prompt += "<|turn>model\n<|channel>thought\n<channel|>";
    } else {   // qwen35: ChatML
        for (const auto & m : messages)
            prompt += "<|im_start|>" + m["role"].get<std::string>() + "\n" + m["content"].get<std::string>() + "<|im_end|>\n";
        prompt += "<|im_start|>assistant\n<think>\n\n</think>\n\n";
    }

    std::vector<llama_token> toks(prompt.size() + 16);
    const int n = llama_tokenize(llm_->vocab, prompt.data(), static_cast<int32_t>(prompt.size()), toks.data(),
                                 static_cast<int32_t>(toks.size()), true, true);
    if (n < 0) throw std::runtime_error("tokenize failed");
    toks.resize(n);
    if (n + max_tokens > llm_->n_ctx) throw std::runtime_error("chapter too long for the director context");
    llama_memory_clear(llama_get_memory(llm_->ctx), true);

    llama_sampler * grammar = nullptr;
    if (!schema.is_null()) {
        const std::string gbnf = json_schema_to_grammar(nlohmann::ordered_json::parse(schema.dump()));
        grammar = llama_sampler_init_grammar(llm_->vocab, gbnf.c_str(), "root");
        if (!grammar) throw std::runtime_error("grammar init failed");
    }
    // llama-server's default chain at the requested temperature
    llama_sampler * chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(temperature));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(1234));
    struct Free {
        llama_sampler * a, * b;
        ~Free() {
            if (a) llama_sampler_free(a);
            if (b) llama_sampler_free(b);
        }
    } guard{grammar, chain};

    for (int i = 0; i < n; i += 2048) {
        const int k = std::min(2048, n - i);
        if (llama_decode(llm_->ctx, llama_batch_get_one(toks.data() + i, k))) throw std::runtime_error("prompt decode failed");
    }
    const int n_vocab = llama_vocab_n_tokens(llm_->vocab);
    std::vector<llama_token_data> cand(n_vocab);
    std::string out;
    for (int step = 0; step < max_tokens; ++step) {
        const float * logits = llama_get_logits_ith(llm_->ctx, -1);
        auto sample = [&](bool with_grammar) {
            for (llama_token t = 0; t < n_vocab; ++t) cand[t] = {t, logits[t], 0.f};
            llama_token_data_array arr{cand.data(), cand.size(), -1, false};
            if (with_grammar) llama_sampler_apply(grammar, &arr);
            llama_sampler_apply(chain, &arr);
            return arr.data[arr.selected].id;
        };
        llama_token t = sample(false);
        if (grammar) {   // grammar checked on the sampled token first, as in llama.cpp's common_sampler
            llama_token_data one{t, 1.0f, 0.f};
            llama_token_data_array a1{&one, 1, -1, false};
            llama_sampler_apply(grammar, &a1);
            if (std::isinf(one.logit)) t = sample(true);
            llama_sampler_accept(grammar, t);
        }
        llama_sampler_accept(chain, t);
        if (llama_vocab_is_eog(llm_->vocab, t)) break;
        char buf[256];
        const int k = llama_token_to_piece(llm_->vocab, t, buf, sizeof buf, 0, false);
        if (k > 0) out.append(buf, k);
        if (llama_decode(llm_->ctx, llama_batch_get_one(&t, 1))) throw std::runtime_error("decode failed");
    }
    return out;
}

// ---------------------------------------------------------------- ModernBookNLP

struct Engines::Bnlp {
    booknlp::Backend backend;
    booknlp::Pipeline pipeline;
    Bnlp(const Deploy & d) : backend(d.gpu, d.gpu_device), pipeline(d.booknlp_models.u8string(), backend) {}
};

booknlp::Result Engines::booknlp(const std::string & chapter_text) {
    std::lock_guard<std::mutex> lk(bnlp_mu_);
    if (!bnlp_) bnlp_ = std::make_unique<Bnlp>(d_);
    return bnlp_->pipeline.run(chapter_text);
}

// ---------------------------------------------------------------- misc

Engines::Engines(const Deploy & d) : d_(d) {}
Engines::~Engines() = default;

bool Engines::tts_available() const { return std::filesystem::exists(d_.tts_model); }
bool Engines::llm_available(const std::string & preset_id) const {
    const LlmPreset * p = find_llm_preset(preset_id);
    return p && std::filesystem::exists(llm_path(d_, *p));
}
bool Engines::booknlp_available() const {
    for (const char * f : {"bnlp-entities.gguf", "bnlp-quote.gguf", "bnlp-coref.gguf"})
        if (!std::filesystem::exists(d_.booknlp_models / f)) return false;
    return true;
}

}  // namespace rm
