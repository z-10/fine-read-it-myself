// rm-tts: render one line with VoxCPM2 ultimate clone (reference clip as prompt audio + transcript).
//
//   rm-tts --model voxcpm2-q8_0.gguf --ref voice.wav --ref-text "transcript of voice.wav"
//          --text "Line to speak." --out out.wav [--backend vulkan|metal|cpu] [--device 0] [--seed 1234]
//   --lines lines.txt (UTF-8, one text per line) instead of --text: renders out_<n>.wav for each, model loaded once
#include "engine/framework/audio/wav_reader.h"
#include "engine/framework/runtime/session.h"
#include "engine/models/voxcpm2/loader.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <iterator>

namespace rt = engine::runtime;

static void write_wav16(const std::string &path, const rt::AudioBuffer &a) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    const uint32_t n = static_cast<uint32_t>(a.samples.size()), sr = static_cast<uint32_t>(a.sample_rate);
    const uint16_t ch = static_cast<uint16_t>(a.channels), bits = 16;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    f.write("RIFF", 4); u32(36 + n * 2); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(ch); u32(sr);
    u32(sr * ch * bits / 8); u16(static_cast<uint16_t>(ch * bits / 8)); u16(bits); f.write("data", 4); u32(n * 2);
    for (float s : a.samples) {
        const float c = s > 1.f ? 1.f : (s < -1.f ? -1.f : s);
        const int16_t v = static_cast<int16_t>(c * 32767.f);
        f.write(reinterpret_cast<const char *>(&v), 2);
    }
}

int main(int argc, char **argv) {
    std::map<std::string, std::string> a{{"--backend", "vulkan"}, {"--device", "0"}, {"--seed", "1234"}};
    for (int i = 1; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    if (a.count("--lines")) {
        std::ifstream in(a["--lines"], std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), line;
        a["--text"] = "";
        size_t n = 0;
        for (size_t i = 0; i <= all.size(); ++i) {
            if (i < all.size() && all[i] != '\n') { line += all[i]; continue; }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) a["--line" + std::to_string(n++)] = line;
            line.clear();
        }
    }
    for (const char *k : {"--model", "--ref", "--ref-text", "--text", "--out"})
        if (!a.count(k)) { std::cerr << "missing " << k << "\n"; return 2; }
    try {
        const auto t0 = std::chrono::steady_clock::now();
        auto model = engine::models::voxcpm2::load_voxcpm2_model(a["--model"]);

        rt::SessionOptions so;
        const std::string be = a["--backend"];
        so.backend.type = be == "cpu" ? engine::core::BackendType::Cpu
                        : be == "metal" ? engine::core::BackendType::Metal : engine::core::BackendType::Vulkan;
        so.backend.device = std::stoi(a["--device"]);
        auto session = model->create_task_session(rt::TaskSpec{rt::VoiceTaskKind::Tts, rt::RunMode::Offline}, so);
        auto *offline = dynamic_cast<rt::IOfflineVoiceTaskSession *>(session.get());
        if (!offline) throw std::runtime_error("VoxCPM2 session is not offline-capable");

        const auto wav = engine::audio::read_wav_f32(std::filesystem::path(a["--ref"]));
        rt::AudioBuffer ref{wav.sample_rate, wav.channels, wav.samples};
        std::vector<std::pair<std::string, std::string>> jobs;   // text, out path
        if (a.count("--lines")) {
            const std::string stem = a["--out"].substr(0, a["--out"].rfind('.'));
            for (int n = 0; a.count("--line" + std::to_string(n)); ++n)
                jobs.push_back({a["--line" + std::to_string(n)], stem + "_" + std::to_string(n) + ".wav"});
        } else jobs.push_back({a["--text"], a["--out"]});
        for (const auto & [text, out] : jobs) {
        const auto t1 = std::chrono::steady_clock::now();
        rt::TaskRequest req;
        req.text_input = rt::Transcript{text, ""};
        req.audio_input = ref;                                        // prompt audio (ultimate clone)
        req.voice = rt::VoiceCondition{rt::VoiceReference{ref, std::nullopt}, std::nullopt};
        req.options["reference_text"] = a["--ref-text"];
        req.options["seed"] = a["--seed"];
        req.options["guidance_scale"] = "2.0";
        if (a.count("--opts"))   // extra engine options: "key=value,key=value"
            for (size_t p = 0; p < a["--opts"].size();) {
                size_t e = a["--opts"].find(',', p);
                if (e == std::string::npos) e = a["--opts"].size();
                const std::string kv = a["--opts"].substr(p, e - p);
                const size_t eq = kv.find('=');
                if (eq != std::string::npos) req.options[kv.substr(0, eq)] = kv.substr(eq + 1);
                p = e + 1;
            }
        offline->prepare(rt::build_preparation_request(req));
        const auto result = offline->run(req);
        const auto t2 = std::chrono::steady_clock::now();
        if (!result.audio_output) throw std::runtime_error("no audio produced");
        write_wav16(out, *result.audio_output);

        const double secs = double(result.audio_output->samples.size()) /
                            (result.audio_output->sample_rate * result.audio_output->channels);
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::printf("load %.0f ms | render %.0f ms | audio %.2f s | %d Hz -> %s\n",
                    ms(t1 - t0), ms(t2 - t1), secs, result.audio_output->sample_rate, out.c_str());
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
