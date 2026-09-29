#include "voices.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace rm {

namespace fs = std::filesystem;

static std::string read_file(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.rfind("\xEF\xBB\xBF", 0) == 0) s = s.substr(3);
    return s;
}

std::string display_name(const std::string & reader) {
    std::string out;
    for (size_t i = 0; i < reader.size(); ++i) {
        if (i > 0 && std::islower(static_cast<unsigned char>(reader[i - 1])) && std::isupper(static_cast<unsigned char>(reader[i])))
            out += ' ';
        out += reader[i];
    }
    const size_t a = out.find_first_not_of(" \t"), b = out.find_last_not_of(" \t");
    return a == std::string::npos ? "" : out.substr(a, b - a + 1);
}

std::string voice_label(const json & v) {
    static const std::map<std::string, std::string> word = {{"low", "low"}, {"mid", "medium"}, {"high", "high"}};
    std::string name = display_name(v.value("reader", ""));
    if (name.empty()) name = "Voice " + v["id"].get<std::string>();
    std::string pitch;
    auto it = word.find(v.value("band", ""));
    if (it != word.end()) pitch = ", " + it->second + " pitch";
    return name + " (" + v["gender"].get<std::string>() + pitch + ")";
}

Pool::Pool(const std::vector<fs::path> & dirs, const fs::path & overrides) : overrides_path_(overrides) {
    if (!dirs.empty()) first_dir_ = dirs.front();
    for (const auto & dir : dirs) {
        if (!fs::exists(dir / "pool.json")) continue;
        for (auto & v : json::parse(read_file(dir / "pool.json")).get<std::vector<json>>()) {
            const std::string id = v["id"].get<std::string>();
            if (dir_of_.count(id)) continue;
            dir_of_[id] = dir;
            voices.push_back(std::move(v));
        }
    }
    overrides_ = fs::exists(overrides) ? json::parse(read_file(overrides)) : json::object();
    for (auto & v : voices) {
        const json o = overrides_.value(v["id"].get<std::string>(), json::object());
        v["metadata_gender"] = v["gender"];
        const std::string g = o.value("gender", "");
        if (g == "male" || g == "female") v["gender"] = g;
        v["banned"] = o.value("banned", false);
    }
    // pitch bands are relative within a gender: recompute after gender flips are applied
    for (const char * g : {"male", "female"}) {
        std::vector<json *> vs;
        for (auto & v : voices)
            if (v["gender"] == g) vs.push_back(&v);
        std::stable_sort(vs.begin(), vs.end(), [](const json * a, const json * b) {
            const double fa = a->value("f0", json(0.0)).is_null() ? 0.0 : a->value("f0", 0.0);
            const double fb = b->value("f0", json(0.0)).is_null() ? 0.0 : b->value("f0", 0.0);
            return fa < fb;
        });
        for (size_t r = 0; r < vs.size(); ++r) {
            const double frac = static_cast<double>(r + 1) / vs.size();
            (*vs[r])["band"] = frac <= 1.0 / 3 ? "low" : (frac <= 2.0 / 3 ? "mid" : "high");
        }
    }
    std::map<std::string, int> seen;
    for (auto & v : voices) {
        v["label"] = voice_label(v);
        seen[v["label"].get<std::string>()] += 1;
    }
    for (auto & v : voices)   // same reader name twice: add the id to tell them apart
        if (seen[v["label"].get<std::string>()] > 1) v["label"] = v["label"].get<std::string>() + " \xC2\xB7 " + v["id"].get<std::string>();
}

std::vector<const json *> Pool::usable(const std::optional<std::string> & gender) const {
    std::vector<const json *> out;
    for (const auto & v : voices) {
        const bool qa_ok = !v.contains("qa") || v["qa"].value("ok", true);
        if (qa_ok && !v["banned"].get<bool>() && (!gender || v["gender"] == *gender)) out.push_back(&v);
    }
    return out;
}

const json * Pool::get(const std::string & id) const {
    for (const auto & v : voices)
        if (v["id"] == id) return &v;
    return nullptr;
}

std::pair<fs::path, std::string> Pool::ref(const std::string & id) const {
    const fs::path & dir = dir_of_.at(id);
    return {dir / (id + ".wav"), read_file(dir / (id + ".txt"))};
}

bool Pool::bundled(const std::string & id) const {
    auto it = dir_of_.find(id);
    return it != dir_of_.end() && it->second == first_dir_;
}

std::set<std::string> removed_voices(const fs::path & overrides) {
    std::set<std::string> out;
    if (!fs::exists(overrides)) return out;
    const json o = json::parse(read_file(overrides));
    for (auto it = o.begin(); it != o.end(); ++it)
        if (it.value().is_object() && it.value().value("banned", false)) out.insert(it.key());
    return out;
}

void set_voice_removed(const fs::path & overrides, const std::string & id, bool removed) {
    json o = fs::exists(overrides) ? json::parse(read_file(overrides)) : json::object();
    json & e = o[id];
    if (!e.is_object()) e = json::object();
    if (removed) e["banned"] = true;
    else e.erase("banned");
    if (e.empty()) o.erase(id);
    fs::create_directories(overrides.parent_path());
    std::ofstream(overrides, std::ios::binary) << o.dump(1);
}

fs::path Pool::sample(const std::string & id) const {
    const fs::path & dir = dir_of_.at(id);
    return fs::exists(dir / "samples" / (id + ".wav")) ? dir / "samples" / (id + ".wav") : dir / (id + ".wav");
}

void Pool::set_override(const std::string & id, const std::optional<std::string> & gender,
                        const std::optional<bool> & banned, bool reset_gender) {
    json & o = overrides_[id];
    if (!o.is_object()) o = json::object();
    if (reset_gender) o.erase("gender");
    else if (gender) o["gender"] = *gender;
    if (banned) o["banned"] = *banned;
    if (o.empty()) overrides_.erase(id);
    fs::create_directories(overrides_path_.parent_path());
    std::ofstream(overrides_path_, std::ios::binary) << overrides_.dump(1);
}

std::string pitch_from_director(const json & entry) {
    const std::string age = entry.value("age", "adult");
    std::string hint = entry.contains("voice") && entry["voice"].is_string() ? entry["voice"].get<std::string>() : "";
    std::transform(hint.begin(), hint.end(), hint.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (age == "child" || age == "teen") return "high";
    if (age == "elder") return "low";
    static const std::regex low(R"(\b(deep|bass|baritone|low|gravel\w*|rumbl\w*|husky|older|elderly|old|mature)\b)");
    static const std::regex high(R"(\b(high|squeak\w*|bright|teen\w*|young|boy|girl|child|soprano|tenor|light|nasal)\b)");
    const auto lo = std::distance(std::sregex_iterator(hint.begin(), hint.end(), low), std::sregex_iterator());
    const auto hi = std::distance(std::sregex_iterator(hint.begin(), hint.end(), high), std::sregex_iterator());
    return lo > hi ? "low" : (hi > lo ? "high" : "all");
}

static double f0_of(const json & v) { return v.contains("f0") && v["f0"].is_number() ? v["f0"].get<double>() : 0.0; }

std::string default_narrator(const Pool & pool, const std::string & gender) {
    auto c = pool.usable(gender);
    if (c.empty()) c = pool.usable();
    if (c.empty()) return "";
    std::stable_sort(c.begin(), c.end(), [](const json * a, const json * b) { return f0_of(*a) < f0_of(*b); });
    return (*c[c.size() / 2])["id"].get<std::string>();
}

std::map<std::string, std::string> assign(const Pool & pool, const std::vector<json> & cast, const std::string & narrator,
                                          const std::vector<std::pair<std::string, json>> & fresh_in,
                                          const std::map<std::string, int> & counts) {
    std::map<std::string, double> f0;
    for (const auto & v : pool.voices) f0[v["id"].get<std::string>()] = f0_of(v);
    std::set<std::string> used{narrator};
    std::map<std::string, std::vector<std::string>> by_gender{{"male", {}}, {"female", {}}};
    for (const auto & c : cast) {
        used.insert(c["voice"].get<std::string>());
        const std::string g = c["gender"].get<std::string>();
        if (by_gender.count(g)) by_gender[g].push_back(c["voice"].get<std::string>());
    }
    auto order = fresh_in;
    auto cnt = [&](const std::string & n) {
        auto it = counts.find(n);
        return it == counts.end() ? 0 : it->second;
    };
    std::stable_sort(order.begin(), order.end(), [&](const auto & a, const auto & b) { return cnt(a.first) > cnt(b.first); });
    std::map<std::string, std::string> picks;
    const json * nv = pool.get(narrator);
    for (const auto & [name, e] : order) {
        const std::string g = e.value("gender", "") == "female" ? "female" : "male";
        std::string b = e.contains("pitch") && e["pitch"].is_string() && !e["pitch"].get<std::string>().empty()
                            ? e["pitch"].get<std::string>()
                            : pitch_from_director(e);
        const auto cands = pool.usable(g);
        std::vector<const json *> pick_from;
        for (auto * v : cands)
            if (!used.count((*v)["id"].get<std::string>())) pick_from.push_back(v);
        if (pick_from.empty())
            for (auto * v : cands)
                if ((*v)["id"] != narrator) pick_from.push_back(v);
        if (pick_from.empty()) pick_from = cands;
        if (pick_from.empty()) continue;
        std::vector<std::string> taken = by_gender[g];
        if (nv && (*nv)["gender"] == g) taken.push_back(narrator);
        auto score = [&](const json * v) {
            double dist = 100;
            bool any = false;
            for (const auto & t : taken) {
                auto it = f0.find(t);
                if (it == f0.end()) continue;
                const double d = std::fabs(f0_of(*v) - it->second);
                dist = any ? std::min(dist, d) : d;
                any = true;
            }
            return ((*v)["band"] == b ? 25.0 : 0.0) + dist;
        };
        const json * best = pick_from.front();
        double bs = score(best);
        for (auto * v : pick_from) {   // max(): first of equal scores wins
            const double s = score(v);
            if (s > bs) {
                bs = s;
                best = v;
            }
        }
        const std::string id = (*best)["id"].get<std::string>();
        picks[name] = id;
        used.insert(id);
        by_gender[g].push_back(id);
    }
    return picks;
}

}  // namespace rm
