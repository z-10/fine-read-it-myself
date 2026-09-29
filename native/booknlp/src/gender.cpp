#include "gender.h"

#include "frontend.h"
#include "ordered.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

namespace rm::booknlp {
namespace {

constexpr int kDistance = 25, kEpochs = 25;
constexpr double kUpper = 10;

std::vector<std::string> split(const std::string & s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (size_t b; (b = s.find(sep, a)) != std::string::npos; a = b + 1) out.push_back(s.substr(a, b - a));
    out.push_back(s.substr(a));
    return out;
}

std::string rstrip(const std::string & s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return s.substr(0, e);
}

double round3(double x) {   // float("%.3f" % x)
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", x);
    return std::strtod(buf, nullptr);
}

std::pair<std::string, std::string> prop_cat(const std::string & cat) {
    const size_t us = cat.find('_');
    return {cat.substr(0, us), us == std::string::npos ? "" : cat.substr(us + 1)};
}

const std::unordered_set<std::string> kHonorifics = {
    "mr.", "mrs.", "miss", "lady", "sir", "captain", "mr", "lord", "aunt", "madame", "mrs", "uncle", "colonel",
    "monsieur", "mademoiselle", "general", "major", "sergeant", "ms.", "king", "queen", "herr", "frau", "fräulein",
    "dame", "mister", "master", "mistress", "prince", "princess", "lieutenant"};

using Vec = std::vector<double>;

}  // namespace

const std::vector<std::vector<std::string>> & gender_categories() {
    static const std::vector<std::vector<std::string>> g = {
        {"he", "him", "his"}, {"she", "her"}, {"they", "them", "their"}, {"xe", "xem", "xyr", "xir"}, {"ze", "zem", "zir", "hir"}};
    return g;
}

GenderPriors::GenderPriors(const std::string & terms_tsv) {
    const auto & cats = gender_categories();
    const int G = static_cast<int>(cats.size());
    std::istringstream in(terms_tsv);
    std::string line;
    std::getline(in, line);
    std::vector<std::pair<int, int>> mapping;   // gender id -> column
    const auto header = split(rstrip(line), '\t');
    for (size_t c = 2; c < header.size(); ++c)
        for (int g = 0; g < G; ++g) {
            std::string id;
            for (size_t k = 0; k < cats[g].size(); ++k) id += (k ? "/" : "") + cats[g][k];
            if (header[c] == id) mapping.emplace_back(g, static_cast<int>(c));
        }
    while (std::getline(in, line)) {
        const auto cols = split(rstrip(line), '\t');
        Vec vals(G, 0.0);
        for (const auto & [g, c] : mapping) vals[g] = std::strtod(cols[c].c_str(), nullptr);
        const std::string first = split(cols[0], ' ')[0];
        if (kHonorifics.count(first)) {
            auto & h = honorific[first];
            if (h.empty()) h.assign(G, 0.0);
            for (int i = 0; i < G; ++i) h[i] += vals[i];
        }
        double total = 0;
        for (double v : vals) total += v;
        if (total >= kUpper) {
            for (auto & v : vals) v = (v / total) * kUpper;
            const std::string key = py_lower(cols[0] + "\t" + cols[1]);
            if (!hyper.count(key)) order.push_back(key);
            hyper[key] = vals;
        }
    }
    for (auto & [_, h] : honorific) {
        double total = 0;
        for (double v : h) total += v;
        if (total >= kUpper)
            for (auto & v : h) v = (v / total) * kUpper;
    }
}

std::map<int, GenderInfo> infer_genders(const GenderPriors & priors, const std::vector<Token> & tokens,
                                        const std::vector<Entity> & entities, const std::vector<int> & refs) {
    const auto & cats = gender_categories();
    const int G = static_cast<int>(cats.size());
    std::unordered_map<std::string, int> pronoun;
    for (int g = 0; g < G; ++g)
        for (const auto & t : cats[g]) pronoun[t] = g;

    auto entity_key = [&](size_t idx, bool use_refs) {
        const Entity & e = entities[idx];
        const auto [prop, cat] = prop_cat(e.cat);
        if (use_refs && refs[idx] != -1) return std::to_string(refs[idx]) + "\tCOREF";
        if (prop == "NOM") return tokens[nom_head(e.start, e.end, tokens)].text + "\t" + prop;
        return e.text + "\t" + prop;
    };

    // vocab: hyperparameter terms, then PER mention keys
    std::unordered_map<std::string, int> vocab;
    std::vector<std::string> terms;
    auto add = [&](const std::string & k) {
        if (vocab.emplace(k, static_cast<int>(terms.size())).second) terms.push_back(k);
    };
    for (const auto & k : priors.order) add(k);
    for (size_t i = 0; i < entities.size(); ++i)
        if (prop_cat(entities[i].cat).second == "PER") add(py_lower(entity_key(i, true)));
    const size_t V = terms.size();
    std::vector<Vec> joint(V, Vec(G, 0.0)), t_f_e(V, Vec(G, 0.0));
    Vec e_counts(V, 0.0);

    // per coref id: Counter of mention keys (all entities with a ref)
    OrderedMap<int, OrderedMap<std::string, int>> counts;
    for (size_t i = 0; i < entities.size(); ++i)
        if (refs[i] != -1) counts[refs[i]][py_lower(entity_key(i, false))] += 1;

    auto add_hyperparameters = [&]() {
        for (size_t e = 0; e < V; ++e) {
            auto it = priors.hyper.find(terms[e]);
            for (int f = 0; f < G; ++f) {
                const double mf = it != priors.hyper.end() ? it->second[f] : 1.0;
                joint[e][f] = mf + 0.1;
                e_counts[e] += mf + 0.1;
            }
        }
        for (size_t e = 0; e < V; ++e) {
            const auto parts = split(terms[e], '\t');
            if (parts.size() < 2 || parts[1] != "coref") continue;
            const int idd = std::atoi(parts[0].c_str());
            if (idd == -1) continue;
            Vec mf(G, 0.0);
            const auto & c = counts.at(idd);
            for (const auto & [text, n] : c) {
                auto v = vocab.find(text);
                if (v == vocab.end()) continue;
                for (int f = 0; f < G; ++f) mf[f] = joint[v->second][f] * n;
            }
            double s = 0;
            for (double x : mf) s += x;
            if (s > kUpper)
                for (int i = 0; i < G; ++i) {   // sum(mf) is re-evaluated after each assignment
                    double cur = 0;
                    for (double x : mf) cur += x;
                    mf[i] = (mf[i] / cur) * kUpper;
                }
            for (const auto & [text, n] : c) {
                auto h = priors.honorific.find(split(text, ' ')[0]);
                if (h == priors.honorific.end()) continue;
                for (int f = 0; f < G; ++f) mf[f] = h->second[f] * n * 10000;
            }
            e_counts[e] = 0;
            for (int f = 0; f < G; ++f) {
                joint[e][f] = mf[f] + 0.1;
                e_counts[e] += mf[f] + 0.1;
            }
        }
    };
    auto maximization = [&]() {
        for (size_t e = 0; e < V; ++e)
            for (int f = 0; f < G; ++f) t_f_e[e][f] = e_counts[e] > 0 ? joint[e][f] / e_counts[e] : 0.0;
    };
    auto delete_counts = [&]() {
        for (size_t e = 0; e < V; ++e) {
            std::fill(joint[e].begin(), joint[e].end(), 0.0);
            e_counts[e] = 0;
        }
    };
    add_hyperparameters();
    maximization();
    delete_counts();

    // process(): tagged pronouns and the PER mentions ending before them within 25 tokens
    std::map<int, std::vector<std::pair<int, std::string>>> loc_starts;   // start -> (end, key)
    std::vector<int> tagged;
    for (size_t i = 0; i < entities.size(); ++i) {
        const Entity & e = entities[i];
        if (prop_cat(e.cat).second != "PER") continue;
        if (pronoun.count(py_lower(e.text))) tagged.push_back(e.start);
        loc_starts[e.start].emplace_back(e.end, entity_key(i, true));
    }
    std::vector<std::vector<int>> X;
    std::vector<int> Y;
    for (int idx : tagged) {
        std::vector<int> ms;
        for (int d = 1; d < kDistance; ++d) {
            auto it = loc_starts.find(idx - d);
            if (it == loc_starts.end()) continue;
            for (const auto & [end, key] : it->second)
                if (end < idx) ms.push_back(vocab.at(py_lower(key)));
        }
        if (!ms.empty()) {
            X.push_back(ms);
            Y.push_back(pronoun.at(py_lower(tokens[idx].text)));
        }
    }
    for (int epoch = 0; epoch < kEpochs; ++epoch) {
        for (size_t k = 0; k < X.size(); ++k) {
            const int f = Y[k];
            double total = 0;
            for (int e : X[k]) total += t_f_e[e][f];
            for (int e : X[k]) {
                const double d = t_f_e[e][f] / total;
                joint[e][f] += d;
                e_counts[e] += d;
            }
        }
        maximization();
        if (epoch < kEpochs - 1) {
            delete_counts();
            add_hyperparameters();
        }
    }
    std::map<int, GenderInfo> genders;
    for (size_t e = 0; e < V; ++e) {
        const auto parts = split(terms[e], '\t');
        if (parts.size() < 2 || parts[1] != "coref") continue;
        GenderInfo gi;
        double total = 0;
        for (int i = 0; i < G; ++i) total += joint[e][i];
        gi.inference = joint[e];
        if (total > 0)
            for (auto & v : gi.inference) v = round3(v / total);
        for (int i = 0; i < G; ++i)
            if (gi.inference[i] > gi.max) {
                gi.max = gi.inference[i];
                gi.argmax = i;
            }
        gi.total = round3(total);
        genders[std::atoi(parts[0].c_str())] = gi;
    }
    return genders;
}

void update_gender_from_coref(std::map<int, GenderInfo> & genders, const std::vector<Entity> & entities,
                              const std::vector<int> & corefs) {
    const auto & cats = gender_categories();
    const int G = static_cast<int>(cats.size());
    OrderedMap<int, OrderedMap<int, int>> counts;   // coref -> category -> n
    for (size_t i = 0; i < entities.size(); ++i) {
        auto & c = counts[corefs[i]];
        const std::string t = py_lower(entities[i].text);
        for (int g = 0; g < G; ++g)
            for (const auto & term : cats[g])
                if (t == term) c[g] += 1;
    }
    for (const auto & [c, cc] : counts) {
        if (genders.count(c)) continue;
        int total = 0, maxv = 0, maxg = -1;
        for (const auto & [g, n] : cc) {
            total += n;
            if (n > maxv) {
                maxv = n;
                maxg = g;
            }
        }
        if (total == 0) continue;
        GenderInfo gi;
        gi.inference.assign(G, 0.0);
        for (const auto & [g, n] : cc) gi.inference[g] = static_cast<double>(n) / total;
        gi.argmax = maxg;
        gi.max = static_cast<double>(maxv) / total;
        gi.total = total;
        genders[c] = gi;
    }
}

}  // namespace rm::booknlp
