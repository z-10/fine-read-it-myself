#include "names.h"

#include <functional>
#include <set>
#include <sstream>
#include <unordered_set>

namespace rm::booknlp {
namespace {

std::vector<std::string> split(const std::string & s, char sep) {   // Python str.split(sep)
    std::vector<std::string> out;
    size_t a = 0;
    for (size_t b; (b = s.find(sep, a)) != std::string::npos; a = b + 1) out.push_back(s.substr(a, b - a));
    out.push_back(s.substr(a));
    return out;
}

std::string join(const std::vector<std::string> & v, const std::string & sep = " ") {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

std::string rstrip(const std::string & s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return s.substr(0, e);
}

std::pair<std::string, std::string> prop_cat(const std::string & cat) {
    const size_t us = cat.find('_');
    return {cat.substr(0, us), us == std::string::npos ? "" : cat.substr(us + 1)};
}

const std::unordered_set<std::string> kHonorifics = {"mr", "mr.", "mrs", "mrs.", "miss", "uncle", "aunt", "lady",
                                                     "lord", "monsieur", "master", "mistress"};

OrderedMap<std::string, int> get_variants(const std::vector<std::string> & parts) {
    OrderedMap<std::string, int> variants;
    const int n = static_cast<int>(parts.size());
    std::vector<int> idx;
    std::function<void(int)> rec = [&](int last) {   // nested loops of depth 2..7, depth first
        for (int k = last + 1; k < n; ++k) {
            idx.push_back(k);
            std::vector<std::string> w;
            for (int i : idx) w.push_back(parts[i]);
            variants[join(w)] = 1;
            if (idx.size() < 7) rec(k);
            idx.pop_back();
        }
    };
    for (int i = 0; i < n; ++i) {
        if (!kHonorifics.count(py_lower(parts[i]))) variants[parts[i]] = 1;
        idx = {i};
        rec(i);
    }
    return variants;
}

}  // namespace

NameCoref::NameCoref(const std::string & aliases_tsv) {
    std::istringstream in(aliases_tsv);
    for (std::string line; std::getline(in, line);) {
        const auto cols = split(rstrip(line), '\t');
        const std::string canonical = py_lower(cols[0]);
        for (size_t i = 1; i < cols.size(); ++i) aliases_[py_lower(cols[i])][canonical] = 1;
    }
}

NameCoref::Names NameCoref::get_canonical(const std::vector<std::string> & name_tokens) const {
    const std::string name = py_lower(join(name_tokens));
    Names out;
    if (const auto * a = aliases_.find(name)) {
        for (const auto & [can, _] : *a) out.push_back(split(can, ' '));
        return out;
    }
    std::vector<std::vector<std::string>> parts;
    for (const auto & tok : name_tokens) {
        std::vector<std::string> p;
        if (const auto * a = aliases_.find(py_lower(tok)))
            for (const auto & [can, _] : *a) p.push_back(can);
        else
            p.push_back(tok);
        parts.push_back(p);
    }
    // itertools.product
    out.push_back({});
    for (const auto & p : parts) {
        Names next;
        for (const auto & prefix : out)
            for (const auto & choice : p) {
                auto v = prefix;
                v.push_back(choice);
                next.push_back(v);
            }
        out.swap(next);
    }
    return out;
}

std::vector<int> NameCoref::name_cluster(const Names & entities, const std::vector<int> & is_named,
                                         const std::vector<int> & existing) const {
    OrderedMap<std::string, int> uniq;
    for (size_t i = 0; i < is_named.size(); ++i)
        if (is_named[i] == 1 && entities[i].size() < 10) {
            const std::string name = py_lower(join(entities[i]));
            if (!name.empty()) uniq[name] += 1;
        }
    // names that are subsets of others
    std::unordered_set<std::string> subsets;
    for (const auto & [name1, _] : uniq)
        for (const auto & c1 : get_canonical(split(name1, ' '))) {
            const std::set<std::string> s1(c1.begin(), c1.end());
            for (const auto & [name2, __] : uniq) {
                if (name1 == name2) continue;
                for (const auto & c2 : get_canonical(split(name2, ' '))) {
                    const std::set<std::string> s2(c2.begin(), c2.end());
                    if (s1 == s2) continue;
                    if (std::includes(s1.begin(), s1.end(), s2.begin(), s2.end())) subsets.insert(name2);
                }
            }
        }
    OrderedMap<std::string, OrderedMap<std::string, int>> subpart;
    for (const auto & [name, _] : uniq) {
        if (subsets.count(name)) continue;
        for (const auto & c : get_canonical(split(name, ' ')))
            for (const auto & [v, __] : get_variants(c)) subpart[v][name] = 1;
    }
    OrderedMap<std::string, int> charids, last_seen;
    int max_id = 1;
    if (!existing.empty()) max_id = *std::max_element(existing.begin(), existing.end()) + 1;
    std::vector<int> refs;
    for (size_t i = 0; i < is_named.size(); ++i) {
        if (existing[i] != -1) {
            refs.push_back(existing[i]);
            continue;
        }
        if (is_named[i] != 1) {
            refs.push_back(-1);
            continue;
        }
        const std::string * top = nullptr;
        int max_score = 0;
        for (const auto & c : get_canonical(entities[i])) {
            const auto * cands = subpart.find(py_lower(join(c)));
            if (!cands) continue;
            for (const auto & [entity, _] : *cands) {
                int score = uniq.at(entity);
                if (const int * ls = last_seen.find(entity)) score += *ls;
                if (score > max_score) {
                    max_score = score;
                    top = &entity;
                }
            }
        }
        if (top) {
            last_seen[*top] = static_cast<int>(i);
            if (!charids.contains(*top)) charids[*top] = max_id++;
            refs.push_back(charids.at(*top));
        } else {
            refs.push_back(-1);
        }
    }
    return refs;
}

std::vector<int> NameCoref::cluster(const Names & entities, const std::vector<int> & is_named,
                                    const std::vector<int> & existing) const {
    std::vector<int> refs = name_cluster(entities, is_named, existing);
    OrderedMap<int, OrderedMap<std::string, int>> clusters;
    std::unordered_set<int> dead;   // clusters[ref] = None
    for (size_t i = 0; i < refs.size(); ++i) clusters[refs[i]][join(entities[i])] += 1;
    std::vector<int> keys;
    for (const auto & [k, _] : clusters) keys.push_back(k);
    auto total = [](const OrderedMap<std::string, int> & c) {
        int s = 0;
        for (const auto & [_, v] : c) s += v;
        return s;
    };
    for (int ref : keys)
        for (int ref2 : keys) {
            if (ref == ref2 || dead.count(ref) || dead.count(ref2) || ref == -1 || ref2 == -1 || ref == 0 || ref2 == 0)
                continue;
            int big = ref, small = ref2;
            if (total(clusters[ref2]) > total(clusters[ref])) std::swap(big, small);
            auto & cs = clusters[small];
            auto & cb = clusters[big];
            int overlap = 0;
            for (const auto & [name, v] : cs)
                if (cb.contains(name)) overlap += v;
            const double sim = static_cast<double>(overlap) / total(cs);
            if (sim > 0.9) {
                for (const auto & [k, v] : most_common(cs)) cb[k] += v;
                for (int & r : refs)
                    if (r == small) r = big;
                dead.insert(small);
            }
        }
    return refs;
}

std::vector<int> NameCoref::cluster_narrator(const std::vector<Entity> & entities, const std::vector<int> & in_quotes) const {
    static const std::unordered_set<std::string> kNarrator = {"i", "me", "my", "myself"};
    std::vector<int> refs;
    for (size_t i = 0; i < entities.size(); ++i)
        refs.push_back(in_quotes[i] == 0 && kNarrator.count(py_lower(entities[i].text)) ? 0 : -1);
    return refs;
}

std::vector<int> NameCoref::cluster_identical_propers(const std::vector<Entity> & entities, std::vector<int> refs) const {
    int max_id = 1;
    if (!refs.empty()) max_id = *std::max_element(refs.begin(), refs.end()) + 1;
    std::unordered_map<std::string, int> names;
    for (size_t i = 0; i < entities.size(); ++i) {
        const auto [prop, cat] = prop_cat(entities[i].cat);
        if (prop == "PROP" && cat != "PER") {
            const std::string key = py_lower(entities[i].text) + "_" + prop + "_" + cat;
            auto it = names.find(key);
            if (it == names.end()) it = names.emplace(key, max_id++).first;
            refs[i] = it->second;
        }
    }
    return refs;
}

std::vector<int> NameCoref::cluster_only_nouns(const std::vector<Entity> & entities, std::vector<int> refs,
                                               const std::vector<Token> & tokens) const {
    static const std::unordered_map<std::string, std::string> kHon = {
        {"mister", "mr."}, {"mr.", "mr."}, {"mr", "mr."}, {"mistah", "mr."}, {"mastah", "mr."}, {"master", "mr."},
        {"miss", "miss"}, {"ms.", "miss"}, {"ms", "miss"}, {"missus", "miss"}, {"mistress", "miss"},
        {"mrs.", "mrs."}, {"mrs", "mrs."}};
    std::vector<int> is_named;
    Names names;
    for (const auto & e : entities) {
        const auto [prop, cat] = prop_cat(e.cat);
        is_named.push_back(prop == "PROP" && cat == "PER" ? 1 : 0);
        std::vector<std::string> nt;
        for (int i = e.start; i <= e.end; ++i) {
            const std::string & w = tokens[i].text;
            auto hon = kHon.find(py_lower(w));
            const bool nounish = hon != kHon.end() || tokens[i].pos == "NOUN" || tokens[i].pos == "PROPN";
            if (nounish && first_is_upper(w)) nt.push_back(hon != kHon.end() ? hon->second : w);
        }
        names.push_back(nt.empty() ? split(e.text, ' ') : nt);
    }
    return cluster(names, is_named, refs);
}

}  // namespace rm::booknlp
