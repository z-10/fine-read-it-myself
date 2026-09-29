// rm-bnlp: ModernBookNLP C++ port driver.
//
//   rm-bnlp tokens chapter.txt out.json     spaCy-compatible tokens: [[paragraph_id, text, char_offset], ...]
//   rm-bnlp bert model.gguf in.json out.bin [backend]
//                                         in.json {"words": [...], "last_k": K} -> wordpiece ids + last K layers (f32)
//   rm-bnlp entities chapter.txt models_dir out.json [backend]
//                                         {"tokens": [[paragraph, sentence, text, idx], ...], "entities": [[s, e, cat, text], ...]}
//   rm-bnlp run chapter.txt models_dir out.json [backend]
//                                         full pipeline, every intermediate (same layout as the Python dumps)
//   rm-bnlp attrib chapter.txt models_dir out.json [backend]
//                                         + "quotes": [[s, e], ...], "attributions": [entity index or null, ...]
#include "bert.h"
#include "entity_tagger.h"
#include "frontend.h"
#include "quote_attrib.h"
#include "quotes.h"
#include "pipeline.h"

#include <chrono>
#include "tokenizer.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

using namespace rm::booknlp;

static std::string read_text(const std::string & path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str(), out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {  // Python universal newlines
        if (s[i] == '\r') {
            out += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

int main(int argc, char ** argv) {
    if (argc < 4) {
        std::cerr << "usage: rm-bnlp tokens chapter.txt out.json\n";
        return 2;
    }
    try {
        const std::string cmd = argv[1];
        if (cmd == "bert") {
            Backend be(argc > 5 ? argv[5] : "");
            GgufModel m(argv[2], be);
            Bert bert(m);
            const auto in = nlohmann::json::parse(read_text(argv[3]));
            std::vector<int> ids{bert.vocab().id("[CLS]")};
            for (const auto & w : in["words"])
                for (const auto & t : bert.vocab().tokenize(w.get<std::string>())) ids.push_back(bert.vocab().id(t));
            ids.push_back(bert.vocab().id("[SEP]"));
            const auto layers = bert.encode(ids, in["last_k"].get<int>());
            std::ofstream f(argv[4], std::ios::binary);
            const int32_t n = static_cast<int32_t>(ids.size());
            f.write(reinterpret_cast<const char *>(&n), 4);
            f.write(reinterpret_cast<const char *>(ids.data()), 4 * n);
            for (const auto & l : layers) f.write(reinterpret_cast<const char *>(l.data()), 4 * l.size());
            std::cout << be.name() << ": " << n << " wordpieces\n";
            return 0;
        }
        if (cmd == "run") {
            Backend be(argc > 5 ? argv[5] : "");
            const auto t0 = std::chrono::steady_clock::now();
            Pipeline pipe(argv[3], be);
            const double load = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const Result r = pipe.run(read_text(argv[2]));
            const auto & cats = gender_categories();
            auto cat_name = [&](int g) {
                std::string n;
                for (size_t k = 0; k < cats[g].size(); ++k) n += (k ? "/" : "") + cats[g][k];
                return n;
            };
            auto gender_json = [&](const GenderInfo & gi) {
                nlohmann::json inf = nlohmann::json::object();
                for (size_t g = 0; g < cats.size(); ++g) inf[cat_name(static_cast<int>(g))] = gi.inference[g];
                return nlohmann::json{{"inference", inf},
                                      {"argmax", gi.argmax < 0 ? nlohmann::json(nullptr) : nlohmann::json(cat_name(gi.argmax))},
                                      {"max", gi.max}, {"total", gi.total}};
            };
            nlohmann::json out;
            out["tokens"] = nlohmann::json::array();
            for (const auto & t : r.tokens) out["tokens"].push_back({t.paragraph_id, t.sentence_id, t.text, t.idx});
            out["entities"] = nlohmann::json::array();
            for (const auto & e : r.entities) out["entities"].push_back({e.start, e.end, e.cat, e.text});
            out["quotes"] = nlohmann::json::array();
            for (const auto & [a, b] : r.quotes) out["quotes"].push_back({a, b});
            out["attributions"] = nlohmann::json::array();
            for (int a : r.attributions) out["attributions"].push_back(a < 0 ? nlohmann::json(nullptr) : nlohmann::json(a));
            out["refs"] = r.refs;
            out["assignments"] = r.assignments;
            out["genders_em"] = nlohmann::json::object();
            for (const auto & [id, gi] : r.genders_em) out["genders_em"][std::to_string(id)] = gender_json(gi);
            out["genders"] = nlohmann::json::object();
            for (const auto & [id, gi] : r.genders) out["genders"][std::to_string(id)] = gender_json(gi);
            out["characters"] = nlohmann::json::array();
            for (const auto & c : r.characters) {
                nlohmann::json pr = nlohmann::json::array();
                for (const auto & [n, k] : c.proper) pr.push_back({{"c", k}, {"n", n}});
                out["characters"].push_back({{"id", c.id}, {"proper", pr}, {"g", c.g ? gender_json(*c.g) : nlohmann::json(nullptr)}});
            }
            nlohmann::json rq = nlohmann::json::array(), rg = nlohmann::json::object();
            for (const auto & [q, w] : r.speaker_quotes) rq.push_back({q, w});
            for (const auto & [n, g] : r.speaker_genders) rg[n] = g;
            out["result"] = {{"quotes", rq}, {"genders", rg}};
            out["seconds"] = r.seconds;
            std::ofstream(argv[4], std::ios::binary) << out.dump();
            std::cout << be.name() << ": load " << load << " s";
            for (const auto & [k, v] : r.seconds) std::cout << ", " << k << " " << v << " s";
            std::cout << "\n";
            return 0;
        }
        if (cmd == "entities" || cmd == "attrib") {
            Backend be(argc > 5 ? argv[5] : "");
            const std::string dir = argv[3];
            const auto t0 = std::chrono::steady_clock::now();
            GgufModel m(dir + "/bnlp-entities.gguf", be);
            EntityTagger tagger(m);
            auto tokens = make_tokens(read_text(argv[2]));
            const auto t1 = std::chrono::steady_clock::now();
            const auto ents = tagger.tag(tokens);
            const auto t2 = std::chrono::steady_clock::now();
            nlohmann::json quotes_j = nlohmann::json::array(), attrib_j = nlohmann::json::array();
            double attrib_s = 0;
            if (cmd == "attrib") {
                const auto quotes = tag_quotes(tokens);
                GgufModel qm(dir + "/bnlp-quote.gguf", be);
                QuoteAttribution qa(qm);
                const auto t3 = std::chrono::steady_clock::now();
                const auto att = qa.tag(quotes, ents, tokens);
                attrib_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t3).count();
                for (const auto & [a, b] : quotes) quotes_j.push_back({a, b});
                for (int a : att) attrib_j.push_back(a < 0 ? nlohmann::json(nullptr) : nlohmann::json(a));
            }
            nlohmann::json out{{"tokens", nlohmann::json::array()}, {"entities", nlohmann::json::array()}};
            for (const auto & t : tokens) out["tokens"].push_back({t.paragraph_id, t.sentence_id, t.text, t.idx});
            for (const auto & e : ents) out["entities"].push_back({e.start, e.end, e.cat, e.text});
            out["quotes"] = quotes_j;
            out["attributions"] = attrib_j;
            std::ofstream(argv[4], std::ios::binary) << out.dump();
            const auto sec = [](auto d) { return std::chrono::duration<double>(d).count(); };
            std::cout << be.name() << ": " << tokens.size() << " tokens, " << ents.size() << " entities, load "
                      << sec(t1 - t0) << " s, tag " << sec(t2 - t1) << " s, attrib " << attrib_s << " s\n";
            return 0;
        }
        const auto text = utf8_to_u32(read_text(argv[2]));
        if (cmd == "tokens") {
            nlohmann::json out = nlohmann::json::array();
            int paragraph = 0;
            std::u32string ws;
            for (const auto & t : tokenize(text)) {
                if (t.space) {
                    ws += t.text;
                    continue;
                }
                if (ws.find(U"\n\n") != std::u32string::npos) ++paragraph;
                ws.clear();
                out.push_back({paragraph, u32_to_utf8(t.text), t.idx});
            }
            std::ofstream(argv[3], std::ios::binary) << out.dump();
        } else {
            throw std::runtime_error("unknown command " + cmd);
        }
        return 0;
    } catch (const std::exception & e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
