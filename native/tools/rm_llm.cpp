// rm-llm: greedy, JSON-schema-constrained generation with the director LLM (Qwen3.5 GGUF).
//
//   rm-llm --model Qwen3.5-4B-Q4_K_M.gguf --prompt prompt.txt [--schema schema.json] --out out.txt
//          [--ngl 99] [--ctx 16384] [--max-tokens 4000]
//
// The prompt file is the already-templated text (ChatML); special tokens in it are parsed.
#include "json-schema-to-grammar.h"
#include <nlohmann/json.hpp>
#include "llama.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char **argv) {
    std::map<std::string, std::string> a{{"--ngl", "99"}, {"--ctx", "16384"}, {"--max-tokens", "4000"}};
    for (int i = 1; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    for (const char *k : {"--model", "--prompt", "--out"})
        if (!a.count(k)) { std::cerr << "missing " << k << "\n"; return 2; }
    static const bool verbose = a.count("--verbose") && a["--verbose"] == "1";
    llama_log_set([](ggml_log_level lvl, const char *msg, void *) {
        if (verbose || lvl >= GGML_LOG_LEVEL_WARN) std::fputs(msg, stderr);
    }, nullptr);
    try {
        const auto t0 = std::chrono::steady_clock::now();
        llama_backend_init();
        auto mp = llama_model_default_params();
        mp.n_gpu_layers = std::stoi(a["--ngl"]);
        llama_model *model = llama_model_load_from_file(a["--model"].c_str(), mp);
        if (!model) throw std::runtime_error("cannot load model");
        const llama_vocab *vocab = llama_model_get_vocab(model);
        auto cp = llama_context_default_params();
        cp.n_ctx = std::stoi(a["--ctx"]);
        cp.n_batch = cp.n_ctx;
        llama_context *ctx = llama_init_from_model(model, cp);
        if (!ctx) throw std::runtime_error("cannot create context");

        const std::string prompt = slurp(a["--prompt"]);
        std::vector<llama_token> toks(prompt.size() + 16);
        const int n = llama_tokenize(vocab, prompt.data(), (int32_t) prompt.size(), toks.data(),
                                     (int32_t) toks.size(), true, true);
        if (n < 0) throw std::runtime_error("tokenize failed");
        toks.resize(n);

        llama_sampler *grammar = nullptr;
        if (a.count("--schema")) {
            const std::string gbnf = json_schema_to_grammar(nlohmann::ordered_json::parse(slurp(a["--schema"])));
            grammar = llama_sampler_init_grammar(vocab, gbnf.c_str(), "root");
            if (!grammar) throw std::runtime_error("grammar init failed");
        }
        const int n_vocab = llama_vocab_n_tokens(vocab);
        std::vector<llama_token_data> cand(n_vocab);
        // greedy; the grammar is checked on the argmax only and applied to the whole vocab when it
        // rejects that token (same result as grammar-then-greedy, as in llama.cpp common_sampler)
        auto sample = [&]() -> llama_token {
            const float *logits = llama_get_logits_ith(ctx, -1);
            llama_token best = 0;
            for (llama_token i = 1; i < n_vocab; ++i) if (logits[i] > logits[best]) best = i;
            if (!grammar) return best;
            llama_token_data one{best, logits[best], 0.f};
            llama_token_data_array arr1{&one, 1, -1, false};
            llama_sampler_apply(grammar, &arr1);
            if (one.logit != -INFINITY) return best;
            for (llama_token i = 0; i < n_vocab; ++i) cand[i] = {i, logits[i], 0.f};
            llama_token_data_array arr{cand.data(), cand.size(), -1, false};
            llama_sampler_apply(grammar, &arr);
            best = 0;
            for (llama_token i = 1; i < n_vocab; ++i) if (cand[i].logit > cand[best].logit) best = i;
            if (cand[best].logit == -INFINITY) throw std::runtime_error("grammar allows no token");
            return best;
        };
        const auto t1 = std::chrono::steady_clock::now();

        if (llama_decode(ctx, llama_batch_get_one(toks.data(), (int32_t) toks.size())))
            throw std::runtime_error("prompt decode failed");
        const auto t2 = std::chrono::steady_clock::now();

        std::string out;
        int n_gen = 0;
        for (const int max = std::stoi(a["--max-tokens"]); n_gen < max; ++n_gen) {
            llama_token t = sample();
            if (grammar) llama_sampler_accept(grammar, t);
            if (llama_vocab_is_eog(vocab, t)) break;
            char buf[256];
            const int k = llama_token_to_piece(vocab, t, buf, sizeof buf, 0, false);
            if (k > 0) out.append(buf, k);
            if (llama_decode(ctx, llama_batch_get_one(&t, 1))) throw std::runtime_error("decode failed");
        }
        const auto t3 = std::chrono::steady_clock::now();
        std::ofstream(a["--out"], std::ios::binary) << out;

        const auto s = [](auto d) { return std::chrono::duration<double>(d).count(); };
        std::printf("load %.1f s | prompt %d tok %.1f s (%.0f tok/s) | gen %d tok %.1f s (%.1f tok/s)\n",
                    s(t1 - t0), n, s(t2 - t1), n / s(t2 - t1), n_gen, s(t3 - t2), n_gen / s(t3 - t2));
        if (grammar) llama_sampler_free(grammar);
        llama_free(ctx);
        llama_model_free(model);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
